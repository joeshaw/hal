/***************************************************************************
 * CVSID: $Id$
 *
 * SCSI device device class
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
#include <limits.h>

#include "../logger.h"
#include "../device_store.h"
#include "class_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxInput Input class
 * @ingroup HalDaemonLinux
 * @brief Input class
 * @{
 */


static void 
scsi_device_class_post_process (ClassDeviceHandler *self,
				HalDevice *d,
				const char *sysfs_path,
				struct sysfs_class_device *class_device)
{
	const char *last_elem;
	int host_num, bus_num, target_num, lun_num;

	/* Sets last_elem to 1:2:3:4 in 
	 * path=/sys/class/scsi_host/host23/1:2:3:4 
	 */
	last_elem = get_last_element (sysfs_path);
	sscanf (last_elem, "%d:%d:%d:%d",
		&host_num, &bus_num, &target_num, &lun_num);
	hal_device_property_set_int (d, "scsi_device.host", host_num);
	hal_device_property_set_int (d, "scsi_device.bus", bus_num);
	hal_device_property_set_int (d, "scsi_device.target", target_num);
	hal_device_property_set_int (d, "scsi_device.lun", lun_num);

	/* guestimate product name */
	hal_device_property_set_string (d, "info.product", "SCSI Device");

	/* this is a virtual device */
	hal_device_property_set_bool (d, "info.virtual", TRUE);
}

static char *
scsi_device_class_compute_udi (HalDevice * d, int append_num)
{
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format =
		    "/org/freedesktop/Hal/devices/scsi_device_%d_%d_%d_%d";
	else
		format =
		    "/org/freedesktop/Hal/devices/scsi_device_%d_%d_%d_%d-%d";

	snprintf (buf, 256, format,
		  hal_device_property_get_int (d, "scsi_device.host"),
		  hal_device_property_get_int (d, "scsi_device.bus"),
		  hal_device_property_get_int (d, "scsi_device.target"),
		  hal_device_property_get_int (d, "scsi_device.lun"),
		  append_num);

	return buf;
}

/** Method specialisations for input device class */
ClassDeviceHandler scsi_device_class_handler = {
	class_device_init,                  /**< init function */
	class_device_detection_done,        /**< detection is done */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	scsi_device_class_post_process,     /**< add more properties */
	scsi_device_class_compute_udi,      /**< No UDI computation */
	"scsi_device",                      /**< sysfs class name */
	"scsi_device",                      /**< hal class name */
	FALSE,                              /**< don't require device file */
	FALSE                               /**< add as child to parent */
};

/** @} */
