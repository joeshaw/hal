/***************************************************************************
 * CVSID: $Id$
 *
 * USB serial
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>

#include "../logger.h"
#include "../device_store.h"
#include "bus_device.h"
#include "common.h"

static char *
usb_serial_device_compute_udi (HalDevice *d, int append_num)
{
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format = "%s_usb-serial";
	else
		format = "%s_usb-serial-%d";

	snprintf (buf, 255, format,
		  hal_device_property_get_string (d, "info.parent"), append_num);
	buf[255] = '\0';

	return buf;
}

static void
usb_serial_device_pre_process (BusDeviceHandler *self,
			       HalDevice *d,
			       const char *sysfs_path,
			       struct sysfs_device *device)
{
	/* a class/tty device is expected to overide this by merging into this device */
	hal_device_property_set_string (d, "info.product", "USB Serial Device");
}

/** Method specialisations */
BusDeviceHandler usb_serial_bus_handler = {
	.init		= bus_device_init,
	.shutdown	= bus_device_shutdown,
	.tick		= bus_device_tick,
	.accept		= bus_device_accept,
	.visit		= bus_device_visit,
	.removed	= bus_device_removed,
	.compute_udi	= usb_serial_device_compute_udi,
	.pre_process	= usb_serial_device_pre_process,
	.got_udi	= bus_device_got_udi,
	.in_gdl		= bus_device_in_gdl,
	.sysfs_bus_name	= "usb-serial",
	.hal_bus_name	= "usb-serial"
};

/** @} */
