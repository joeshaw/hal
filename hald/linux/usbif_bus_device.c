/***************************************************************************
 * CVSID: $Id$
 *
 * linux_usb.c : USB handling on Linux 2.6
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
#include "bus_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxUsbIF USB interfaces
 * @ingroup HalDaemonLinux
 * @brief USB interfaces
 * @{
 */

/** Specialised accept function since both USB devices and USB interfaces
 *  share the same bus name
 *
 *  @param  self                Pointer to class members
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 *  @param  is_probing          Set to TRUE only on initial detection
 */
static dbus_bool_t
usbif_device_accept (BusDeviceHandler *self, const char *path, 
		     struct sysfs_device *device, dbus_bool_t is_probing)
{
	unsigned int i;

	if (strcmp (device->bus, "usb") != 0)
		return FALSE;

	/* only USB interfaces got a : in the bus_id */
	for (i = 0; device->bus_id[i] != 0; i++) {
		if (device->bus_id[i] == ':') {
			return TRUE;
		}
	}

	return FALSE;
}

static char *
usbif_device_compute_udi (HalDevice *d, int append_num)
{
	int i, len;
	const char *format;
	const char *pd;
	const char *name;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/usbif_%s_%d";
	else
		format = "/org/freedesktop/Hal/devices/usbif_%s_%d-%d";

	hal_device_print (d);

	pd = hal_device_property_get_string (d, "info.parent");
	len = strlen (pd);
	for (i = len - 1; pd[i] != '/' && i >= 0; i--);
	name = pd + i + 1;

	snprintf (buf, 256, format,
		  name,
		  hal_device_property_get_int (d, "usbif.number"), append_num);

	return buf;
}


static void 
usbif_device_post_process (BusDeviceHandler *self,
			   HalDevice *d,
			   const char *sysfs_path,
			   struct sysfs_device *device)
{
	int i;
	int len;
	struct sysfs_attribute *cur;
	char attr_name[SYSFS_NAME_LEN];

	dlist_for_each_data (sysfs_get_device_attributes (device), cur,
			     struct sysfs_attribute) {

		if (sysfs_get_name_from_path (cur->path,
					      attr_name,
					      SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; i > 0 && isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		/*printf("attr_name=%s -> '%s'\n", attr_name, cur->value); */

		if (strcmp (attr_name, "bInterfaceClass") == 0)
			hal_device_property_set_int (d, "usbif.interface_class",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bInterfaceSubClass") == 0)
			hal_device_property_set_int (d, "usbif.interface_subclass",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bInterfaceProtocol") == 0)
			hal_device_property_set_int (d, "usbif.interface_protocol",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bInterfaceNumber") == 0)
			hal_device_property_set_int (d, "usbif.number",
					     parse_dec (cur->value));
	}

	hal_device_property_set_bool (d, "info.virtual", TRUE);
}

/** Method specialisations for bustype usbif */
BusDeviceHandler usbif_bus_handler = {
	bus_device_init,           /**< init function */
	bus_device_detection_done, /**< detection is done */
	bus_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	usbif_device_accept,       /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	usbif_device_compute_udi,  /**< UDI computing function */
	usbif_device_post_process, /**< add more properties */
	"usb",                     /**< sysfs bus name */
	"usbif"                    /**< namespace */
};

/** @} */
