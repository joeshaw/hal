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
 * @defgroup HalDaemonLinuxIdeHost IDE Host
 * @ingroup HalDaemonLinux
 * @brief IDE Host
 * @{
 */


static dbus_bool_t
ide_host_device_accept (BusDeviceHandler *self, const char *path, 
			struct sysfs_device *device, dbus_bool_t is_probing)
{
	int ide_host_number;

	if (sscanf (device->bus_id, "ide%d", &ide_host_number) != 1) {
		return FALSE;
	}

	return TRUE;
}

static char *
ide_host_device_compute_udi (HalDevice *d, int append_num)
{
	static char buf[256];

	if (append_num == -1)
		sprintf (buf, "/org/freedesktop/Hal/devices/ide_host_%d",
			 hal_device_property_get_int (d, "ide_host.host_number"));
	else
		sprintf (buf, "/org/freedesktop/Hal/devices/ide_host_%d/%d",
			 hal_device_property_get_int (d, "ide_host.host_number"),
			 append_num);

	return buf;
}

static void 
ide_host_device_post_process (BusDeviceHandler *self,
			      HalDevice *d,
			      const char *sysfs_path,
			      struct sysfs_device *device)
{
	int ide_host_number;

	sscanf (device->bus_id, "ide%d", &ide_host_number);
	hal_device_property_set_int (d, "ide_host.host_number", ide_host_number);

	/* guestimate product name */
	hal_device_property_set_string (d, "info.product", "IDE host controller");

	/* virtual device */
	hal_device_property_set_bool (d, "info.virtual", TRUE);
}


/** Method specialisations for bustype pci */
BusDeviceHandler ide_host_bus_handler = {
	bus_device_init,           /**< init function */
	bus_device_detection_done, /**< detection is done */
	bus_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	ide_host_device_accept,         /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	ide_host_device_compute_udi,    /**< UDI computing function */
	ide_host_device_post_process,   /**< add more properties */
	"ide_host",                /**< sysfs bus name */
	"ide_host"                 /**< namespace */
};


/** @} */
