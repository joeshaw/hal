/***************************************************************************
 * CVSID: $Id$
 *
 * PCI bus devices
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

/**
 * @defgroup HalDaemonLinuxIde IDE
 * @ingroup HalDaemonLinux
 * @brief IDE
 * @{
 */

static dbus_bool_t
ide_device_accept (BusDeviceHandler *self, const char *path, 
		   struct sysfs_device *device)
{

/* Uncomment this to test that we can ignore a device
 * (specifically, in this case, a PCMCIA card reader on a
 * normal system
 */
/*
	if (strcmp (device->path, "/sys/devices/ide2/2.0") == 0)
		return FALSE;
*/

	/* only care about given bus name  */
	return strcmp (device->bus, self->sysfs_bus_name) == 0;
}


static char *
ide_device_compute_udi (HalDevice *d, int append_num)
{
	static char buf[256];

	if (append_num == -1)
		sprintf (buf, "/org/freedesktop/Hal/devices/ide_%d_%d",
			 hal_device_property_get_int (d, "ide.host"),
			 hal_device_property_get_int (d, "ide.channel"));
	else
		sprintf (buf, "/org/freedesktop/Hal/devices/ide_%d_%d/%d",
			 hal_device_property_get_int (d, "ide.host"),
			 hal_device_property_get_int (d, "ide.channel"),
			 append_num);

	return buf;
}

static void 
ide_device_pre_process (BusDeviceHandler *self,
			HalDevice *d,
			const char *sysfs_path,
			struct sysfs_device *device)
{
	int host, channel;

	sscanf (device->bus_id, "%d.%d", &host, &channel);
	hal_device_property_set_int (d, "ide.host", host);
	hal_device_property_set_int (d, "ide.channel", channel);

	if (channel == 0) {
		hal_device_property_set_string (d, "info.product",
						"IDE device (master)");
	} else {
		hal_device_property_set_string (d, "info.product",
						"IDE device (slave)");
	}
}


/** Method specialisations for bustype pci */
BusDeviceHandler ide_bus_handler = {
	bus_device_init,           /**< init function */
	bus_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	ide_device_accept,         /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	ide_device_compute_udi,    /**< UDI computing function */
	ide_device_pre_process,    /**< add more properties */
	bus_device_got_udi,        /**< got UDI */
	bus_device_in_gdl,         /**< in GDL */
	"ide",                     /**< sysfs bus name */
	"ide"                      /**< namespace */
};


/** @} */
