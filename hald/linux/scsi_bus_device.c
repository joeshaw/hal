/***************************************************************************
 * CVSID: $Id$
 *
 * SCSI bus devices
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

static char *
scsi_device_compute_udi (HalDevice *d, int append_num)
{
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format =
		    "/org/freedesktop/Hal/devices/scsi_%d_%d_%d_%d";
	else
		format =
		    "/org/freedesktop/Hal/devices/scsi_%d_%d_%d_%d-%d";

	snprintf (buf, 256, format,
		  hal_device_property_get_int (d, "scsi.host"),
		  hal_device_property_get_int (d, "scsi.bus"),
		  hal_device_property_get_int (d, "scsi.target"),
		  hal_device_property_get_int (d, "scsi.lun"),
		  append_num);

	return buf;
}

static void 
scsi_device_pre_process (BusDeviceHandler *self,
			 HalDevice *d,
			 const char *sysfs_path,
			 struct sysfs_device *device)
{
	const char *last_elem;
	int host_num, bus_num, target_num, lun_num;

	/* Sets last_elem to 1:2:3:4 in 
	 * path=/sys/class/scsi_host/host23/1:2:3:4 
	 */
	last_elem = get_last_element (sysfs_path);
	sscanf (last_elem, "%d:%d:%d:%d",
		&host_num, &bus_num, &target_num, &lun_num);
	hal_device_property_set_int (d, "scsi.host", host_num);
	hal_device_property_set_int (d, "scsi.bus", bus_num);
	hal_device_property_set_int (d, "scsi.target", target_num);
	hal_device_property_set_int (d, "scsi.lun", lun_num);

	/* guestimate product name */
	hal_device_property_set_string (d, "info.product", "SCSI Device");
}


/** Method specialisations for bustype pci */
BusDeviceHandler scsi_bus_handler = {
	bus_device_init,           /**< init function */
	bus_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	bus_device_accept,         /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	scsi_device_compute_udi,   /**< UDI computing function */
	scsi_device_pre_process,   /**< add more properties */
	bus_device_got_udi,        /**< got UDI */
	bus_device_in_gdl,         /**< in GDL */
	"scsi",                    /**< sysfs bus name */
	"scsi"                     /**< namespace */
};


/** @} */
