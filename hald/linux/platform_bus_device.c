/***************************************************************************
 * CVSID: $Id$
 *
 * Platform bus devices
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
#include <stdarg.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/fd.h>

#include "../logger.h"
#include "../device_store.h"
#include "bus_device.h"
#include "common.h"


static dbus_bool_t
platform_device_accept (BusDeviceHandler *self, const char *path, 
			struct sysfs_device *device)
{
	int fd;
	int number;
	char device_file[256];
	char fd_sysfs_path[256];
	char name[256];
	struct floppy_drive_struct ds;

	if (strcmp (device->bus, "platform") != 0)
		return FALSE;

	/* Only support floppies */
	if (strncmp (device->bus_id, "floppy", 6) != 0)
		return FALSE;

	sscanf (device->bus_id, "floppy%d", &number);

	/* get device file */
	snprintf (fd_sysfs_path, sizeof (fd_sysfs_path), "%s/block/fd%d", 
		  sysfs_mount_path, number);

	if (!class_device_get_device_file (fd_sysfs_path, 
					   device_file, 
					   sizeof (device_file))) {
		HAL_ERROR (("Could not get device file floppy %d", number));
		return FALSE;
	}

	/* Check that there actually is a drive at the other end */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		HAL_ERROR (("Could not open %s", device_file));
		return FALSE;
	}
	
	/* @todo Could use the name here */
	ioctl (fd, FDRESET, NULL);
	if (ioctl (fd, FDGETDRVTYP, name) != 0) {
		HAL_ERROR (("FDGETDRVTYP failed for %s", device_file));
		close (fd);
		return FALSE;
	}
	HAL_INFO (("name is '%s'", name));

	if (ioctl (fd, FDPOLLDRVSTAT, &ds)) {
		HAL_ERROR (("FDPOLLDRVSTAT failed for %s", device_file));
		close (fd);
		return FALSE;
	}
	close (fd);
	
	if (ds.track < 0) {
		HAL_ERROR (("floppy drive %s seems not to exist", device_file));
		return FALSE;
	}
	

	return TRUE;
}

static char *
platform_device_compute_udi (HalDevice *d, int append_num)
{
	static char buf[256];

	if (append_num == -1)
		sprintf (buf, "/org/freedesktop/Hal/devices/legacy_floppy_%d",
			 hal_device_property_get_int (d, "storage.legacy_floppy.number"));
	else
		sprintf (buf, "/org/freedesktop/Hal/devices/legacy_floppy_%d/%d",
			 hal_device_property_get_int (d, "storage.legacy_floppy.number"),
			 append_num);

	return buf;
}

static void
platform_device_got_udi (BusDeviceHandler *self,
			HalDevice *d,
			const char *udi)
{
	hal_device_property_set_string (d, "block.storage_device", udi);
}

static void 
platform_device_pre_process (BusDeviceHandler *self,
			     HalDevice *d,
			     const char *sysfs_path,
			     struct sysfs_device *device)
{
	int major;
	int minor;
	int number;
	char device_file[256];
	char fd_sysfs_path[256];

	sscanf (device->bus_id, "floppy%d", &number);
	hal_device_property_set_int (d, "storage.legacy_floppy.number", number);

	hal_device_property_set_string (d, "info.product", "Legacy Floppy Drive");
	hal_device_property_set_string (d, "info.vendor", "");

	/* All the following is a little but of cheating, but hey, what
	 * can you do when the kernel doesn't properly export legacy
	 * floppy drives properly.
	 *
	 * @todo Fix the kernel to support legacy floppies properly in
	 * sysfs
	 */

	snprintf (fd_sysfs_path, sizeof (fd_sysfs_path), "%s/block/fd%d", 
		  sysfs_mount_path, number);

	if (!class_device_get_device_file (fd_sysfs_path, 
					   device_file, 
					   sizeof (device_file))) {
		HAL_ERROR (("Could not get device file floppy %d", number));
		return;
	}
	
	class_device_get_major_minor (fd_sysfs_path, &major, &minor);

	hal_device_property_set_string (d, "block.device", device_file);
	hal_device_property_set_bool (d, "block.is_volume", FALSE);
	hal_device_property_set_int (d, "block.major", major);
	hal_device_property_set_int (d, "block.minor", minor);
	hal_device_property_set_bool (d, "block.no_partitions", TRUE);
	hal_device_property_set_bool (d, "block.have_scanned", FALSE);

	hal_device_property_set_string (d, "storage.bus", "platform");
	hal_device_property_set_string (d, "storage.drive_type", "floppy");
	hal_device_property_set_bool (d, "storage.hotpluggable", FALSE);
	hal_device_property_set_bool (d, "storage.removable", TRUE);
	hal_device_property_set_bool (d, "storage.media_check_enabled", FALSE);
	hal_device_property_set_bool (d, "storage.automount_enabled_hint", FALSE);
	hal_device_property_set_bool (d, "storage.no_partitions_hint", TRUE);
	hal_device_property_set_string (d, "storage.physical_device", "");
	
	hal_device_property_set_string (d, "storage.vendor", "");
	hal_device_property_set_string (d, "storage.model", "Floppy Drive");

	hal_device_property_set_string (d, "storage.physical_device", 
					"/org/freedesktop/Hal/devices/computer");

	hal_device_property_set_string (d, "info.parent", 
					"/org/freedesktop/Hal/devices/computer");

	hal_device_add_capability (d, "block");
	hal_device_add_capability (d, "storage");
	hal_device_property_set_string (d, "info.category", 
					"storage");
}

static void 
platform_device_in_gdl (BusDeviceHandler *self,
		   	HalDevice *d,
		   	const char *udi)
{
	if (hal_device_has_capability (d, "storage")) {
		/* Check the mtab to see if the device is mounted */
		etc_mtab_process_all_block_devices (TRUE);
	}
}


/** Method specialisations for bustype pci */
BusDeviceHandler platform_bus_handler = {
	bus_device_init,              /**< init function */
	bus_device_shutdown,          /**< shutdown function */
	bus_device_tick,              /**< timer function */
	platform_device_accept,       /**< accept function */
 	bus_device_visit,             /**< visitor function */
	bus_device_removed,           /**< device is removed */
	platform_device_compute_udi,  /**< UDI computing function */
	platform_device_pre_process,  /**< add more properties */
	platform_device_got_udi,      /**< got UDI */
	platform_device_in_gdl,       /**< in GDL */
	"platform",                   /**< sysfs bus name */
	"platform"                    /**< namespace */
};


/** @} */
