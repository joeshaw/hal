/***************************************************************************
 * CVSID: $Id$
 *
 * IEEE1394 host class device
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

#include <ctype.h>

#ifdef HAVE_STRING_H
#  include <string.h>
#endif

#include <ctype.h>

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "class_device.h"
#include "common.h"

static char *
ieee1394_host_class_compute_udi (HalDevice *d, int append_num)
{
	static int counter = 0;
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/ieee1394_host_%d";
	else
		format = "/org/freedesktop/Hal/devices/ieee1394_host_%d-%d";

	snprintf (buf, 256, format, counter, append_num);

	return buf;
}

static void
ieee1394_host_class_pre_process (ClassDeviceHandler *self,
				 HalDevice *d,
				 const char *sysfs_path,
				 struct sysfs_class_device *class_device)
{
	struct sysfs_device *sysdevice;
	struct sysfs_attribute *cur;
	char attr_name[SYSFS_NAME_LEN];
	int tmp;

	sysdevice = sysfs_get_classdev_device (class_device);
	dlist_for_each_data (sysfs_get_device_attributes (sysdevice),
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

		if (strcmp (attr_name, "is_busmgr") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_bool (d, "ieee1394.is_busmgr",
						      tmp);
		} else if (strcmp (attr_name, "is_irm") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_bool (d, "ieee1394.is_irn",
						      tmp);
		} else if (strcmp (attr_name, "is_root") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_bool (d, "ieee1394.is_root",
						      tmp);
		} else if (strcmp (attr_name, "node_count") == 0) {
			tmp = parse_dec (cur->value);
			
			hal_device_property_set_int (d, "ieee1394.node_count",
						     tmp);
		} else if (strcmp (attr_name, "nodes_active") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_int (d, "ieee1394.nodes_active",
						     tmp);
		}
	}

}
		  
/** Method specialisations for ieee1394_host device class */
ClassDeviceHandler ieee1394_host_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	class_device_accept,                /**< accept function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	ieee1394_host_class_pre_process,    /**< add more properties */
	class_device_post_merge,            /**< post merge function */
	class_device_got_udi,               /**< got UDI */
	ieee1394_host_class_compute_udi,    /**< No UDI computation */
	class_device_in_gdl,                /**< in GDL */
	"ieee1394_host",                    /**< sysfs class name */
	"ieee1394_host",                    /**< hal class name */
	FALSE,                              /**< don't require device file */
	FALSE                               /**< add as child to parent */
};
