/***************************************************************************
 * CVSID: $Id$
 *
 * addon-generic-backlight.c: 
 * Copyright (C) 2008 Danny Kukawka <danny.kukawka@web.de>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h> 

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../../logger.h"
#include "../../util_helper.h"
#include "../../util_helper_priv.h"

static GMainLoop *main_loop;
static LibHalContext *halctx = NULL;
static DBusConnection *conn;

const char *udi = NULL;
const char *path = NULL;
static int levels = 0;

/* Getting backlight level */
static int
get_backlight ()
{
	FILE *f;
	int value;
	char buf[64];
	gchar sysfs_path[512];
	
	f = NULL;
	value = -1;

	g_snprintf (sysfs_path, sizeof (sysfs_path), "%s/actual_brightness", path);

	f = fopen (sysfs_path, "rb");
        if (f == NULL) {
		HAL_WARNING(("Could not read brightness from '%s'", sysfs_path));
		return -1;
	}

	if (fgets (buf, sizeof (buf), f) == NULL) {
		HAL_ERROR (("Cannot read from '%s'", sysfs_path));
                goto out;
        }

	errno = 0;
	value = strtol (buf, NULL, 10);
	if (errno != 0) { 
		value = -1;
	}

out:
        if (f != NULL)
                fclose (f);

        return value;
}

/* Setting backlight level */
static int
set_backlight (int level)
{
	int fd, l, ret;
	gchar sysfs_path[512];
	/* Assume we don't need more */
	char buf[5];

	/* sanity-checking level */
	if (level > levels-1)
		level = levels-1;

	if (level < 0)
		level = 0;

	ret = -1;

	g_snprintf (sysfs_path, sizeof (sysfs_path), "%s/brightness", path);

	fd = open (sysfs_path, O_WRONLY);
	if (fd < 0) {
		HAL_WARNING(("Could not open '%s'", sysfs_path));
		goto out;
	}
 
	if ((l = snprintf (buf, 4, "%d", level)) < 4) {
		if (write (fd, buf, l) < 0) {
			HAL_WARNING(("Could not write '%s' to '%s'", buf , sysfs_path));
		} else {
			/* everything okay */
			ret = level;
		}
	} 
		
out:
	if (fd >= 0)
		close (fd);

	return ret;
}

/* DBus filter function */
static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply;
	int brightness;

	if (!check_priv (halctx, connection, message, dbus_message_get_path (message),
	                 "org.freedesktop.hal.power-management.lcd-panel")) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = NULL;

	dbus_error_init (&err);

	if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Device.LaptopPanel",
					 "SetBrightness")) {
		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			if (brightness < 0 || brightness > levels -1) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.LaptopPanel.Invalid",
								"Brightness level is invalid");
			} else {			
				int return_code;
				int set;

				set = set_backlight (brightness);

				reply = dbus_message_new_method_return (message);
				if (reply == NULL)
					goto error;

				if (set == brightness)
					return_code = 0;
				else 
					return_code = 1;

				dbus_message_append_args (reply,
							  DBUS_TYPE_INT32, &return_code,
							  DBUS_TYPE_INVALID);
			}

			dbus_connection_send (connection, reply, NULL);
		}
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device.LaptopPanel",
						"GetBrightness")) {
		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INVALID)) {
			brightness = get_backlight();

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
	}

error:
	LIBHAL_FREE_DBUS_ERROR (&err);

	if (reply != NULL)
		dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
	DBusError err;
	char * level_str;
	int retval = 0;

	setup_logger ();
	udi = getenv ("UDI");
	path = getenv ("HAL_PROP_LINUX_SYSFS_PATH");

	HAL_DEBUG (("udi='%s', path='%s'", udi, path));
	if (udi == NULL) {
		HAL_ERROR (("No device specified"));
		return -2;
	}
	if (path == NULL) {
		HAL_ERROR (("No sysfs path specified"));
		return -2;
	}
	
	level_str = getenv ("HAL_PROP_LAPTOP_PANEL_NUM_LEVELS");
	if (level_str != NULL) {
		levels = atoi (level_str);
	} else {
		HAL_ERROR (("No laptop_panel.num_levels defined"));
	}
		
	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		HAL_ERROR (("Cannot connect to hald"));
		retval = -3;
		goto out;
	}

	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

	if (!libhal_device_claim_interface (halctx,
					    udi,
					    "org.freedesktop.Hal.Device.LaptopPanel",
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.LaptopPanel'"));
		retval = -4;
		goto out;
	}

	dbus_error_init (&err);
	if (!libhal_device_addon_is_ready (halctx, udi, &err)) {
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
