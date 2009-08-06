/***************************************************************************
 * CVSID: $Id$
 *
 * addon-leds.c: 
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

static GMainLoop *gmain = NULL;
static LibHalContext *ctx = NULL;
static GHashTable *leds = NULL;

/* Getting current brightness */
static int
get_leds_brightness (const char *udi)
{
	FILE *f;
	char buf[64];
	char path[256];
	char *sysfs_path;
	int brightness;
	
	f = NULL;

	if (!g_hash_table_lookup_extended (leds, udi, NULL, (gpointer) &sysfs_path)) {
		return -1;
	}

	snprintf (path, sizeof (path), "%s/brightness", sysfs_path);

        if ((f = fopen (path, "rb")) == NULL) {
		HAL_WARNING(("Could not read brightness from '%s' for device '%s'", path, udi));
		return -1;
	}

	if (fgets (buf, sizeof (buf), f) == NULL) {
		HAL_ERROR (("Cannot read from '%s' for device '%s'", path, udi));
                goto out;
        }

	errno = 0;
	brightness = strtol (buf, NULL, 10);
	if (errno != 0) { 
		brightness = -1;
	} 

out:
        if (f != NULL)
                fclose (f);

        return brightness;
}

/* Setting current brightness of the led */
static int
set_leds_brightness (const char *udi, int level)
{
	int fd, l, ret;
	char path[256];
	char buf[5];	/* Assume we don't need more */
	char *sysfs_path;
	DBusError error;

	ret = -1;

	if (!g_hash_table_lookup_extended (leds, udi, NULL, (gpointer) &sysfs_path)) {
		return -1;
	}

	dbus_error_init (&error);

	if (level > libhal_device_get_property_int (ctx, udi, "leds.num_levels", &error) -1) {
		HAL_DEBUG (("Requested level is higher than max level (check leds.num_levels)."));
		return -1;
	}

	snprintf (path, sizeof (path), "%s/brightness", sysfs_path);

	fd = open (path, O_WRONLY);
	if (fd < 0) {
		HAL_WARNING(("Could not open '%s' for '%s', error='%s'", sysfs_path, udi, strerror(errno)));
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
	const char *_udi;
	const char *interface;
	char *sysfs_path;

	if ((_udi = dbus_message_get_path (message)) == NULL) {
		HAL_DEBUG (("Couldn't get the udi for this call, ignore it."));
		return DBUS_HANDLER_RESULT_HANDLED;
	} else {
		if(!g_hash_table_lookup_extended (leds, _udi, NULL, (gpointer *) &sysfs_path)) {
			HAL_DEBUG (("This device (%s) isn't yet handled by the addon.", _udi));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

	dbus_error_init (&err);

	interface = dbus_message_get_interface (message);
	if (interface != NULL && strcmp (interface, "org.freedesktop.Hal.Device.KeyboardBacklight") == 0) {
		if (!check_priv (ctx, connection, message, dbus_message_get_path (message), "org.freedesktop.hal.power-management.keyboard-backlight")) {
                	HAL_DEBUG(("User don't have the permissions to call the interface"));
                	return DBUS_HANDLER_RESULT_HANDLED;
		}
	} else if (!check_priv (ctx, connection, message, dbus_message_get_path (message), "org.freedesktop.hal.leds.brightness")) {
		HAL_DEBUG(("User don't have the permissions to call the interface"));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = NULL;

	if (dbus_message_is_method_call (message, "org.freedesktop.Hal.Device.Leds", "SetBrightness") || 
	    dbus_message_is_method_call (message, "org.freedesktop.Hal.Device.KeyboardBacklight", "SetBrightness")) {
		int brightness;

		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			int return_code = 0;
			int set;

			set = set_leds_brightness (_udi, brightness);

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

			dbus_connection_send (connection, reply, NULL);
		}
	} else if (dbus_message_is_method_call (message, "org.freedesktop.Hal.Device.Leds", "GetBrightness") ||
		   dbus_message_is_method_call (message, "org.freedesktop.Hal.Device.KeyboardBacklight", "GetBrightness")) {
		int brightness;

		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INVALID)) {
			brightness = get_leds_brightness(_udi);

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
	if (reply != NULL)
		dbus_message_unref (reply);

	LIBHAL_FREE_DBUS_ERROR (&err);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void
add_device (LibHalContext *ctx,
	    const char *udi,
	    const LibHalPropertySet *properties)
{
	DBusError err;
	DBusConnection *dbus_connection;
	const char* sysfs_path;
	const char* function;
	static gboolean initialized = FALSE;

	if ((sysfs_path = libhal_ps_get_string (properties, "linux.sysfs_path")) == NULL) {
		HAL_ERROR(("%s has no property linux.sysfs_path", udi));
		return;
	}
	if ((function = libhal_ps_get_string (properties, "leds.function")) == NULL) {
		HAL_ERROR(("%s has no property leds.function", udi));
		return;
	}

	/* claim the interface */
	
        if ((dbus_connection = libhal_ctx_get_dbus_connection(ctx)) == NULL) {
                HAL_WARNING (("Cannot get DBus connection"));
                return;
        }

	if (!initialized) {
		dbus_connection_add_filter (dbus_connection, filter_function, NULL, NULL);
		initialized = TRUE;
	}

	dbus_error_init (&err);

	if (function != NULL && strcmp(function, "kbd_backlight") == 0) {
		HAL_DEBUG (("Found a led which is a keyboard backlight."));
		
		if (!libhal_device_claim_interface (ctx,
						    udi,
						    "org.freedesktop.Hal.Device.KeyboardBacklight",
						    "    <method name=\"SetBrightness\">\n"
						    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
						    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
						    "    </method>\n"
						    "    <method name=\"GetBrightness\">\n"
						    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
						    "    </method>\n",
						    &err)) {
			HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.KeyboardBacklight'"));
			LIBHAL_FREE_DBUS_ERROR (&err);
			return;
		}
	} else if (!libhal_device_claim_interface (ctx,
					    udi,
					    "org.freedesktop.Hal.Device.Leds",
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.Leds'"));
		LIBHAL_FREE_DBUS_ERROR (&err);
		return;
	}
	
	g_hash_table_insert (leds, g_strdup(udi), g_strdup(sysfs_path));
}

static void
remove_device (LibHalContext *ctx,
	       const char *udi,
	       const LibHalPropertySet *properties)
{
	gpointer sysfs_path;
	gboolean handling_udi;

	HAL_DEBUG (("Removing channel for '%s'", udi));

	handling_udi = g_hash_table_lookup_extended (leds, udi, NULL, &sysfs_path);

	if (!handling_udi) {
		HAL_ERROR(("DeviceRemove called for unknown device: '%s'.", udi));
		return;
	}

	g_hash_table_remove (leds, udi);

	if (g_hash_table_size (leds) == 0) {
		HAL_INFO(("no more devices, exiting"));
		g_main_loop_quit (gmain);
	}
}

int
main (int argc, char *argv[])
{
	DBusConnection *dbus_connection;
	DBusError error;
	const char *commandline;

	hal_set_proc_title_init (argc, argv);

	setup_logger ();

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		HAL_WARNING (("Unable to init libhal context"));
		goto out;
	}

	if ((dbus_connection = libhal_ctx_get_dbus_connection(ctx)) == NULL) {
		HAL_WARNING (("Cannot get DBus connection"));
		goto out;
	}

	if ((commandline = getenv ("SINGLETON_COMMAND_LINE")) == NULL) {
		HAL_WARNING (("SINGLETON_COMMAND_LINE not set"));
		goto out;
	}

	libhal_ctx_set_singleton_device_added (ctx, add_device);
	libhal_ctx_set_singleton_device_removed (ctx, remove_device);

	dbus_connection_setup_with_g_main (dbus_connection, NULL);
	dbus_connection_set_exit_on_disconnect (dbus_connection, 0);


	if (!libhal_device_singleton_addon_is_ready (ctx, commandline, &error)) {
		goto out;
	}

	leds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	gmain = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (gmain);

	return 0;

out:
	HAL_DEBUG (("An error occured, exiting cleanly"));
	
	LIBHAL_FREE_DBUS_ERROR (&error);

	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return 0;
}
