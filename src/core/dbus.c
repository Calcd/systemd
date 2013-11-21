/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <errno.h>
#include <unistd.h>

#include "sd-bus.h"
#include "log.h"
#include "strv.h"
#include "mkdir.h"
#include "missing.h"
#include "dbus-unit.h"
#include "dbus-job.h"
#include "dbus-manager.h"
#include "dbus-execute.h"
#include "dbus-kill.h"
#include "dbus-cgroup.h"
#include "special.h"
#include "dbus.h"
#include "bus-util.h"
#include "bus-error.h"
#include "bus-errors.h"
#include "strxcpyx.h"
#include "dbus-client-track.h"
#include "bus-internal.h"
#include "selinux-access.h"

#define CONNECTIONS_MAX 512

static void destroy_bus(Manager *m, sd_bus **bus);

int bus_send_queued_message(Manager *m) {
        int r;

        assert(m);

        if (!m->queued_message)
                return 0;

        assert(m->queued_message_bus);

        /* If we cannot get rid of this message we won't dispatch any
         * D-Bus messages, so that we won't end up wanting to queue
         * another message. */

        r = sd_bus_send(m->queued_message_bus, m->queued_message, NULL);
        if (r < 0)
                log_warning("Failed to send queued message: %s", strerror(-r));

        m->queued_message = sd_bus_message_unref(m->queued_message);
        m->queued_message_bus = sd_bus_unref(m->queued_message_bus);

        return 0;
}

static int signal_agent_released(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *cgroup;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &cgroup);
        if (r < 0) {
                bus_log_parse_error(r);
                return 0;
        }

        manager_notify_cgroup_empty(m, cgroup);

        if (m->running_as == SYSTEMD_SYSTEM && m->system_bus) {
                /* If we are running as system manager, forward the
                 * message to the system bus */

                r = sd_bus_send(m->system_bus, message, NULL);
                if (r < 0)
                        log_warning("Failed to forward Released message: %s", strerror(-r));
        }

        return 0;
}

static int signal_disconnected(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;

        assert(bus);
        assert(message);
        assert(m);

        if (bus == m->api_bus)
                destroy_bus(m, &m->api_bus);
        if (bus == m->system_bus)
                destroy_bus(m, &m->system_bus);
        if (set_remove(m->private_buses, bus)) {
                log_debug("Got disconnect on private connection.");
                destroy_bus(m, &bus);
        }

        return 0;
}

static int signal_name_owner_changed(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        const char *name, *old_owner, *new_owner;
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner);
        if (r < 0) {
                bus_log_parse_error(r);
                return 0;
        }

        manager_dispatch_bus_name_owner_changed(
                        m, name,
                        isempty(old_owner) ? NULL : old_owner,
                        isempty(new_owner) ? NULL : new_owner);

        return 0;
}

static int signal_activation_request(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0) {
                bus_log_parse_error(r);
                return 0;
        }

        if (manager_unit_inactive_or_pending(m, SPECIAL_DBUS_SERVICE) ||
            manager_unit_inactive_or_pending(m, SPECIAL_DBUS_SOCKET)) {
                r = sd_bus_error_setf(&error, BUS_ERROR_SHUTTING_DOWN, "Refusing activation, D-Bus is shutting down.");
                goto failed;
        }

        r = manager_load_unit(m, name, NULL, &error, &u);
        if (r < 0)
                goto failed;

        if (u->refuse_manual_start) {
                r = sd_bus_error_setf(&error, BUS_ERROR_ONLY_BY_DEPENDENCY, "Operation refused, %u may be requested by dependency only.", u->id);
                goto failed;
        }

        r = manager_add_job(m, JOB_START, u, JOB_REPLACE, true, &error, NULL);
        if (r < 0)
                goto failed;

        /* Successfully queued, that's it for us */
        return 0;

failed:
        if (!sd_bus_error_is_set(&error))
                sd_bus_error_set_errno(&error, r);

        log_debug("D-Bus activation failed for %s: %s", name, bus_error_message(&error, r));

        r = sd_bus_message_new_signal(bus, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Activator", "ActivationFailure", &reply);
        if (r < 0) {
                bus_log_create_error(r);
                return 0;
        }

        r = sd_bus_message_append(reply, "sss", name, error.name, error.message);
        if (r < 0) {
                bus_log_create_error(r);
                return 0;
        }

        r = sd_bus_send_to(bus, reply, "org.freedesktop.DBus", NULL);
        if (r < 0) {
                log_error("Failed to respond with to bus activation request: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int selinux_filter(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *verb, *path;
        Unit *u = NULL;
        Job *j;
        int r;

        assert(bus);
        assert(message);

        /* Our own method calls are all protected individually with
         * selinux checks, but the built-in interfaces need to be
         * protected too. */

        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Set"))
                verb = "reload";
        else if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", NULL) ||
                 sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", NULL) ||
                 sd_bus_message_is_method_call(message, "org.freedesktop.DBus.ObjectManager", NULL) ||
                 sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Peer", NULL))
                verb = "status";
        else
                return 0;

        path = sd_bus_message_get_path(message);

        if (object_path_startswith("/org/freedesktop/systemd1", path)) {

                r = selinux_access_check(bus, message, verb, error);
                if (r < 0)
                        return r;

                return 0;
        }

        if (streq_ptr(path, "/org/freedesktop/systemd1/unit/self")) {
                pid_t pid;

                r = sd_bus_get_owner_pid(bus, sd_bus_message_get_sender(message), &pid);
                if (r < 0)
                        return 0;

                u = manager_get_unit_by_pid(m, pid);
        } else {
                r = manager_get_job_from_dbus_path(m, path, &j);
                if (r >= 0)
                        u = j->unit;
                else
                        manager_load_unit_from_dbus_path(m, path, NULL, &u);
        }

        if (!u)
                return 0;

        r = selinux_unit_access_check(u, bus, message, verb, error);
        if (r < 0)
                return r;

        return 0;
}

static int bus_job_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        Job *j;
        int r;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        r = manager_get_job_from_dbus_path(m, path, &j);
        if (r < 0)
                return 0;

        *found = j;
        return 1;
}

static Unit *find_unit(Manager *m, sd_bus *bus, const char *path) {
        Unit *u;
        int r;

        assert(m);
        assert(bus);
        assert(path);

        if (streq_ptr(path, "/org/freedesktop/systemd1/unit/self")) {
                sd_bus_message *message;
                pid_t pid;

                message = sd_bus_get_current(bus);
                if (!message)
                        return NULL;

                r = sd_bus_get_owner_pid(bus, sd_bus_message_get_sender(message), &pid);
                if (r < 0)
                        return NULL;

                u = manager_get_unit_by_pid(m, pid);
        } else {
                r = manager_load_unit_from_dbus_path(m, path, NULL, &u);
                if (r < 0)
                        return NULL;
        }

        return u;
}

static int bus_unit_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        Unit *u;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        u = find_unit(m, bus, path);
        if (!u)
                return 0;

        *found = u;
        return 1;
}

static int bus_unit_interface_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        Unit *u;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        u = find_unit(m, bus, path);
        if (!u)
                return 0;

        if (!streq_ptr(interface, UNIT_VTABLE(u)->bus_interface))
                return 0;

        *found = u;
        return 1;
}

static int bus_unit_cgroup_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        Unit *u;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        u = find_unit(m, bus, path);
        if (!u)
                return 0;

        if (!streq_ptr(interface, UNIT_VTABLE(u)->bus_interface))
                return 0;

        if (!unit_get_cgroup_context(u))
                return 0;

        *found = u;
        return 1;
}

static int bus_cgroup_context_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        CGroupContext *c;
        Unit *u;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        u = find_unit(m, bus, path);
        if (!u)
                return 0;

        if (!streq_ptr(interface, UNIT_VTABLE(u)->bus_interface))
                return 0;

        c = unit_get_cgroup_context(u);
        if (!c)
                return 0;

        *found = c;
        return 1;
}

static int bus_exec_context_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        ExecContext *c;
        Unit *u;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        u = find_unit(m, bus, path);
        if (!u)
                return 0;

        if (!streq_ptr(interface, UNIT_VTABLE(u)->bus_interface))
                return 0;

        c = unit_get_exec_context(u);
        if (!c)
                return 0;

        *found = c;
        return 1;
}

static int bus_kill_context_find(sd_bus *bus, const char *path, const char *interface, void **found, void *userdata) {
        Manager *m = userdata;
        KillContext *c;
        Unit *u;

        assert(bus);
        assert(path);
        assert(interface);
        assert(found);
        assert(m);

        u = find_unit(m, bus, path);
        if (!u)
                return 0;

        if (!streq_ptr(interface, UNIT_VTABLE(u)->bus_interface))
                return 0;

        c = unit_get_kill_context(u);
        if (!c)
                return 0;

        *found = c;
        return 1;
}

static int bus_job_enumerate(sd_bus *bus, const char *path, char ***nodes, void *userdata) {
        _cleanup_free_ char **l = NULL;
        Manager *m = userdata;
        unsigned k = 0;
        Iterator i;
        Job *j;

        l = new0(char*, hashmap_size(m->jobs)+1);
        if (!l)
                return -ENOMEM;

        HASHMAP_FOREACH(j, m->jobs, i) {
                l[k] = job_dbus_path(j);
                if (!l[k])
                        return -ENOMEM;

                k++;
        }

        assert(hashmap_size(m->jobs) == k);

        *nodes = l;
        l = NULL;

        return k;
}

static int bus_unit_enumerate(sd_bus *bus, const char *path, char ***nodes, void *userdata) {
        _cleanup_free_ char **l = NULL;
        Manager *m = userdata;
        unsigned k = 0;
        Iterator i;
        Unit *u;

        l = new0(char*, hashmap_size(m->units)+1);
        if (!l)
                return -ENOMEM;

        HASHMAP_FOREACH(u, m->units, i) {
                l[k] = unit_dbus_path(u);
                if (!l[k])
                        return -ENOMEM;

                k++;
        }

        *nodes = l;
        l = NULL;

        return k;
}

static int bus_setup_api_vtables(Manager *m, sd_bus *bus) {
        UnitType t;
        int r;

        assert(m);
        assert(bus);

        r = sd_bus_add_filter(bus, selinux_filter, m);
        if (r < 0) {
                log_error("Failed to add SELinux access filter: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_object_vtable(bus, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", bus_manager_vtable, m);
        if (r < 0) {
                log_error("Failed to register Manager vtable: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/job", "org.freedesktop.systemd1.Job", bus_job_vtable, bus_job_find, m);
        if (r < 0) {
                log_error("Failed to register Job vtable: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_node_enumerator(bus, "/org/freedesktop/systemd1/job", bus_job_enumerate, m);
        if (r < 0) {
                log_error("Failed to add job enumerator: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/unit", "org.freedesktop.systemd1.Unit", bus_unit_vtable, bus_unit_find, m);
        if (r < 0) {
                log_error("Failed to register Unit vtable: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_node_enumerator(bus, "/org/freedesktop/systemd1/unit", bus_unit_enumerate, m);
        if (r < 0) {
                log_error("Failed to add job enumerator: %s", strerror(-r));
                return r;
        }

        for (t = 0; t < _UNIT_TYPE_MAX; t++) {
                r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/unit", unit_vtable[t]->bus_interface, unit_vtable[t]->bus_vtable, bus_unit_interface_find, m);
                if (r < 0)  {
                        log_error("Failed to register type specific vtable for %s: %s", unit_vtable[t]->bus_interface, strerror(-r));
                        return r;
                }

                if (unit_vtable[t]->cgroup_context_offset > 0) {
                        r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/unit", unit_vtable[t]->bus_interface, bus_unit_cgroup_vtable, bus_unit_cgroup_find, m);
                        if (r < 0) {
                                log_error("Failed to register control group unit vtable for %s: %s", unit_vtable[t]->bus_interface, strerror(-r));
                                return r;
                        }

                        r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/unit", unit_vtable[t]->bus_interface, bus_cgroup_vtable, bus_cgroup_context_find, m);
                        if (r < 0) {
                                log_error("Failed to register control group vtable for %s: %s", unit_vtable[t]->bus_interface, strerror(-r));
                                return r;
                        }
                }

                if (unit_vtable[t]->exec_context_offset > 0) {
                        r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/unit", unit_vtable[t]->bus_interface, bus_exec_vtable, bus_exec_context_find, m);
                        if (r < 0) {
                                log_error("Failed to register execute vtable for %s: %s", unit_vtable[t]->bus_interface, strerror(-r));
                                return r;
                        }
                }

                if (unit_vtable[t]->kill_context_offset > 0) {
                        r = sd_bus_add_fallback_vtable(bus, "/org/freedesktop/systemd1/unit", unit_vtable[t]->bus_interface, bus_kill_vtable, bus_kill_context_find, m);
                        if (r < 0) {
                                log_error("Failed to register kill vtable for %s: %s", unit_vtable[t]->bus_interface, strerror(-r));
                                return r;
                        }
                }
        }

        return 0;
}

static int bus_setup_disconnected_match(Manager *m, sd_bus *bus) {
        int r;

        assert(m);
        assert(bus);

        r = sd_bus_add_match(
                        bus,
                        "type='signal',"
                        "path='/org/freedesktop/DBus/Local',"
                        "interface='org.freedesktop.DBus.Local',"
                        "member='Disconnected'",
                        signal_disconnected, m);

        if (r < 0) {
                log_error("Failed to register match for Disconnected message: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int bus_on_connection(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        _cleanup_close_ int nfd = -1;
        Manager *m = userdata;
        sd_id128_t id;
        int r;

        assert(s);
        assert(m);

        nfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK|SOCK_CLOEXEC);
        if (nfd < 0) {
                log_warning("Failed to accept private connection, ignoring: %m");
                return 0;
        }

        if (set_size(m->private_buses) >= CONNECTIONS_MAX) {
                log_warning("Too many concurrent connections, refusing");
                return 0;
        }

        r = set_ensure_allocated(&m->private_buses, trivial_hash_func, trivial_compare_func);
        if (r < 0) {
                log_oom();
                return 0;
        }

        r = sd_bus_new(&bus);
        if (r < 0) {
                log_warning("Failed to allocate new private connection bus: %s", strerror(-r));
                return 0;
        }

        r = sd_bus_set_fd(bus, nfd, nfd);
        if (r < 0) {
                log_warning("Failed to set fd on new connection bus: %s", strerror(-r));
                return 0;
        }

        nfd = -1;

        r = bus_check_peercred(bus);
        if (r < 0) {
                log_warning("Incoming private connection from unprivileged client, refusing: %s", strerror(-r));
                return 0;
        }

        assert_se(sd_id128_randomize(&id) >= 0);

        r = sd_bus_set_server(bus, 1, id);
        if (r < 0) {
                log_warning("Failed to enable server support for new connection bus: %s", strerror(-r));
                return 0;
        }

        r = sd_bus_start(bus);
        if (r < 0) {
                log_warning("Failed to start new connection bus: %s", strerror(-r));
                return 0;
        }

        r = sd_bus_attach_event(bus, m->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0) {
                log_warning("Failed to attach new connection bus to event loop: %s", strerror(-r));
                return 0;
        }

        if (m->running_as == SYSTEMD_SYSTEM) {
                /* When we run as system instance we get the Released
                 * signal via a direct connection */

                r = sd_bus_add_match(
                                bus,
                                "type='signal',"
                                "interface='org.freedesktop.systemd1.Agent',"
                                "member='Released',"
                                "path='/org/freedesktop/systemd1/agent'",
                                signal_agent_released, m);

                if (r < 0) {
                        log_warning("Failed to register Released match on new connection bus: %s", strerror(-r));
                        return 0;
                }
        }

        r = bus_setup_disconnected_match(m, bus);
        if (r < 0)
                return 0;

        r = bus_setup_api_vtables(m, bus);
        if (r < 0) {
                log_warning("Failed to set up API vtables on new connection bus: %s", strerror(-r));
                return 0;
        }

        r = set_put(m->private_buses, bus);
        if (r < 0) {
                log_warning("Failed to add new conenction bus to set: %s", strerror(-r));
                return 0;
        }

        bus = NULL;

        log_debug("Accepted new private connection.");

        return 0;
}

static int bus_list_names(Manager *m, sd_bus *bus) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        const char *name;
        int r;

        assert(m);
        assert(bus);

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.DBus",
                        "/org/freedesktop/DBus",
                        "org.freedesktop.DBus",
                        "ListNames",
                        &error, &reply,
                        NULL);
        if (r < 0) {
                log_error("Failed to get initial list of names: %s", bus_error_message(&error, r));
                return r;
        }

        r = sd_bus_message_enter_container(reply, 'a', "s");
        if (r < 0)
                return bus_log_parse_error(r);

        /* This is a bit hacky, we say the owner of the name is the
         * name itself, because we don't want the extra traffic to
         * figure out the real owner. */
        while ((r = sd_bus_message_read(reply, "s", &name)) > 0)
                manager_dispatch_bus_name_owner_changed(m, name, NULL, name);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        return 0;
}

static int bus_setup_api(Manager *m, sd_bus *bus) {
        int r;

        assert(m);
        assert(bus);

        r = bus_setup_api_vtables(m, bus);
        if (r < 0)
                return r;

        r = sd_bus_add_match(
                        bus,
                        "type='signal',"
                        "sender='org.freedesktop.DBus',"
                        "path='/org/freedesktop/DBus',"
                        "interface='org.freedesktop.DBus',"
                        "member='NameOwnerChanged'",
                        signal_name_owner_changed, m);
        if (r < 0)
                log_warning("Failed to subscribe to NameOwnerChanged signal: %s", strerror(-r));

        r = sd_bus_add_match(
                        bus,
                        "type='signal',"
                        "sender='org.freedesktop.DBus',"
                        "path='/org/freedesktop/DBus',"
                        "interface='org.freedesktop.systemd1.Activator',"
                        "member='ActivationRequest'",
                        signal_activation_request, m);
        if (r < 0)
                log_warning("Failed to subscribe to activation signal: %s", strerror(-r));

        /* Allow replacing of our name, to ease implementation of
         * reexecution, where we keep the old connection open until
         * after the new connection is set up and the name installed
         * to allow clients to synchronously wait for reexecution to
         * finish */
        r = sd_bus_request_name(bus,"org.freedesktop.systemd1", SD_BUS_NAME_ALLOW_REPLACEMENT|SD_BUS_NAME_REPLACE_EXISTING);
        if (r < 0) {
                log_error("Failed to register name: %s", strerror(-r));
                return r;
        }

        if (r != SD_BUS_NAME_PRIMARY_OWNER) {
                log_error("Failed to acquire name.");
                return -EEXIST;
        }

        bus_list_names(m, bus);

        log_debug("Successfully connected to API bus.");
        return 0;
}

static int bus_init_api(Manager *m) {
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        if (m->api_bus)
                return 0;

        /* The API and system bus is the same if we are running in system mode */
        if (m->running_as == SYSTEMD_SYSTEM && m->system_bus)
                bus = sd_bus_ref(m->system_bus);
        else {
                if (m->running_as == SYSTEMD_SYSTEM)
                        r = sd_bus_open_system(&bus);
                else
                        r = sd_bus_open_user(&bus);

                if (r < 0) {
                        log_debug("Failed to connect to API bus, retrying later...");
                        return 0;
                }

                r = sd_bus_attach_event(bus, m->event, SD_EVENT_PRIORITY_NORMAL);
                if (r < 0) {
                        log_error("Failed to attach API bus to event loop: %s", strerror(-r));
                        return 0;
                }

                r = bus_setup_disconnected_match(m, bus);
                if (r < 0)
                        return 0;
        }

        r = bus_setup_api(m, bus);
        if (r < 0) {
                log_error("Failed to set up API bus: %s", strerror(-r));
                return 0;
        }

        m->api_bus = bus;
        bus = NULL;

        return 0;
}

static int bus_setup_system(Manager *m, sd_bus *bus) {
        int r;

        assert(m);
        assert(bus);

        if (m->running_as == SYSTEMD_SYSTEM)
                return 0;

        /* If we are a user instance we get the Released message via
         * the system bus */
        r = sd_bus_add_match(
                        bus,
                        "type='signal',"
                        "interface='org.freedesktop.systemd1.Agent',"
                        "member='Released',"
                        "path='/org/freedesktop/systemd1/agent'",
                        signal_agent_released, m);

        if (r < 0)
                log_warning("Failed to register Released match on system bus: %s", strerror(-r));

        log_debug("Successfully connected to system bus.");
        return 0;
}

static int bus_init_system(Manager *m) {
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        if (m->system_bus)
                return 0;

        /* The API and system bus is the same if we are running in system mode */
        if (m->running_as == SYSTEMD_SYSTEM && m->api_bus) {
                m->system_bus = sd_bus_ref(m->api_bus);
                return 0;
        }

        r = sd_bus_open_system(&bus);
        if (r < 0) {
                log_debug("Failed to connect to system bus, retrying later...");
                return 0;
        }

        r = bus_setup_disconnected_match(m, bus);
        if (r < 0)
                return 0;

        r = sd_bus_attach_event(bus, m->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0) {
                log_error("Failed to attach system bus to event loop: %s", strerror(-r));
                return 0;
        }

        r = bus_setup_system(m, bus);
        if (r < 0) {
                log_error("Fauiled to set up system bus: %s", strerror(-r));
                return 0;
        }

        m->system_bus = bus;
        bus = NULL;

        return 0;
}

static int bus_init_private(Manager *m) {
        _cleanup_close_ int fd = -1;
        union sockaddr_union sa = {
                .un.sun_family = AF_UNIX
        };
        sd_event_source *s;
        socklen_t salen;
        int r;

        assert(m);

        if (m->private_listen_fd >= 0)
                return 0;

        if (m->running_as == SYSTEMD_SYSTEM) {

                /* We want the private bus only when running as init */
                if (getpid() != 1)
                        return 0;

                strcpy(sa.un.sun_path, "/run/systemd/private");
                salen = offsetof(union sockaddr_union, un.sun_path) + sizeof("/run/systemd/private") - 1;
        } else {
                size_t left = sizeof(sa.un.sun_path);
                char *p = sa.un.sun_path;
                const char *e;

                e = secure_getenv("XDG_RUNTIME_DIR");
                if (!e) {
                        log_error("Failed to determine XDG_RUNTIME_DIR");
                        return -EHOSTDOWN;
                }

                left = strpcpy(&p, left, e);
                left = strpcpy(&p, left, "/systemd/private");

                salen = sizeof(sa.un) - left;

                mkdir_parents_label(sa.un.sun_path, 0755);
        }

        unlink(sa.un.sun_path);

        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0) {
                log_error("Failed to allocate private socket: %m");
                return -errno;
        }

        r = bind(fd, &sa.sa, salen);
        if (r < 0) {
                log_error("Failed to bind private socket: %m");
                return -errno;
        }

        r = listen(fd, SOMAXCONN);
        if (r < 0) {
                log_error("Failed to make private socket listening: %m");
                return -errno;
        }

        r = sd_event_add_io(m->event, fd, EPOLLIN, bus_on_connection, m, &s);
        if (r < 0) {
                log_error("Failed to allocate event source: %s", strerror(-r));
                return r;
        }

        m->private_listen_fd = fd;
        m->private_listen_event_source = s;
        fd = -1;

        log_debug("Successfully created private D-Bus server.");

        return 0;
}

int bus_init(Manager *m, bool try_bus_connect) {
        int r;

        if (try_bus_connect) {
                r = bus_init_system(m);
                if (r < 0)
                        return r;

                r = bus_init_api(m);
                if (r < 0)
                        return r;
        }

        r = bus_init_private(m);
        if (r < 0)
                return r;

        return 0;
}

static void destroy_bus(Manager *m, sd_bus **bus) {
        Iterator i;
        Job *j;

        assert(m);
        assert(bus);

        if (!*bus)
                return;

        /* Get rid of tracked clients on this bus */
        bus_client_untrack_bus(m->subscribed, *bus);
        HASHMAP_FOREACH(j, m->jobs, i)
                bus_client_untrack_bus(j->subscribed, *bus);

        /* Get rid of queued message on this bus */
        if (m->queued_message_bus == *bus) {
                m->queued_message_bus = sd_bus_unref(m->queued_message_bus);

                if (m->queued_message)
                        m->queued_message = sd_bus_message_unref(m->queued_message);
        }

        /* Possibly flush unwritten data, but only if we are
         * unprivileged, since we don't want to sync here */
        if (m->running_as != SYSTEMD_SYSTEM)
                sd_bus_flush(*bus);

        /* And destroy the object */
        sd_bus_close(*bus);
        *bus = sd_bus_unref(*bus);
}

void bus_done(Manager *m) {
        sd_bus *b;

        assert(m);

        if (m->api_bus)
                destroy_bus(m, &m->api_bus);
        if (m->system_bus)
                destroy_bus(m, &m->system_bus);
        while ((b = set_steal_first(m->private_buses)))
                destroy_bus(m, &b);

        set_free(m->private_buses);
        set_free(m->subscribed);

        if (m->private_listen_event_source)
                m->private_listen_event_source = sd_event_source_unref(m->private_listen_event_source);

        if (m->private_listen_fd >= 0) {
                close_nointr_nofail(m->private_listen_fd);
                m->private_listen_fd = -1;
        }
}

int bus_fdset_add_all(Manager *m, FDSet *fds) {
        Iterator i;
        sd_bus *b;
        int fd;

        assert(m);
        assert(fds);

        /* When we are about to reexecute we add all D-Bus fds to the
         * set to pass over to the newly executed systemd. They won't
         * be used there however, except thatt they are closed at the
         * very end of deserialization, those making it possible for
         * clients to synchronously wait for systemd to reexec by
         * simply waiting for disconnection */

        if (m->api_bus) {
                fd = sd_bus_get_fd(m->api_bus);
                if (fd >= 0) {
                        fd = fdset_put_dup(fds, fd);
                        if (fd < 0)
                                return fd;
                }
        }

        SET_FOREACH(b, m->private_buses, i) {
                fd = sd_bus_get_fd(b);
                if (fd >= 0) {
                        fd = fdset_put_dup(fds, fd);
                        if (fd < 0)
                                return fd;
                }
        }

        /* We don't offer any APIs on the system bus (well, unless it
         * is the same as the API bus) hence we don't bother with it
         * here */

        return 0;
}

void bus_serialize(Manager *m, FILE *f) {
        assert(m);
        assert(f);

        bus_client_track_serialize(m, f, m->subscribed);
}

int bus_deserialize_item(Manager *m, const char *line) {
        assert(m);
        assert(line);

        return bus_client_track_deserialize_item(m, &m->subscribed, line);
}
