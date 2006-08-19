/***************************************************************************
 * CVSID: $Id$
 *
 * probe-input.c : Probe input devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhal/libhal.h"

#include "shared.h"

/* we must use this kernel-compatible implementation */
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

static void 
check_abs (int fd, LibHalContext *ctx, const char *udi)
{
	long bitmask[NBITS(ABS_MAX)];
	long bitmask_touch[NBITS(KEY_MAX)];
	DBusError error;

	if (ioctl (fd, EVIOCGBIT(EV_ABS, sizeof (bitmask)), bitmask) < 0) {
		dbg ("ioctl EVIOCGBIT for EV_ABS failed");
		goto out;
	}

	if (ioctl (fd, EVIOCGBIT(EV_KEY, sizeof (bitmask_touch)), bitmask_touch) < 0) {
		dbg ("ioctl EVIOCGBIT for EV_KEY failed");
		goto out;
	}
	
	if (!test_bit(ABS_X, bitmask) || !test_bit(ABS_Y, bitmask)) {
		dbg ("missing x or y absolute axes");
		goto out;
	}

	dbus_error_init (&error);
	if (test_bit(BTN_TOUCH, bitmask_touch) != 0) {
		libhal_device_add_capability (ctx, udi, "input.tablet", &error);
		goto out;
	}
	libhal_device_add_capability (ctx, udi, "input.joystick", &error);

out:
	;
}

static void 
check_key (int fd, LibHalContext *ctx, const char *udi)
{
	unsigned int i;
	long bitmask[NBITS(KEY_MAX)];
	int is_keyboard;
	DBusError error;

	if (ioctl (fd, EVIOCGBIT(EV_KEY, sizeof (bitmask)), bitmask) < 0) {
		dbg ("ioctl EVIOCGBIT for EV_KEY failed");
		goto out;
	}

	is_keyboard = FALSE;

	/* All keys that are not buttons are less than BTN_MISC */
	for (i = KEY_RESERVED + 1; i < BTN_MISC; i++) {
		if (test_bit (i, bitmask)) {
			is_keyboard = TRUE;
			break;
		}
	}

	if (is_keyboard) {
		dbus_error_init (&error);
		libhal_device_add_capability (ctx, udi, "input.keyboard", &error);
	}

out:
	;
}

static void 
check_rel (int fd, LibHalContext *ctx, const char *udi)
{
	long bitmask[NBITS(REL_MAX)];
	DBusError error;

	if (ioctl (fd, EVIOCGBIT(EV_REL, sizeof (bitmask)), bitmask) < 0) {
		dbg ("ioctl EVIOCGBIT for EV_REL failed");
		goto out;
	}

	if (!test_bit (REL_X, bitmask) || !test_bit (REL_Y, bitmask)) {
		dbg ("missing x or y relative axes");
		goto out;
	}

	dbus_error_init (&error);
	libhal_device_add_capability (ctx, udi, "input.mouse", &error);

out:
	;
}

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	char *physical_device;
	LibHalContext *ctx = NULL;
	DBusError error;
	char name[128];
	struct input_id id;

	_set_debug ();

	fd = -1;

	/* assume failure */
	ret = 1;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	device_file = getenv ("HAL_PROP_INPUT_DEVICE");
	if (device_file == NULL)
		goto out;

	dbg ("Doing probe-input for %s (udi=%s)",
	     device_file, udi);

	fd = open (device_file, O_RDONLY);
	if (fd < 0) {
		dbg ("Cannot open %s: %s", device_file, strerror (errno));
		goto out;
	}

	/* if we don't have a physical device then only accept input buses
	 * that we now aren't hotpluggable
	 */
	if (ioctl (fd, EVIOCGID, &id) < 0) {
		dbg ("Error: EVIOCGID failed: %s\n", strerror(errno));
		goto out;
	}
	physical_device = getenv ("HAL_PROP_INPUT_PHYSICAL_DEVICE");

	dbg ("probe-input: id.bustype=%i", id.bustype);
	if (physical_device == NULL) {
		switch (id.bustype) {
		case BUS_I8042: /* x86 legacy port */
		case BUS_HOST: /* not hotpluggable */
		case BUS_PARPORT: /* XXX: really needed? */
		case BUS_ADB: /* ADB on Apple computers */
			break;

		default:
			goto out;
		}
	}

	/* only consider devices with the event interface */
	if (ioctl (fd, EVIOCGNAME(sizeof (name)), name) < 0) {
		dbg ("Error: EVIOCGNAME failed: %s\n", strerror(errno));
		goto out;
	}
	if (!libhal_device_set_property_string (ctx, udi, "info.product", name, &error))
		goto out;
	if (!libhal_device_set_property_string (ctx, udi, "input.product", name, &error))
		goto out;

	check_abs (fd, ctx, udi);
	check_rel (fd, ctx, udi);
	check_key (fd, ctx, udi);

	/* success */
	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
