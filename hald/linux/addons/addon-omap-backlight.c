/***************************************************************************
 * CVSID: $Id$
 *
 * addon-omap-backlight.c : daemon, handling OMAP
 * non-standard backlight. Based on macbookpro addon by
 * David Zeuthen and Nicolas Boichat
 *
 * Copyright (C) 2006 Sergey Lapin <slapinid@gmail.com>
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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
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
static char *udi;
static DBusConnection *conn;

#define NUM_BUF_LEN 11

static char buffer[NUM_BUF_LEN];

struct backlight
{
	void (*set_backlight_level) (struct backlight *bl, int i);
	int (*get_backlight_level) (struct backlight *bl);
	void (*backlight_init) (struct backlight *bl);
	int bl_min;
	int bl_max;
};

struct backlight bl_data;

/* Reads backligh level */
static int
read_backlight (struct backlight * bl)
{
	int fd, ret;

	fd = open ("/sys/devices/platform/omapfb/panel/backlight_level", O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read (fd, buffer, NUM_BUF_LEN);
	close (fd);

	if (ret >= 0)
		return atoi (buffer);
	else
		return -1;
}

/* Read maximum bl level */
/* TODO: set actual maximum level in property.
   No we have fixed value in FDI file, but it
   is better to set it in addon code.
*/
static void
backlight_init (struct backlight * bl)
{
	int fd, ret;

	/* Reading maximum backlight level */
	fd = open ("/sys/devices/platform/omapfb/panel/backlight_max", O_RDONLY);
	if (fd < 0)
		return;

	ret = read(fd, buffer, NUM_BUF_LEN - 1);
	close(fd);

	if (ret >= 0)
		bl->bl_max = atoi (buffer);
}

/* Setting backlight level */
static void
write_backlight (struct backlight * bl, int level)
{
	int fd, l, ret;

	/* sanity-checking level we're required to set */
	if (level > bl->bl_max)
		level = bl->bl_max;

	if (level < bl->bl_min)
		level = bl->bl_min;

	fd = open ("/sys/devices/platform/omapfb/panel/backlight_level", O_WRONLY);
	if (fd < 0)
		return;

	l = snprintf (buffer, NUM_BUF_LEN - 1, "%d", level);

	if (l < (NUM_BUF_LEN - 1))
		write (fd, buffer, l);

	close (fd);
}


/* DBus filter function */
static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply;

	if (!check_priv (halctx, connection, message, dbus_message_get_path (message),
	                 "org.freedesktop.hal.power-management.lcd-panel")) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

#ifdef DEBUG_OMAP_BL
	dbg ("filter_function: sender=%s destination=%s obj_path=%s interface=%s method=%s",
	     dbus_message_get_sender (message),
	     dbus_message_get_destination (message),
	     dbus_message_get_path (message),
	     dbus_message_get_interface (message),
	     dbus_message_get_member (message));
#endif
	reply = NULL;
	dbus_error_init (&err);

	if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Device.LaptopPanel",
					 "SetBrightness")) {
		int brightness;

		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			if (brightness < 0 || brightness > 228) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.LaptopPanel.Invalid",
								"Brightness has to be between 0 and 228!");
			} else {
				int return_code;

				bl_data.set_backlight_level (&bl_data, brightness);

				reply = dbus_message_new_method_return (message);
				if (reply == NULL)
					goto error;

				return_code = 0;
				dbus_message_append_args (reply,
							  DBUS_TYPE_INT32, &return_code,
							  DBUS_TYPE_INVALID);
			}

			dbus_connection_send (connection, reply, NULL);
		}
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device.LaptopPanel",
						"GetBrightness")) {
		int brightness;

		if (dbus_message_get_args (message,
					   &err,
					   DBUS_TYPE_INVALID)) {

			brightness = bl_data.get_backlight_level (&bl_data);
			if (brightness < bl_data.bl_min)
				brightness = bl_data.bl_min;
			if (brightness > bl_data.bl_max)
				brightness = bl_data.bl_max;

			/* dbg ("getting brightness, it's %d", brightness); */

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

/* Setting-up backlight structure */
static void
setup_cb (void)
{
	memset (&bl_data, 0, sizeof (struct backlight));
	bl_data.backlight_init = backlight_init;
	bl_data.get_backlight_level = read_backlight;
	bl_data.set_backlight_level = write_backlight;
}

int
main (int argc, char *argv[])
{
	DBusError err;
	int retval = 0;

	setup_logger ();
	setup_cb ();
	udi = getenv ("UDI");

	HAL_DEBUG (("udi=%s", udi));
	if (udi == NULL) {
		HAL_ERROR (("No device specified"));
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

	if (!libhal_device_claim_interface (halctx,
					    "/org/freedesktop/Hal/devices/omapfb_bl",
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

	if (!libhal_device_addon_is_ready (halctx, udi, &err)) {
		retval = -4;
		goto out;
	}

	bl_data.backlight_init (&bl_data);
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
