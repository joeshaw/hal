/*
 * Licensed under the GNU General Public License Version 2
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/input.h>

#include "libhal/libhal.h"
#include "hal-setup-keymap-hash-name.h"

static LibHalContext *ctx = NULL;
static char* udi;

static int
evdev_open (const char *dev)
{
	int fd;

	/* requires root privs */
	fd = open (dev, O_RDWR);
	if (fd < 0) {
		printf ("hal-setup-keymap: failed to open('%s'): %s\n",
			dev, strerror (errno));
		return -1;
	}

	return fd;
}

static int
evdev_set_keycode (int fd, int scancode, int keycode)
{
	int ret;
	int codes[2];

	codes[0] = scancode;
	codes[1] = keycode;

	ret = ioctl (fd, EVIOCSKEYCODE, codes);
	if (ret < 0) {
		printf ("hal-setup-keymap: failed to set scancode %x to keycode %d: %s",
			scancode, keycode, strerror(errno));
		return -1;
	}

	return 0;
}

int
main (int argc, char **argv)
{
	char **keymap_list;
	int i = 0;
	int fd;
	unsigned int scancode = 0;
	int keycode = 0;
	char keyname[128];
	int values;
	char *device;
	DBusError error;
	const struct key *name;

	udi = getenv ("UDI");
	if (udi == NULL) {
		printf ("hal-setup-keymap: Failed to get UDI");
		return 1;
	}
	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		printf ("hal-setup-keymap: Unable to initialise libhal context: %s", error.message);
		return 1;
	}

	dbus_error_init (&error);
	if (!libhal_device_addon_is_ready (ctx, udi, &error)) {
		return 1;
	}

	/* get the string list data */
	dbus_error_init (&error);
	keymap_list = libhal_device_get_property_strlist (ctx, udi, "input.keymap.data", &error);
	if (dbus_error_is_set (&error) == TRUE) {
		printf ("hal-setup-keymap: Failed to get keymap list: '%s'", error.message);
		return 1;
	}

	/* get the device */
	device = libhal_device_get_property_string (ctx, udi, "input.device", &error);
	if (dbus_error_is_set (&error) == TRUE) {
		printf ("hal-setup-keymap: Failed to get input device list: '%s'", error.message);
		return 1;
	}

	/* get a file descriptor to the device */
	fd = evdev_open (device);
	if (fd < 0) {
		printf ("hal-setup-keymap: Could not open device");
		return 1;
	}

	/* add each of the keys */
	do {
		values = sscanf (keymap_list[i], "%x:%s", &scancode, keyname);
		if (values == 2) {
			/* use gperf as it's really quick */
			name = lookup_key (keyname, strlen(keyname));
			if (name != NULL) {
				keycode = name->id;
				printf ("hal-setup-keymap: parsed %s to (%x, %i)",
					keymap_list[i], scancode, keycode);
				evdev_set_keycode (fd, scancode, keycode);
			} else {
				printf ("hal-setup-keymap: failed to find key '%s'", keyname);
			}
		} else {
			printf ("hal-setup-keymap: Failed to parse %s", keymap_list[i]);
		}
	} while (keymap_list[i++] != NULL);

	libhal_free_string_array (keymap_list);
	close (fd);

	/* we do not have to poll anything, so exit */
	return 0;
}

