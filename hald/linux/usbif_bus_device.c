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
#include "../hald.h"
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
 */
static dbus_bool_t
usbif_device_accept (BusDeviceHandler *self, const char *path, 
		     struct sysfs_device *device)
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
		format = "/org/freedesktop/Hal/devices/usb_%s_%d";
	else
		format = "/org/freedesktop/Hal/devices/usb_%s_%d-%d";

	/*hal_device_print (d);*/

	pd = hal_device_property_get_string (d, "info.parent");
	len = strlen (pd);
	for (i = len - 1; pd[i] != '/' && i >= 0; i--);
	name = pd + i + 1;

	snprintf (buf, 256, format,
		  name,
		  hal_device_property_get_int (d, "usb.interface.number"), 
		  append_num);

	return buf;
}


static void 
compute_name_from_if (HalDevice *d)
{
	int c, s, p;

	c = hal_device_property_get_int (d, "usb.interface.class");
	s = hal_device_property_get_int (d, "usb.interface.subclass");
	p = hal_device_property_get_int (d, "usb.interface.protocol");

	hal_device_property_set_string (
		d, "info.product", "USB Interface");

	/* Just do this for a couple of mainstream interfaces. GUI
	 * Device Managers should do this themselves (for i18n reasons)
	 *
	 * See http://www.linux-usb.org/usb.ids for details */
	switch (c)
	{
	case 0x01:
		hal_device_property_set_string (
			d, "info.product", "USB Audio Interface");
		break;

	case 0x02:
		hal_device_property_set_string (
			d, "info.product", "USB Communications Interface");
		break;

	case 0x03:
		hal_device_property_set_string (
			d, "info.product", "USB HID Interface");
		break;

	case 0x06:
		if (s==0x01 && p==0x01)
			hal_device_property_set_string (
				d, "info.product", "USB PTP Interface");
		break;

	case 0x07:
		hal_device_property_set_string (
			d, "info.product", "USB Printing Interface");
		break;

	case 0x08:
		hal_device_property_set_string (
			d, "info.product", "USB Mass Storage Interface");
		break;

	case 0x09:
		hal_device_property_set_string (
			d, "info.product", "USB Hub Interface");
		break;

	case 0x0a:
		hal_device_property_set_string (
			d, "info.product", "USB Data Interface");
		break;

	case 0x0e:
		hal_device_property_set_string (
			d, "info.product", "USB Video Interface");
		break;

	case 0xe0:
		if (s==0x01 && p==0x01)
			hal_device_property_set_string (
				d, "info.product", "USB Bluetooth Interface");
		break;

	case 0xff:
		hal_device_property_set_string (
			d, "info.product", "Vendor Specific Interface");
		break;
	}
}


static void 
usbif_device_pre_process (BusDeviceHandler *self,
			  HalDevice *d,
			  const char *sysfs_path,
			  struct sysfs_device *device)
{
	int i;
	int len;
	struct sysfs_attribute *cur;
	char attr_name[SYSFS_NAME_LEN];
	const char *parent_udi;
	HalDevice *parent_device;

	/* Merge all the usb_device.* properties into the usb.* namespace */
	parent_udi = hal_device_property_get_string (d, "info.parent");
	parent_device = hal_device_store_find (hald_get_gdl (), parent_udi);
	hal_device_merge_with_rewrite (d, parent_device, "usb.", "usb_device.");
	/* But maintain the correct usb.linux.sysfs_path property */
	hal_device_property_set_string (d, "usb.linux.sysfs_path", 
					device->path);


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
			hal_device_property_set_int (d, "usb.interface.class",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bInterfaceSubClass") == 0)
			hal_device_property_set_int (d, "usb.interface.subclass",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bInterfaceProtocol") == 0)
			hal_device_property_set_int (d, "usb.interface.protocol",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bInterfaceNumber") == 0)
			hal_device_property_set_int (d, "usb.interface.number",
					     parse_dec (cur->value));
	}

	compute_name_from_if (d);
}

/** Method specialisations for bustype usbif */
BusDeviceHandler usbif_bus_handler = {
	bus_device_init,           /**< init function */
	bus_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	usbif_device_accept,       /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	usbif_device_compute_udi,  /**< UDI computing function */
	usbif_device_pre_process,  /**< add more properties */
	bus_device_got_udi,        /**< got UDI */
	bus_device_in_gdl,            /**< in GDL */
	"usb",                     /**< sysfs bus name */
	"usb"                      /**< namespace */
};

/** @} */
