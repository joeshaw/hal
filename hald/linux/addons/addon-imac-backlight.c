/*
 * Apple iMac Backlight control
 *
 * Copyright (C) 2007 Martin Szulecki <opensuse@sukimashita.com>
 * Copyright (C) 2006 Nicolas Boichat <nicolas@boichat.ch>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../../logger.h"
#include "../../util_helper_priv.h"

static LibHalContext *halctx = NULL;

static void
backlight_set(int value)
{
	outb(0x04 | (value << 4), 0xB3);
	outb(0xBF, 0xB2);
}

static int
backlight_get(void)
{
	outb(0x03, 0xB3);
	outb(0xBF, 0xB2);
	return inb(0xB3) >> 4;
}

#define BACKLIGHT_OBJECT \
  "/org/freedesktop/Hal/devices/imac_backlight"
#define BACKLIGHT_IFACE \
  "org.freedesktop.Hal.Device.LaptopPanel"
#define INTERFACE_DESCRIPTION \
  "    <method name=\"SetBrightness\">\n" \
  "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n" \
  "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n" \
  "    </method>\n" \
  "    <method name=\"GetBrightness\">\n" \
  "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n" \
  "    </method>\n"

static DBusHandlerResult
filter_function (DBusConnection * connection, DBusMessage * message, void *userdata)
{
	DBusMessage *reply;
	DBusError err;
	int level;
	int ret;

        if (!check_priv (halctx, connection, message, dbus_message_get_path (message), "org.freedesktop.hal.power-management.lcd-panel")) {
                return DBUS_HANDLER_RESULT_HANDLED;
        }

	reply = NULL;
	ret = 0;

	dbus_error_init (&err);

	if (dbus_message_is_method_call (message, BACKLIGHT_IFACE, "SetBrightness")) {

		if (dbus_message_get_args (message, &err, DBUS_TYPE_INT32,
					   &level, DBUS_TYPE_INVALID)) {
			if (level < 0 || level > 15) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.LaptopPanel.Invalid",
								"Brightness has to be between 0 and 15!");

			} else {
				backlight_set (level);

				if ((reply = dbus_message_new_method_return (message)))
					dbus_message_append_args (reply, DBUS_TYPE_INT32,
								  &ret, DBUS_TYPE_INVALID);
			}
		}
	} else if (dbus_message_is_method_call (message, BACKLIGHT_IFACE, "GetBrightness")) {
		if (dbus_message_get_args (message, &err, DBUS_TYPE_INVALID)) {
			level = backlight_get();
			if (level < 0)
				level = 0;
			if (level > 15)
				level = 15;

			if ((reply = dbus_message_new_method_return (message)))
				dbus_message_append_args (reply, DBUS_TYPE_INT32,
							  &level, DBUS_TYPE_INVALID);
		}
	}

	if (reply) {
		dbus_connection_send (connection, reply, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	LIBHAL_FREE_DBUS_ERROR (&err);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int
main (int argc, char **argv)
{
	DBusConnection *conn;
	GMainLoop *main_loop;
	const char *udi;
	DBusError err;
	int retval;

	setup_logger ();
	udi = getenv ("UDI");
	
	HAL_DEBUG (("udi=%s", udi));
	if (udi == NULL)
	{
		HAL_ERROR (("No device specified"));
		return -2;
	}

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL)
	{
		HAL_ERROR (("Cannot connect to hald"));
		retval = -3;
		goto out;
	}

	if (!libhal_device_addon_is_ready (halctx, udi, &err)) 
	{
		retval = -4;
		goto out;
	}

	if (ioperm(0xB2, 0xB3, 1) < 0)
	{
		HAL_ERROR (("ioperm failed (you should be root)."));
		exit(1);
	}

	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

	if (!libhal_device_claim_interface (halctx, BACKLIGHT_OBJECT,
					    BACKLIGHT_IFACE, INTERFACE_DESCRIPTION, &err))
	{
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.LaptopPanel'"));
		retval = -4;
		goto out;
	}

	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	return 0;

out:
        LIBHAL_FREE_DBUS_ERROR (&err);

        if (halctx != NULL) {
                libhal_ctx_shutdown (halctx, &err);
                LIBHAL_FREE_DBUS_ERROR (&err);
                libhal_ctx_free (halctx);
        }

        return retval;
}
