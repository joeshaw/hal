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
#include <fcntl.h>
#include <linux/input.h>
#include <errno.h>

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


/** Accept function for input devices.  It's like
 *  class_device_accept(), except we accept the device even if there
 *  is no sysfs device
 *
 *  @param  self                Pointer to class members
 *  @param  path                Sysfs-path for device
 *  @param  class_device        libsysfs object for class device
 */
static dbus_bool_t
input_class_accept (ClassDeviceHandler *self,
                    const char *path,
                    struct sysfs_class_device *class_device)
{
        /* If there is no sysfs device we only accept event%d devices. */
        if (class_device->sysdevice == NULL &&
            strncmp (class_device->name, "event", 5) != 0)
                return FALSE;

	/* only care about given sysfs class name */
	if (strcmp (class_device->classname, self->sysfs_class_name) == 0) {
		return TRUE;
	}

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
input_class_pre_process (ClassDeviceHandler *self,
			 HalDevice *d,
			 const char *sysfs_path,
			 struct sysfs_class_device *class_device)
{
        char name[64];
        const char *device_file;
        int major, minor;
        int fd;

	/* add capabilities for device */
	hal_device_property_set_string (d, "info.category", "input");
	hal_device_add_capability (d, "input");

	/** @todo use some ioctl()'s on the device file (specific by property
	 *        .target_dev) and set additional properties */

	/** @todo read some data from sysfs and set additional properties */

	class_device_get_major_minor (sysfs_path, &major, &minor);
	hal_device_property_set_int (d, "input.major", major);
	hal_device_property_set_int (d, "input.minor", minor);

	/* Find out what kind of input device this is */
	if (!hal_device_has_property (d, "input.device"))
                /* We're being called for for a mouse%d alias device
                 * that doesn't have the input.device property. */
                return;

        device_file = hal_device_property_get_string (d, "input.device");
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;
		
        /* If there is no corresponding sysfs device we ask the event
         * layer for a name for this device.*/
        if (class_device->sysdevice == NULL) {
                if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0) {
                        hal_device_property_set_string (d, "info.product",
                                                        name);
                } else
		    HAL_ERROR(("EVIOCGNAME failed: %s\n", strerror(errno)));
        }

        /* @todo add more ioctl()'s here */

        close(fd);
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


/** Compute the device udi for input devices that do not have a sysfs
 *  device file.
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until
 *                              the next invocation of this function
 */
static char *
input_class_compute_udi (HalDevice *d, int append_num)
{
        char *format;
        static char buf[256];

        if (append_num == -1)
                format = "/org/freedesktop/Hal/devices/input_%d_%d";
        else
                format = "/org/freedesktop/Hal/devices/input_%d_%d-%d";
 
        snprintf (buf, 256, format,
                  hal_device_property_get_int (d, "input.major"),
                  hal_device_property_get_int (d, "input.minor"), append_num);
 
        return buf;
  
}

/** Method specialisations for input device class */
ClassDeviceHandler input_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	input_class_accept,                 /**< accept function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	input_class_get_device_file_target, /**< where to store devfile name */
	input_class_pre_process,            /**< add more properties */
	class_device_post_merge,            /**< post merge function */
	class_device_got_udi,               /**< got UDI */
	input_class_compute_udi,            /**< UDI computation */
	class_device_in_gdl,                /**< in GDL */
	"input",                            /**< sysfs class name */
	"input",                            /**< hal class name */
	TRUE,                               /**< require device file */
	TRUE                                /**< merge onto sysdevice */
};

/** @} */
