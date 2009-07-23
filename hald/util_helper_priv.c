/***************************************************************************
 *
 * util_helper.c - HAL utilities for helper to check privileges 
 * (as e.g. prober/addons) et al. 
 *
 * Copyright (C) 2008 Danny Kukawka, <danny.kukawka@web,de>
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util_helper_priv.h"

gboolean
check_priv (LibHalContext *halctx, DBusConnection *connection, DBusMessage *message, const char *udi, const char *action)
#ifdef HAVE_POLKIT
{
        gboolean ret;
        char *polkit_result;
        const char *invoked_by_syscon_name;
        DBusMessage *reply;
        DBusError error;

        ret = FALSE;
        polkit_result = NULL;

        invoked_by_syscon_name = dbus_message_get_sender (message);
        
        dbus_error_init (&error);
        polkit_result = libhal_device_is_caller_privileged (halctx,
                                                            udi,
                                                            action,
                                                            invoked_by_syscon_name,
                                                            &error);
        if (polkit_result == NULL) {
                reply = dbus_message_new_error_printf (message,
                                                       "org.freedesktop.Hal.Device.Error",
                                                       "Cannot determine if caller is privileged",
                                                       action, polkit_result);
                dbus_connection_send (connection, reply, NULL);
		dbus_error_free (&error);
                goto out;
        }
        if (strcmp (polkit_result, "yes") != 0) {

                reply = dbus_message_new_error_printf (message,
                                                       "org.freedesktop.Hal.Device.PermissionDeniedByPolicy",
                                                       "%s %s <-- (action, result)",
                                                       action, polkit_result);
                dbus_connection_send (connection, reply, NULL);
                goto out;
        }

        ret = TRUE;

out:
        if (polkit_result != NULL)
                libhal_free_string (polkit_result);
        return ret;
}
#else
{
        return TRUE;
}
#endif
