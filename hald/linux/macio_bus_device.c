/***************************************************************************
 * CVSID: $Id$
 *
 * MacIO bus device
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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


static char *
macio_device_compute_udi (HalDevice *d, int append_num)
{
	static char buf[256];

	if (append_num == -1)
		sprintf (buf, "/org/freedesktop/Hal/devices/ide_%s",
			 hal_device_property_get_string (d, "macio.bus_id"));
	else
		sprintf (buf, "/org/freedesktop/Hal/devices/ide_%s/%d",
			 hal_device_property_get_string (d, "macio.bus_id"),
			 append_num);

	return buf;
}

static void 
macio_device_pre_process (BusDeviceHandler *self,
			  HalDevice *d,
			  const char *sysfs_path,
			  struct sysfs_device *device)
{
	hal_device_property_set_string (d, "macio.bus_id", device->bus_id);
	hal_device_property_set_bool (d, "info.virtual", TRUE);
}


/** Method specialisations for bustype pci */
BusDeviceHandler macio_bus_handler = {
	bus_device_init,           /**< init function */
	bus_device_detection_done, /**< detection is done */
	bus_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	bus_device_accept,         /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	macio_device_compute_udi,  /**< UDI computing function */
	macio_device_pre_process,  /**< add more properties */
	bus_device_got_udi,        /**< got UDI */
	"macio",                   /**< sysfs bus name */
	"macio"                    /**< namespace */
};


/** @} */
