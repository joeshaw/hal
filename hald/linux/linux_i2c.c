/***************************************************************************
 * CVSID: $Id$
 *
 * linux_i2c.c : I2C handling on Linux 2.6; based on linux_pci.c
 *
 * Copyright (C) 2004 Matthew Mastracci <matt@aclaro.com>
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
#include "linux_i2c.h"

/**
 * @defgroup HalDaemonLinuxI2c I2C
 * @ingroup HalDaemonLinux
 * @brief I2C
 * @{
 */

/** This function will compute the device uid based on other properties
 *  of the device. Specifically, the following properties are required:
 *
 *   - i2c.product
 *
 *  Other properties may also be used.
 *
 *  Requirements for uid:
 *   - do not rely on bus, port etc.; we want this id to be as unique for
 *     the device as we can
 *   - make sure it doesn't rely on properties that cannot be obtained
 *     from the minimal information we can obtain on an unplug event
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char *
i2c_compute_udi (HalDevice * d, int append_num)
{
	static char buf[256];

	if (append_num == -1)
		sprintf (buf, "/org/freedesktop/Hal/devices/i2c_%s",
			 ds_property_get_string (d, "i2c.product"));
	else
		sprintf (buf, "/org/freedesktop/Hal/devices/i2c_%s/%d",
			 ds_property_get_string (d, "i2c.product"),
			 append_num);

	return buf;
}


/* fwd decl */
static void visit_device_i2c_got_parent (HalDevice * parent,
					 void *data1, void *data2);

/** Visitor function for I2C device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void
visit_device_i2c (const char *path, struct sysfs_device *device)
{
	int i;
	int len;
	HalDevice *d;
	char attr_name[SYSFS_NAME_LEN];
	struct sysfs_attribute *cur;
	char *product_name = NULL;
	const char *driver;
	char *parent_sysfs_path;

	/* Must be a new I2C device */
	d = ds_device_new ();
	ds_property_set_string (d, "info.bus", "i2c");
	ds_property_set_string (d, "info.category", "i2c");
	ds_property_set_string (d, "info.capabilities", "i2c");
	ds_property_set_string (d, "linux.sysfs_path", path);
	ds_property_set_string (d, "linux.sysfs_path_device", path);
	/** @note We also set the path here, because otherwise we can't handle
	 *  two identical devices per algorithm used in a #rename_and_merge()
	 *  The point is that we need something unique in the Bus namespace
	 */
	ds_property_set_string (d, "i2c.linux.sysfs_path", path);
	/*printf("*** created udi=%s for path=%s\n", d, path); */

	/* set driver */
	driver = drivers_lookup (path);
	if (driver != NULL)
		ds_property_set_string (d, "linux.driver", driver);

	dlist_for_each_data (sysfs_get_device_attributes (device), cur,
			     struct sysfs_attribute) {

		if (sysfs_get_name_from_path (cur->path,
					      attr_name,
					      SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		/*printf("attr_name=%s -> '%s'\n", attr_name, cur->value); */

		if (strcmp (attr_name, "name") == 0)
			product_name = cur->value;
	}

	if (product_name == NULL)
		product_name = "Unknown";

	ds_property_set_string (d, "i2c.product", product_name);

	/* Provide best-guess of name, goes in Product property; 
	 * .fdi files can override this */
	ds_property_set_string (d, "info.product", product_name);

	parent_sysfs_path = get_parent_sysfs_path (path);

	/* Find parent; this happens asynchronously as our parent might
	 * be added later. If we are probing this can't happen so the
	 * timeout is set to zero in that event..
	 */
	HAL_INFO (("For device = %s, parent = %s",
		   ds_property_get_string (d, "linux.sysfs_path"),
		   parent_sysfs_path));
	ds_device_async_find_by_key_value_string
	    ("linux.sysfs_path_device", parent_sysfs_path, TRUE,
	     visit_device_i2c_got_parent, (void *) d, NULL,
	     is_probing ? 0 : HAL_LINUX_HOTPLUG_TIMEOUT);

	free (parent_sysfs_path);
}

/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void
visit_device_i2c_got_parent (HalDevice * parent, void *data1, void *data2)
{
	char *new_udi = NULL;
	HalDevice *new_d = NULL;
	HalDevice *d = (HalDevice *) data1;

	if (parent != NULL) {
		ds_property_set_string (d, "info.parent", parent->udi);
	} else {
		/* An I2C device should always have a parent! */
		HAL_WARNING (("No parent for I2C device!"));
	}

	/* Compute a proper UDI (unique device id) and try to locate a 
	 * persistent unplugged device or simple add this new device...
	 */
	new_udi = rename_and_merge (d, i2c_compute_udi, "i2c");
	if (new_udi != NULL) {
		new_d = ds_device_find (new_udi);
		if (new_d != NULL) {
			ds_gdl_add (new_d);
		}
	}
}

/** Init function for I2C handling
 *
 */
void
linux_i2c_init ()
{
	/* get all drivers under /sys/bus/i2c/drivers */
	drivers_collect ("i2c");
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void
linux_i2c_detection_done ()
{
}

/** Shutdown function for I2C handling
 *
 */
void
linux_i2c_shutdown ()
{
}

/** @} */
