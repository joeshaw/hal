/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_i2c_adapter.c : I2C functions for sysfs-agent on Linux 2.6
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
#include "linux_class_i2c_adapter.h"

/**
 * @defgroup HalDaemonLinuxI2cAdapter I2C adapter class
 * @ingroup HalDaemonLinux
 * @brief I2C adapter class
 * @{
 */


/** This function will compute the device uid based on other properties
 *  of the device. For I2C adapters it is the adapter number.
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char *
i2c_adapter_compute_udi (HalDevice * d, int append_num)
{
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/i2c_adapter_%d";
	else
		format = "/org/freedesktop/Hal/devices/i2c_adapter_%d-%d";

	snprintf (buf, 256, format,
		  ds_property_get_int (d, "i2c_adapter.adapter"),
		  append_num);

	return buf;
}


/* fwd decl */
static void visit_class_device_i2c_adapter_got_parent (HalDevice * parent,
						       void *data1,
						       void *data2);

/** Visitor function for I2C adapter.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void
visit_class_device_i2c_adapter (const char *path,
				struct sysfs_class_device *class_device)
{
	HalDevice *d;
	struct sysfs_attribute *cur;
	char *parent_sysfs_path;
	char *product_name;
	char attr_name[SYSFS_NAME_LEN];
	const char *last_elem;
	int adapter_num;
	int len;
	int i;

	if (class_device->sysdevice == NULL) {
		HAL_INFO (("Skipping virtual class device at path %s\n",
			   path));
		return;
	}

	HAL_INFO (("i2c_adapter: sysdevice_path=%s, path=%s\n",
		   class_device->sysdevice->path, path));

	/** @todo: see if we already got this device */

	d = ds_device_new ();
	ds_property_set_string (d, "info.bus", "i2c_adapter");
	ds_property_set_string (d, "linux.sysfs_path", path);
	ds_property_set_string (d, "linux.sysfs_path_device",
				class_device->sysdevice->path);

	/* Sets last_elem to i2c-2 in path=/sys/class/i2c-adapter/i2c-2 */
	last_elem = get_last_element (path);
	sscanf (last_elem, "i2c-%d", &adapter_num);
	ds_property_set_int (d, "i2c_adapter.adapter", adapter_num);

	/* guestimate product name */
	dlist_for_each_data (sysfs_get_device_attributes
			     (class_device->sysdevice), cur,
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

	ds_property_set_string (d, "info.product",
				"I2C Adapter Interface");
	if (product_name == NULL)
		ds_property_set_string (d, "i2c_adapter.name",
					"I2C Adapter Interface");
	else
		ds_property_set_string (d, "i2c_adapter.name",
					product_name);

	parent_sysfs_path =
	    get_parent_sysfs_path (class_device->sysdevice->path);

	/* Find parent; this happens asynchronously as our parent might
	 * be added later. If we are probing this can't happen so the
	 * timeout is set to zero in that event..
	 */
	ds_device_async_find_by_key_value_string
	    ("linux.sysfs_path_device", parent_sysfs_path, TRUE,
	     visit_class_device_i2c_adapter_got_parent, (void *) d, NULL,
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
visit_class_device_i2c_adapter_got_parent (HalDevice * parent,
					   void *data1, void *data2)
{
	char *new_udi = NULL;
	HalDevice *new_d = NULL;
	HalDevice *d = (HalDevice *) data1;

	/*printf("parent=0x%08x\n", parent); */

	if (parent != NULL) {
		ds_property_set_string (d, "info.parent", parent->udi);
		find_and_set_physical_device (d);
		ds_property_set_bool (d, "info.virtual", TRUE);
	} else {
		HAL_ERROR (("No parent for I2C adapter device!"));
		ds_device_destroy (d);
		return;
	}

	/* Add the i2c_adapter capability to our parent device */
	ds_add_capability (parent, "i2c_adapter");

	/* Compute a proper UDI (unique device id) and try to locate a persistent
	 * unplugged device or simple add this new device...
	 */
	new_udi =
	    rename_and_merge (d, i2c_adapter_compute_udi, "i2c_adapter");
	if (new_udi != NULL) {
		new_d = ds_device_find (new_udi);
		if (new_d != NULL) {
			ds_gdl_add (new_d);
		}
	}
}


/** Init function for I2C adapter class handling
 *
 */
void
linux_class_i2c_adapter_init ()
{
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void
linux_class_i2c_adapter_detection_done ()
{
}

/** Shutdown function for I2C adapter class handling
 *
 */
void
linux_class_i2c_adapter_shutdown ()
{
}

/** @} */
