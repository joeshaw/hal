/***************************************************************************
 * CVSID: $Id$
 *
 * IEEE1394 node class device
 *
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

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "class_device.h"
#include "common.h"

static char *
ieee1394_node_class_compute_udi (HalDevice *d, int append_num)
{
	static int counter = 0;
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/ieee1394_node_%d";
	else
		format = "/org/freedesktop/Hal/devices/ieee1394_node_%d-%d";

	snprintf (buf, 256, format, counter, append_num);

	return buf;
}

static void
ieee1394_node_class_pre_process (ClassDeviceHandler *self,
				 HalDevice *d,
				 const char *sysfs_path,
				 struct sysfs_class_device *class_device)
{
	struct sysfs_attribute *cur;
	char attr_name[SYSFS_NAME_LEN];
	int tmp;
	const char *vendor_name = NULL;
	int vendor_id;

	dlist_for_each_data (sysfs_get_device_attributes (class_device->sysdevice),
			     cur, struct sysfs_attribute) {
		int len, i;

		if (sysfs_get_name_from_path (cur->path,
					      attr_name,
					      SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; i >= 0 && isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		if (strcmp (attr_name, "capabilities") == 0) {
			tmp = parse_hex (cur->value);

			hal_device_property_set_int (d,
						     "ieee1394.capabilities",
						     tmp);
		} else if (strcmp (attr_name, "guid") == 0) {
			tmp = parse_hex (cur->value);

			hal_device_property_set_int (d, "ieee1394.guid", tmp);
		} else if (strcmp (attr_name, "nodeid") == 0) {
			tmp = parse_hex (cur->value);

			hal_device_property_set_int (d, "ieee1394.nodeid",
						     tmp);
		} else if (strcmp (attr_name, "vendor_id") == 0) {
			vendor_id = parse_hex (cur->value);
		} else if (strcmp (attr_name, "vendor_name_kv") == 0) {
			vendor_name = cur->value;
		}
	}

	hal_device_property_set_int (d, "ieee1394.vendor_id", vendor_id);

	if (vendor_name) {
		hal_device_property_set_string (d, "ieee1394.vendor",
						vendor_name);
		hal_device_property_set_string (d, "info.vendor", vendor_name);
	} else {
		char numeric_name[32];

		snprintf (numeric_name, sizeof (numeric_name),
			  "Unknown (0x%04x)", vendor_id);

		hal_device_property_set_string (d, "ieee1394.vendor",
						numeric_name);
		hal_device_property_set_string (d, "info.vendor",
						numeric_name);
	}

	hal_device_property_set_bool (d, "info.virtual", TRUE);
}
		  
/** Method specialisations for ieee1394_node device class */
ClassDeviceHandler ieee1394_node_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	class_device_accept,                /**< accept function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	ieee1394_node_class_pre_process,    /**< add more properties */
	class_device_post_merge,            /**< post merge function */
	class_device_got_udi,               /**< got UDI */
	ieee1394_node_class_compute_udi,    /**< No UDI computation */
	"ieee1394_node",                    /**< sysfs class name */
	"ieee1394_node",                    /**< hal class name */
	FALSE,                              /**< don't require device file */
	FALSE                               /**< add as child to parent */
};
