/***************************************************************************
 *
 * umount-hal.c : Plug-in for umount(8) - see RH #188193
 *
 * https://bugzilla.redhat.com/bugzilla/show_bug.cgi?id=188193
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libhal.h>
#include <libhal-storage.h>
#include "../hald/util.h"

int
main (int argc, char *argv[])
{
	int ret;
	char device_file_or_mount_point[HAL_PATH_MAX];
	DBusError error;
	DBusConnection *con;
	LibHalContext *hal_ctx;
	LibHalVolume *vol;
	DBusMessage *message;
	DBusMessage *reply;
	int hal_retcode;
	char **options = NULL;
	int num_options = 0;

	ret = 1;

	if (argc < 2 || strlen (argv[1]) == 0) {
		fprintf (stderr, "%s: this program is only supposed to be invoked by umount(8).\n", argv[0]);
		goto out;
	}

	/* it appears the device file / mount point is always the
	 * first argument.  TODO XXX FIXME: we ought to honor
	 * umount(8) options like -v for verbose.
	 */
	realpath(argv[1], device_file_or_mount_point);

	dbus_error_init (&error);
	con = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (con == NULL) {
		fprintf (stderr, "%s: dbus_bus_get(): %s: %s\n", argv[0], error.name, error.message);
		goto out;
	}

	hal_ctx = libhal_ctx_new ();
	libhal_ctx_set_dbus_connection (hal_ctx, con);
	if (!libhal_ctx_init (hal_ctx, &error)) {
		if (dbus_error_is_set(&error)) {
			fprintf (stderr, "%s: libhal_ctx_init: %s: %s\n", argv[0], error.name, error.message);
			dbus_error_free (&error);
		} else {
			fprintf (stderr, "%s: libhal_ctx_init failed. Is hald running?\n", argv[0]);
		}
		goto out;
	}

	vol = libhal_volume_from_device_file (hal_ctx, device_file_or_mount_point);
	if (vol == NULL) {

		/* it might be a mount point! */
		if (device_file_or_mount_point[strlen (device_file_or_mount_point) - 1] == '/') {
			device_file_or_mount_point[strlen (device_file_or_mount_point) - 1] = '\0';
		}
		vol = libhal_volume_from_mount_point (hal_ctx, device_file_or_mount_point);
		if (vol == NULL) {
			fprintf (stderr, "%s: %s is not recognized by hal\n", argv[0], device_file_or_mount_point);
			goto out;
		}
	}

	message = dbus_message_new_method_call ("org.freedesktop.Hal", 
						libhal_volume_get_udi (vol),
						"org.freedesktop.Hal.Device.Volume",
						"Unmount");
	if (message == NULL) {
		goto out;
	}

	if (!dbus_message_append_args (message, 
				       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &options, num_options,
				       DBUS_TYPE_INVALID)) {
		goto out;
	}


	if (!(reply = dbus_connection_send_with_reply_and_block (con, 
								 message, 
								 -1,
								 &error)) || dbus_error_is_set (&error)) {
		fprintf (stderr, "%s: Unmounting %s failed: %s: %s\n", 
			 argv[0], device_file_or_mount_point, error.name, error.message);
		goto out;
	}

	if (!dbus_message_get_args (reply, 
				    &error,
				    DBUS_TYPE_INT32, &hal_retcode,
				    DBUS_TYPE_INVALID)) {
		/* should never happen */
		goto out;
	}

	if (hal_retcode != 0) {
		/* should never happen; we should get an exception instead */
		goto out;
	}

	ret = 0;

out:
	return ret;	
}
