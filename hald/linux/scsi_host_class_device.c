/***************************************************************************
 * CVSID: $Id$
 *
 * SCSI host device class
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
scsi_host_class_post_process (ClassDeviceHandler *self,
			      HalDevice *d,
			      const char *sysfs_path,
			      struct sysfs_class_device *class_device)
{
	int host_num;
	const char *last_elem;

	/* Sets last_elem to host14 in path=/sys/class/scsi_host/host14 */
	last_elem = get_last_element (sysfs_path);
	sscanf (last_elem, "host%d", &host_num);
	hal_device_property_set_int (d, "scsi_host.host", host_num);

	/* guestimate product name */
	hal_device_property_set_string (d, "info.product",
					"SCSI Host Interface");

	/* this is a virtual device */
	hal_device_property_set_bool (d, "info.virtual", TRUE);
}

static char *
scsi_host_class_compute_udi (HalDevice * d, int append_num)
{
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/scsi_host_%d";
	else
		format = "/org/freedesktop/Hal/devices/scsi_host_%d-%d";

	snprintf (buf, 256, format,
		  hal_device_property_get_int (d, "scsi_host.host"), append_num);

	return buf;
}

/** Method specialisations for input device class */
ClassDeviceHandler scsi_host_class_handler = {
	class_device_init,                  /**< init function */
	class_device_detection_done,        /**< detection is done */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	scsi_host_class_post_process,       /**< add more properties */
	scsi_host_class_compute_udi,        /**< No UDI computation */
	"scsi_host",                        /**< sysfs class name */
	"scsi_host",                        /**< hal class name */
	FALSE,                              /**< don't require device file */
	FALSE                               /**< add as child to parent */
};

/** @} */
