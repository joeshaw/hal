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

/* asm/types.h required for __s32 in linux/hiddev.h */
#include <asm/types.h>
#include <fcntl.h>
#include <linux/hiddev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "libhal/libhal.h"

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	char name[256] = "Unknown HID device";
	unsigned int i;
	struct hiddev_devinfo device_info;

	fd = -1;

	/* assume failure */
	ret = 1;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	device_file = getenv ("HAL_PROP_HIDDEV_DEVICE");
	if (device_file == NULL)
		goto out;

	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	if (ioctl (fd, HIDIOCGNAME(sizeof (name)), name) >= 0) {
		if (!libhal_device_set_property_string (ctx, udi, "hiddev.product", name, &error))
			goto out;
		if (!libhal_device_set_property_string (ctx, udi, "info.product", name, &error))
			goto out;
	}

	if (ioctl (fd, HIDIOCGDEVINFO, &device_info) < 0)
		goto out;

	for (i = 0; i < device_info.num_applications; i++) {
		int appl;
		const char *appl_name;
		char buf[256];

		if ((appl = ioctl(fd, HIDIOCAPPLICATION, i)) < 0)
			goto out;

		/* The magic values come from various usage table specs */
		switch (appl >> 16)
		{
		case 0x01 :
			appl_name = "Generic Desktop Page";
			break;
		case 0x0c :
			appl_name = "Consumer Product Page";
			break;
		case 0x80 :
			appl_name = "USB Monitor Page";
			break;
		case 0x81 :
			appl_name = "USB Enumerated Values Page";
			break;
		case 0x82 :
			appl_name = "VESA Virtual Controls Page";
			break;
		case 0x83 :
			appl_name = "Reserved Monitor Page";
			break;
		case 0x84 :
			appl_name = "Power Device Page";
			break;
		case 0x85 :
			appl_name = "Battery System Page";
			break;
		case 0x86 :
		case 0x87 :
			appl_name = "Reserved Power Device Page";
			break;
		default :
			snprintf (buf, sizeof (buf), "Unknown page 0x%02x", appl);
			appl_name = buf;
		}

		if (!libhal_device_property_strlist_append (ctx, udi, "hiddev.application_pages", appl_name, &error))
			goto out;
	}

#if 0
	DBusConnection *conn;

	if (fork () == 0) {
		sleep (10);

		LIBHAL_FREE_DBUS_ERROR (&error);
		if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
			goto out;
		
		if ((ctx = libhal_ctx_new ()) == NULL)
			goto out;
		if (!libhal_ctx_set_dbus_connection (ctx, conn))
			goto out;
		if (!libhal_ctx_init (ctx, &error))
			goto out;

		main2 (ctx, "/org/freedesktop/Hal/devices/usb_device_51d_2_QB0435136106  _if0_hiddev", fd);
	}
	else
		sleep (2);
#endif

	/* success */
	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	LIBHAL_FREE_DBUS_ERROR (&error);

	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
