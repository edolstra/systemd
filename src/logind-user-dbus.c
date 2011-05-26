/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>

#include "logind.h"
#include "logind-user.h"
#include "dbus-common.h"

#define BUS_USER_INTERFACE \
        " <interface name=\"org.freedesktop.login1.User\">\n"           \
        "  <method name=\"Terminate\"/>\n"                              \
        "  <property name=\"UID\" type=\"u\" access=\"read\"/>\n"       \
        "  <property name=\"GID\" type=\"u\" access=\"read\"/>\n"       \
        "  <property name=\"Name\" type=\"s\" access=\"read\"/>\n"      \
        "  <property name=\"RuntimePath\" type=\"s\" access=\"read\"/>\n" \
        "  <property name=\"Service\" type=\"s\" access=\"read\"/>\n"   \
        "  <property name=\"ControlGroupPath\" type=\"s\" access=\"read\"/>\n" \
        "  <property name=\"Display\" type=\"(so)\" access=\"read\"/>\n" \
        "  <property name=\"State\" type=\"s\" access=\"read\"/>\n"     \
        "  <property name=\"Sessions\" type=\"a(so)\" access=\"read\"/>\n" \
        " </interface>\n"                                               \

#define INTROSPECTION                                                   \
        DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                       \
        "<node>\n"                                                      \
        BUS_USER_INTERFACE                                              \
        BUS_PROPERTIES_INTERFACE                                        \
        BUS_PEER_INTERFACE                                              \
        BUS_INTROSPECTABLE_INTERFACE                                    \
        "</node>\n"

#define INTERFACES_LIST                              \
        BUS_GENERIC_INTERFACES_LIST                  \
        "org.freedesktop.login1.User\0"

static int bus_user_append_display(DBusMessageIter *i, const char *property, void *data) {
        DBusMessageIter sub;
        User *u = data;
        const char *id, *path;
        char *p = NULL;

        assert(i);
        assert(property);
        assert(u);

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_STRUCT, NULL, &sub))
                return -ENOMEM;

        if (u->display) {
                id = u->display->id;
                path = p = session_bus_path(u->display);

                if (!p)
                        return -ENOMEM;
        } else {
                id = "";
                path = "/";
        }

        if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &id) ||
            !dbus_message_iter_append_basic(&sub, DBUS_TYPE_OBJECT_PATH, &path)) {
                free(p);
                return -ENOMEM;
        }

        free(p);

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

static int bus_user_append_state(DBusMessageIter *i, const char *property, void *data) {
        User *u = data;
        const char *state;

        assert(i);
        assert(property);
        assert(u);

        state = user_state_to_string(user_get_state(u));

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &state))
                return -ENOMEM;

        return 0;
}

static int bus_user_append_sessions(DBusMessageIter *i, const char *property, void *data) {
        DBusMessageIter sub, sub2;
        User *u = data;
        Session *session;

        assert(i);
        assert(property);
        assert(u);

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "so", &sub))
                return -ENOMEM;

        LIST_FOREACH(sessions_by_user, session, u->sessions) {
                char *p;

                if (!dbus_message_iter_open_container(&sub, DBUS_TYPE_STRUCT, NULL, &sub2))
                        return -ENOMEM;

                p = session_bus_path(session);
                if (!p)
                        return -ENOMEM;

                if (!dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &session->id) ||
                    !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_OBJECT_PATH, &p)) {
                        free(p);
                        return -ENOMEM;
                }

                free(p);

                if (!dbus_message_iter_close_container(&sub, &sub2))
                        return -ENOMEM;
        }

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

static int get_user_for_path(Manager *m, const char *path, User **_u) {
        User *u;
        unsigned long lu;
        int r;

        assert(m);
        assert(path);
        assert(_u);

        if (!startswith(path, "/org/freedesktop/login1/user/"))
                return -EINVAL;

        r = safe_atolu(path + 29, &lu);
        if (r < 0)
                return r;

        u = hashmap_get(m->users, ULONG_TO_PTR(lu));
        if (!u)
                return -ENOENT;

        *_u = u;
        return 0;
}

static DBusHandlerResult user_message_dispatch(
                User *u,
                DBusConnection *connection,
                DBusMessage *message) {

        const BusProperty properties[] = {
                { "org.freedesktop.login1.User", "UID",              bus_property_append_uid,    "u",     &u->uid         },
                { "org.freedesktop.login1.User", "GID",              bus_property_append_gid,    "u",     &u->gid         },
                { "org.freedesktop.login1.User", "Name",             bus_property_append_string, "s",     u->name         },
                { "org.freedesktop.login1.User", "RuntimePath",      bus_property_append_string, "s",     u->runtime_path },
                { "org.freedesktop.login1.User", "ControlGroupPath", bus_property_append_string, "s",     u->cgroup_path  },
                { "org.freedesktop.login1.User", "Service",          bus_property_append_string, "s",     u->service      },
                { "org.freedesktop.login1.User", "Display",          bus_user_append_display,    "(so)",  u               },
                { "org.freedesktop.login1.User", "State",            bus_user_append_state,      "s",     u               },
                { "org.freedesktop.login1.User", "Sessions",         bus_user_append_sessions,   "a(so)", u               },
                { NULL, NULL, NULL, NULL, NULL }
        };

        assert(u);
        assert(connection);
        assert(message);

        return bus_default_message_handler(connection, message, INTROSPECTION, INTERFACES_LIST, properties);
}

static DBusHandlerResult user_message_handler(
                DBusConnection *connection,
                DBusMessage *message,
                void *userdata) {

        Manager *m = userdata;
        User *u;
        int r;

        r = get_user_for_path(m, dbus_message_get_path(message), &u);
        if (r < 0) {

                if (r == -ENOMEM)
                        return DBUS_HANDLER_RESULT_NEED_MEMORY;

                if (r == -ENOENT) {
                        DBusError e;

                        dbus_error_init(&e);
                        dbus_set_error_const(&e, DBUS_ERROR_UNKNOWN_OBJECT, "Unknown user");
                        return bus_send_error_reply(connection, message, &e, r);
                }

                return bus_send_error_reply(connection, message, NULL, r);
        }

        return user_message_dispatch(u, connection, message);
}

const DBusObjectPathVTable bus_user_vtable = {
        .message_function = user_message_handler
};

char *user_bus_path(User *u) {
        char *s;

        assert(u);

        if (asprintf(&s, "/org/freedesktop/login1/user/%llu", (unsigned long long) u->uid) < 0)
                return NULL;

        return s;
}
