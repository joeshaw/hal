/***************************************************************************
 * CVSID: $Id$
 *
 * Input device class
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
#include "../hald.h"

#include "class_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxInput Input class
 * @ingroup HalDaemonLinux
 * @brief Input class
 * @{
 */


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
input_class_post_process (ClassDeviceHandler *self,
			  HalDevice *d,
			  const char *sysfs_path,
			  struct sysfs_class_device *class_device)
{
	/* add capabilities for device */
	hal_device_property_set_string (d, "info.category", "input");
	hal_device_add_capability (d, "input");

	/** @todo use some ioctl()'s on the device file (specific by property
	 *        .target_dev) and set additional properties */

	/** @todo read some data from sysfs and set additional properties */
}

/** Get the name of that the property that the device file should be put in.
 *
 *  Specialised for input device class as multiple device files belong to
 *  a single device (such as a mouse).
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  class_device  Libsysfs object representing class device
 *                        instance
 *  @param  dev_file_prop Device file property name (out)
 *  @param  dev_file_prop_len  Maximum length of string
 */
static void
input_class_get_device_file_target (ClassDeviceHandler *self,
				    HalDevice *d,
				    const char *sysfs_path,
				    struct sysfs_class_device *class_device,
				    char* dev_file_prop,
				    int dev_file_prop_len)
{
	const char *sysfs_name;

	/* since we have multiple device files for class mouse we need 
	 * to merge them into different, welldefined, properties */
	sysfs_name = get_last_element (sysfs_path);
	if (strlen (sysfs_name) > sizeof("event")-1 && 
	    strncmp (sysfs_name, "event", sizeof("event")-1) == 0) {
		strncpy(dev_file_prop, "input.device", dev_file_prop_len);
	} else {
		/* according to kernel sources if the sysfs name doesn't
		 * conform to /sys/class/input/event%d it is a device file
		 * for accessing the input device through an architecture
		 * dependent inferface (e.g. PS/2 on x86). */
		strncpy(dev_file_prop, "input.device.arch", dev_file_prop_len);
	}
}

/** Method specialisations for input device class */
ClassDeviceHandler input_class_handler = {
	class_device_init,                  /**< init function */
	class_device_detection_done,        /**< detection is done */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	input_class_get_device_file_target, /**< where to store devfile name */
	input_class_post_process,           /**< add more properties */
	NULL,                               /**< No UDI computation */
	"input",                            /**< sysfs class name */
	"input",                            /**< hal class name */
	TRUE,                               /**< require device file */
	TRUE                                /**< merge onto sysdevice */
};

/** @} */
