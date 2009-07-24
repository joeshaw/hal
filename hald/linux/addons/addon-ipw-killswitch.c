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

#include <glib.h>
#include <glib/gmain.h>
#include <glib/gstdio.h>
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

static gboolean
init_killswitch () 
{
        DBusError error;
        char *parent;
	char *iface;
	char *_path;
	char **udis;
	int i, num_udis;
	gboolean ret;

	ret = FALSE; 

	dbus_error_init (&error);

	parent = libhal_device_get_property_string (halctx, udi, "info.parent", &error);
	udis = libhal_manager_find_device_string_match (halctx, "info.parent", parent, &num_udis, &error);

	for (i = 0; i < num_udis; i++) {

		if (strcmp (udis[i], udi) == 0)
			continue;

		iface = libhal_device_get_property_string (halctx, udis[i], "net.interface", &error);
		if (iface != NULL) {
			_path = g_strdup_printf ("/sys/class/net/%s/device/rf_kill", iface);

			/* check if the file exists */
			if(g_file_test(_path, G_FILE_TEST_EXISTS) && g_file_test(_path, G_FILE_TEST_IS_REGULAR)) {
				path = g_strdup (_path);
				ret = TRUE;
			}

			g_free (_path);
			libhal_free_string (iface);
		}
	}

        libhal_free_string (parent);
        libhal_free_string_array (udis);

	return ret;
}

/* Getting status of the killswitch */
static int
get_killswitch ()
{
	FILE *f;
	char buf[64];
	int kill_status;
	int ret = -1;
	
	f = NULL;

        if ((f = fopen (path, "r")) == NULL) {
		HAL_WARNING(("Could not read killswitch status from '%s'", path));
		return -1;
	}

	if (fgets (buf, sizeof (buf), f) == NULL) {
		HAL_ERROR (("Cannot read from '%s'", path));
                goto out;
        }

	errno = 0;
	kill_status = strtol (buf, NULL, 10);
	if (errno == 0) { 
		switch(kill_status) {
			case 0:
				ret = 1;
				break;
			case 1:
			case 2:
			case 3:
				ret = 0;
				break;
			default:
				break;
		}
	}

out:
        if (f != NULL)
                fclose (f);

        return ret;
}

/* Setting status of the killswitch */
static int
set_killswitch (gboolean status)
{
	FILE *f;
	int ret;

	if ((f = fopen (path, "w")) == NULL) {
		HAL_WARNING(("Could not open '%s'", path));
		return -1;
	}	

	if (status) {
		ret = fputs ("0", f);
	} else {
		ret = fputs ("1", f);
	}

	if (ret == EOF) {
		HAL_WARNING(("Could write status to '%s'", path));
		ret = -1;
	} else {
		ret = 0;
	}

        if (f != NULL)
                fclose (f);

        return ret;
}

/* DBus filter function */
static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply;

	if (!check_priv (halctx, connection, message, dbus_message_get_path (message),
	                 "org.freedesktop.hal.killswitch.wlan")) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = NULL;
	
	dbus_error_init (&err);

	if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Device.KillSwitch",
					 "SetPower")) {
		gboolean status;

		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_BOOLEAN, &status,
					   DBUS_TYPE_INVALID)) {
			int return_code = 0;
			int set;

			set = set_killswitch (status);

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			if (set != 0)
				return_code = 1;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &return_code,
						  DBUS_TYPE_INVALID);

			dbus_connection_send (connection, reply, NULL);
		}
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device.KillSwitch",
						"GetPower")) {
		int status;

		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INVALID)) {
			status = get_killswitch();

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &status,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
	}

error:
	if (reply != NULL)
		dbus_message_unref (reply);

	LIBHAL_FREE_DBUS_ERROR (&err);

	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
	DBusError err;
	char *method;
	int retval = 0;

	setup_logger ();
	udi = getenv ("UDI");
	method = getenv ("HAL_PROP_KILLSWITCH_ACCESS_METHOD");

	HAL_DEBUG (("udi='%s'", udi));
	if (udi == NULL) {
		HAL_ERROR (("No device specified"));
		return -2;
	}

	if (strcmp (method, "ipw") != 0) {
		HAL_ERROR (("Wrong killswitch.access_method '%s', should be 'ipw'", method));
		return -2;
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

	if (!init_killswitch()) {
		retval = -4;
		goto out;
	}

	if (!libhal_device_claim_interface (halctx,
					    udi,
					    "org.freedesktop.Hal.Device.KillSwitch",
					    "    <method name=\"SetPower\">\n"
					    "      <arg name=\"value\" direction=\"in\" type=\"b\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetPower\">\n"
					    "      <arg name=\"value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.KillSwitch'"));
		retval = -5;
		goto out;
	}

	if (!libhal_device_addon_is_ready (halctx, udi, &err)) {
		retval = -5;
		goto out;
	}

	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	return 0;

out:
        HAL_DEBUG (("An error occured, exiting cleanly"));

        LIBHAL_FREE_DBUS_ERROR (&err);

        if (halctx != NULL) {
                libhal_ctx_shutdown (halctx, &err);
                LIBHAL_FREE_DBUS_ERROR (&err);
                libhal_ctx_free (halctx);
        }

	return retval;
}
