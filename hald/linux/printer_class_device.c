/***************************************************************************
 * CVSID: $Id$
 *
 * Printer device class
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2004 Novell, Inc.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "class_device.h"
#include "common.h"

/* Stolen from kernel 2.6.4, drivers/usb/class/usblp.c */
#define IOCNR_GET_DEVICE_ID 1
#define LPIOC_GET_DEVICE_ID(len) _IOC(_IOC_READ, 'P', IOCNR_GET_DEVICE_ID, len)

/**
 * @defgroup HalDaemonLinuxPrinter Printer class
 * @ingroup HalDaemonLinux
 * @brief Printer class
 * @{
 */

static dbus_bool_t
printer_class_device_accept (ClassDeviceHandler *self,
			     const char *path,
			     struct sysfs_class_device *class_device)
{
	int lp_number;

	if (class_device->sysdevice == NULL)
		return FALSE;

	if (sscanf (class_device->name, "lp%d", &lp_number) == 1)
		return TRUE;

	return FALSE;
}

/** This method is called just before the device is either merged
 *  onto the sysdevice or added to the GDL (cf. merge_or_add). 
 *  This is useful for extracting more information about the device
 *  through e.g. ioctl's using the device file property and also
 *  for setting info.category|capability.
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  class_device  Libsysfs object representing class device
 *                        instance
 */
static void 
printer_class_pre_process (ClassDeviceHandler *self,
			   HalDevice *d,
			   const char *sysfs_path,
			   struct sysfs_class_device *class_device)
{
	int fd;
	char device_id[1024];
	char **props, **iter;
	char *mfg = NULL, *model = NULL, *serial = NULL, *desc = NULL;

	/* add capabilities for device */
	hal_device_property_set_string (d, "info.category", "printer");
	hal_device_add_capability (d, "printer");

	fd = open (hal_device_property_get_string (d, "printer.device"),
		   O_RDWR | O_EXCL);

	if (fd < 0)
		return;

	if (ioctl (fd, LPIOC_GET_DEVICE_ID (sizeof (device_id)),
		   device_id) < 0) {
		close (fd);
		return;
	}

	close (fd);

	HAL_TRACE (("printer IEEE-1284 device_id: %s", device_id+2));

	props = g_strsplit (device_id+2, ";", 0);
	for (iter = props; *iter != NULL; iter++) {
		if (strncmp (*iter, "MANUFACTURER:", 13) == 0)
			mfg = *iter + 13;
		else if (strncmp (*iter, "MFG:", 4) == 0)
			mfg = *iter + 4;
		else if (strncmp (*iter, "MODEL:", 6) == 0)
			model = *iter + 6;
		else if (strncmp (*iter, "MDL:", 4) == 0)
			model = *iter + 4;
		else if (strncmp (*iter, "SN:", 3) == 0)
			serial = *iter + 3;
		else if (strncmp (*iter, "SERN:", 5) == 0)
			serial = *iter + 5;
		else if (strncmp (*iter, "SERIALNUMBER:", 13) == 0)
			serial = *iter + 13;
		else if (strncmp (*iter, "DES:", 4) == 0)
			desc = *iter + 4;
		else if (strncmp (*iter, "DESCRIPTION:", 12) == 0)
			desc = *iter + 12;
	}

	if (mfg != NULL) {
		hal_device_property_set_string (d, "info.vendor", mfg);
		hal_device_property_set_string (d, "printer.vendor", mfg);
	}		

	if (model != NULL) {
		hal_device_property_set_string (d, "info.product", model);
		hal_device_property_set_string (d, "printer.product", model);
	}

	if (serial != NULL)
		hal_device_property_set_string (d, "printer.serial", serial);

	if (desc != NULL) {
		hal_device_property_set_string (d, "printer.description",
						desc);
	}

	g_strfreev (props);
}

/** Method specialisations for input device class */
ClassDeviceHandler printer_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	printer_class_device_accept,        /**< accept function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	printer_class_pre_process,          /**< add more properties */
	class_device_post_merge,            /**< post merge function */
	class_device_got_udi,               /**< got UDI */
	NULL,                               /**< No UDI computation */
	class_device_in_gdl,                /**< in GDL */
	"usb",                              /**< sysfs class name */
	"printer",                          /**< hal class name */
	TRUE,                               /**< require device file */
	TRUE                                /**< merge onto sysdevice */
};

/** @} */
