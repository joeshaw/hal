/***************************************************************************
 * CVSID: $Id$
 *
 * hal_dev.c : Tiny program to transform an udev device event into
 *             a D-BUS message
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.0
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mntent.h>
#include <syslog.h>
#include <linux/limits.h>

#include <dbus/dbus.h>

/**
 * @defgroup HalMisc  Misc tools for HAL
 * @brief  Misc. tools for HAL
 */


/**
 * @defgroup HalLinuxDeviceEvent  HAL device event for Linux
 * @ingroup HalMisc
 * @brief A short program for translating linux-hotplug events into
 *        D-BUS messages. The messages are sent to the HAL daemon.
 * @{
 */


static char sysfs_mnt_path[PATH_MAX];

/** Get the mount path for sysfs. A side-effect is that sysfs_mnt_path
 *  is set on success.
 *
 *  @return                     0 on success, negative on error
 */
static int
get_sysfs_mnt_path ()
{
	FILE *mnt;
	struct mntent *mntent;
	int ret = 0;
	size_t dirlen = 0;

	if ((mnt = setmntent ("/proc/mounts", "r")) == NULL) {
		return -1;
	}

	while (ret == 0 && dirlen == 0
	       && (mntent = getmntent (mnt)) != NULL) {
		if (strcmp (mntent->mnt_type, "sysfs") == 0) {
			dirlen = strlen (mntent->mnt_dir);
			if (dirlen <= (PATH_MAX - 1)) {
				strcpy (sysfs_mnt_path, mntent->mnt_dir);
			} else {
				ret = -1;
			}
		}
	}
	endmntent (mnt);

	if (dirlen == 0 && ret == 0) {
		ret = -1;
	}
	return ret;
}
/** Entry point
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @param  envp                Environment
 *  @return                     Exit code
 */
int
main (int argc, char *argv[], char *envp[])
{
	int i, j, len;
	char *str;
	char *devpath;
	char *devname;
	int is_add;
	DBusError error;
	DBusConnection *sysbus_connection;
	DBusMessage *message;
	DBusMessageIter iter;
	DBusMessageIter iter_dict;

	if (argc != 2)
		return 1;

	if (get_sysfs_mnt_path () != 0)
		return 1;

	openlog ("hal.dev", LOG_PID, LOG_USER);

	/* Connect to a well-known bus instance, the system bus */
	dbus_error_init (&error);
	sysbus_connection = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (sysbus_connection == NULL)
		return 1;

	/* service, object, interface, member */
	message = dbus_message_new_method_call (
		"org.freedesktop.Hal",
		"/org/freedesktop/Hal/Linux/Hotplug",
		"org.freedesktop.Hal.Linux.Hotplug",
		"DeviceEvent");

	/* not interested in a reply */
	dbus_message_set_no_reply (message, TRUE);

	devpath = NULL;
	devname = NULL;
	is_add = FALSE;

	dbus_message_iter_init (message, &iter);
	for (i = 0; envp[i] != NULL; i++) {
		str = envp[i];
		len = strlen (str);
		for (j = 0; j < len && str[j] != '='; j++);
		str[j] = '\0';

		if (strcmp (str, "DEVPATH") == 0) {
			devpath = str + j + 1;
		} else if (strcmp (str, "DEVNODE") == 0) {
			devname = str + j + 1;
		} else if (strcmp (str, "DEVNAME") == 0) {
			devname = str + j + 1;
		} else if (strcmp (str, "ACTION") == 0) {
			if (strcmp (str + j + 1, "add") == 0) {
				is_add = TRUE;
			}
		}
	}

	if (devname == NULL || devpath == NULL) {
		syslog (LOG_ERR, "Missing devname or devpath");
		goto out;
	}

	dbus_message_iter_append_boolean (&iter, is_add);
	dbus_message_iter_append_string (&iter, devname);
	dbus_message_iter_append_string (&iter, devpath);

	if (!dbus_connection_send (sysbus_connection, message, NULL))
		return 1;

	dbus_message_unref (message);

	dbus_connection_flush (sysbus_connection);

	/* Do some sleep here so messages are not lost.. */
	usleep (500 * 1000);
	
out:
	dbus_connection_disconnect (sysbus_connection);

	return 0;
}

/** @} */
