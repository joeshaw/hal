/***************************************************************************
 * CVSID: $Id$
 *
 * serial port device class
 *
 * Copyright (C) 2004 Kay Sievers, <kay.sievers@vrfy.org>
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
#include "../hald.h"

#include "class_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxSerial Serial class
 * @ingroup HalDaemonLinux
 * @brief Serial class
 * @{
 */

static dbus_bool_t
serial_class_device_accept (ClassDeviceHandler *self,
			     const char *path,
			     struct sysfs_class_device *class_device)
{
	int serial_number;

	if (class_device->sysdevice == NULL)
		return FALSE;

	if (sscanf (class_device->name, "ttyUSB%d", &serial_number) == 1)
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
serial_class_pre_process (ClassDeviceHandler *self,
			  HalDevice *d,
			  const char *sysfs_path,
			  struct sysfs_class_device *class_device)
{
	hal_device_property_set_string (d, "info.product", "Serial Port");
	hal_device_property_set_string (d, "info.category", "serial");
	hal_device_property_set_string (d, "info.capabilities", "serial");
}

/** methods for device class */
ClassDeviceHandler serial_class_handler = {
	.init			= class_device_init,
	.shutdown		= class_device_shutdown,
	.tick			= class_device_tick,
	.accept			= serial_class_device_accept,
	.visit			= class_device_visit,
	.removed		= class_device_removed,
	.udev_event		= class_device_udev_event,
	.get_device_file_target = class_device_get_device_file_target,
	.pre_process		= serial_class_pre_process ,
	.post_merge		= class_device_post_merge,
	.got_udi		= class_device_got_udi,
	.compute_udi		= NULL,
	.in_gdl			= class_device_in_gdl,
	.sysfs_class_name	= "tty",
	.hal_class_name		= "serial",
	.require_device_file	= TRUE,
	.merge_or_add		= TRUE
};

/** @} */
