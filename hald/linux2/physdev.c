/***************************************************************************
 * CVSID: $Id$
 *
 * physdev.c : Handling of physical kernel devices 
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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

#include <stdio.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

#include "util.h"
#include "coldplug.h"
#include "hotplug_helper.h"

#include "hotplug.h"
#include "physdev.h"

#include "ids.h"

static gboolean
pci_add (const gchar *sysfs_path, HalDevice *parent, void *end_token)
{
	HalDevice *d;
	gint device_class;
	gchar udi[256];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "pci");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_device_property_set_string (d, "pci.linux.sysfs_path", sysfs_path);

	hal_util_set_int_from_file (d, "pci.product_id", sysfs_path, "device", 16);
	hal_util_set_int_from_file (d, "pci.vendor_id", sysfs_path, "vendor", 16);
	hal_util_set_int_from_file (d, "pci.subsys_product_id", sysfs_path, "subsystem_device", 16);
	hal_util_set_int_from_file (d, "pci.subsys_vendor_id", sysfs_path, "subsystem_vendor", 16);

	if (hal_util_get_int_from_file (sysfs_path, "class", &device_class, 16)) {
		hal_device_property_set_int (d, "pci.device_class", ((device_class >> 16) & 0xff));
		hal_device_property_set_int (d, "pci.device_subclass", ((device_class >> 8) & 0xff));
		hal_device_property_set_int (d, "pci.device_protocol", (device_class & 0xff));
	}

	{
		gchar buf[64];
		char *vendor_name;
		char *product_name;
		char *subsys_vendor_name;
		char *subsys_product_name;

		ids_find_pci (hal_device_property_get_int (d, "pci.vendor_id"), 
			      hal_device_property_get_int (d, "pci.product_id"), 
			      hal_device_property_get_int (d, "pci.subsys_vendor_id"), 
			      hal_device_property_get_int (d, "pci.subsys_product_id"), 
			      &vendor_name, &product_name, &subsys_vendor_name, &subsys_product_name);

		if (vendor_name != NULL) {
			hal_device_property_set_string (d, "pci.vendor", vendor_name);
			hal_device_property_set_string (d, "info.vendor", vendor_name);
		} else {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
				    hal_device_property_get_int (d, "pci.vendor_id"));
			hal_device_property_set_string (d, "pci.vendor", buf);
			hal_device_property_set_string (d, "info.vendor", buf);
		}

		if (product_name != NULL) {
			hal_device_property_set_string (d, "pci.product", product_name);
			hal_device_property_set_string (d, "info.product", product_name);
		} else {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
				    hal_device_property_get_int (d, "pci.product_id"));
			hal_device_property_set_string (d, "pci.product", buf);
			hal_device_property_set_string (d, "info.product", buf);
		}

		if (subsys_vendor_name != NULL) {
			hal_device_property_set_string (d, "pci.subsys_vendor", subsys_vendor_name);
		} else {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
				    hal_device_property_get_int (d, "pci.subsys_vendor_id"));
			hal_device_property_set_string (d, "pci.subsys_vendor", buf);
		}

		if (subsys_product_name != NULL) {
			hal_device_property_set_string (d, "pci.subsys_product", subsys_product_name);
		} else {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
				    hal_device_property_get_int (d, "pci.subsys_product_id"));
			hal_device_property_set_string (d, "pci.subsys_product", buf);
		}
	}

	/* Add to temporary device store */
	hal_device_store_add (hald_get_tdl (), d);

	/* Merge properties from .fdi files */
	di_search_and_merge (d);

	/* TODO: Run callouts */

	/* Compute UDI */
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pci_%x_%x",
			      hal_device_property_get_int (d, "pci.vendor_id"),
			      hal_device_property_get_int (d, "pci.product_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);
	
	hotplug_event_end (end_token);
	return TRUE;
}

static void 
usbif_set_name (HalDevice *d, int ifclass, int ifsubclass, int ifprotocol)
{
	const char *name;

	switch (ifclass) {
	default:
	case 0x00:
		name = "USB Interface";
		break;
	case 0x01:
		name = "USB Audio Interface";
		break;
	case 0x02:
		name = "USB Communications Interface";
		break;
	case 0x03:
		name = "USB HID Interface";
		break;
	case 0x06:
		name = "USB Imaging Interface";
		break;
	case 0x07:
		name = "USB Printer Interface";
		break;
	case 0x08:
		name = "USB Mass Storage Interface";
		break;
	case 0x09:
		name = "USB Hub Interface";
		break;
	case 0x0a:
		name = "USB Data Interface";
		break;
	case 0x0b:
		name = "USB Chip/Smartcard Interface";
		break;
	case 0x0d:
		name = "USB Content Security Interface";
		break;
	case 0x0e:
		name = "USB Video Interface";
		break;
	case 0xdc:
		name = "USB Diagnostic Interface";
		break;
	case 0xe0:
		name = "USB Wireless Interface";
		break;
	case 0xef:
		name = "USB Miscelleneous Interface";
		break;
	case 0xfe:
		name = "USB Application Specific Interface";
		break;
	case 0xff:
		name = "USB Vendor Specific Interface";
		break;
	}

	hal_device_property_set_string (d, "usb.product", name);
	hal_device_property_set_string (d, "info.product", name);
}

static gboolean
usb_add (const gchar *sysfs_path, HalDevice *parent, void *end_token)
{
	HalDevice *d;
	const gchar *bus_id;
	gchar udi[256];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	}

	/* only USB interfaces got a : in the bus_id */
	bus_id = hal_util_get_last_element (sysfs_path);
	if (strchr (bus_id, ':') == NULL) {
		gint bmAttributes;

		hal_device_property_set_string (d, "info.bus", "usb_device");

		hal_device_property_set_string (d, "usb_device.linux.sysfs_path", sysfs_path);

		hal_util_set_int_from_file (d, "usb_device.configuration_value", sysfs_path, "bConfigurationValue", 10);
		hal_util_set_int_from_file (d, "usb_device.num_configurations", sysfs_path, "bNumConfigurations", 10);
		hal_util_set_int_from_file (d, "usb_device.num_interfaces", sysfs_path, "bNumInterfaces", 10);

		hal_util_set_int_from_file (d, "usb_device.device_class", sysfs_path, "bDeviceClass", 16);
		hal_util_set_int_from_file (d, "usb_device.device_subclass", sysfs_path, "bDeviceSubClass", 16);
		hal_util_set_int_from_file (d, "usb_device.device_protocol", sysfs_path, "bDeviceProtocol", 16);

		hal_util_set_int_from_file (d, "usb_device.vendor_id", sysfs_path, "idVendor", 16);
		hal_util_set_int_from_file (d, "usb_device.product_id", sysfs_path, "idProduct", 16);

		{
			gchar buf[64];
			char *vendor_name;
			char *product_name;

			ids_find_usb (hal_device_property_get_int (d, "usb_device.vendor_id"), 
				      hal_device_property_get_int (d, "usb_device.product_id"), 
				      &vendor_name, &product_name);

			if (vendor_name != NULL) {
				hal_device_property_set_string (d, "usb_device.vendor", vendor_name);
			} else {
				if (!hal_util_set_string_from_file (d, "usb_device.vendor", 
								    sysfs_path, "manufacturer")) {
					g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
						    hal_device_property_get_int (d, "usb_device.vendor_id"));
					hal_device_property_set_string (d, "usb_device.vendor", buf); 
				}
			}
			hal_device_property_set_string (d, "info.vendor",
							hal_device_property_get_string (d, "usb_device.vendor"));

			if (product_name != NULL) {
				hal_device_property_set_string (d, "usb_device.product", product_name);
			} else {
				if (!hal_util_set_string_from_file (d, "usb_device.product", 
								    sysfs_path, "product")) {
					g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
						    hal_device_property_get_int (d, "usb_device.product_id"));
					hal_device_property_set_string (d, "usb_device.product", buf); 
				}
			}
			hal_device_property_set_string (d, "info.product",
							hal_device_property_get_string (d, "usb_device.product"));
		}

		hal_util_set_int_from_file (d, "usb_device.device_revision_bcd", sysfs_path, "bcdDevice", 16);

		hal_util_set_int_from_file (d, "usb_device.max_power", sysfs_path, "bMaxPower", 10);
		hal_util_set_int_from_file (d, "usb_device.num_ports", sysfs_path, "maxchild", 10);
		hal_util_set_int_from_file (d, "usb_device.linux.device_number", sysfs_path, "devnum", 10);

		hal_util_set_string_from_file (d, "usb_device.serial", sysfs_path, "serial");

		hal_util_set_string_from_file (d, "usb_device.serial", sysfs_path, "serial");
		hal_util_set_bcd2_from_file (d, "usb_device.speed_bcd", sysfs_path, "speed");
		hal_util_set_bcd2_from_file (d, "usb_device.version_bcd", sysfs_path, "version");

		hal_util_get_int_from_file (sysfs_path, "bmAttributes", &bmAttributes, 16);
		hal_device_property_set_bool (d, "usb_device.is_self_powered", (bmAttributes & 0x40) != 0);
		hal_device_property_set_bool (d, "usb_device.can_wake_up", (bmAttributes & 0x20) != 0);

		if (strncmp (bus_id, "usb", 3) == 0)
			hal_device_property_set_int (d, "usb_device.bus_number", atoi (bus_id + 3));
		else
			hal_device_property_set_int (d, "usb_device.bus_number", atoi (bus_id));

		/* TODO:  .level_number .parent_number  */

		/* Add to temporary device store */
		hal_device_store_add (hald_get_tdl (), d);

		/* Merge properties from .fdi files */
		di_search_and_merge (d);
		
		/* TODO: Run callouts */
		
		/* Compute UDI */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "/org/freedesktop/Hal/devices/usb_device_%x_%x_%s",
				      hal_device_property_get_int (d, "usb_device.vendor_id"),
				      hal_device_property_get_int (d, "usb_device.product_id"),
				      hal_device_has_property (d, "usb_device.serial") ?
				        hal_device_property_get_string (d, "usb_device.serial") :
				        "noserial");
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
		
		/* Move from temporary to global device store */
		hal_device_store_remove (hald_get_tdl (), d);
		hal_device_store_add (hald_get_gdl (), d);

	} else {
		hal_device_property_set_string (d, "info.bus", "usb");

		/* take all usb_device.* properties from parent and make them usb.* on this object */
		hal_device_merge_with_rewrite (d, parent, "usb.", "usb_device.");

		hal_device_property_set_string (d, "usb.linux.sysfs_path", sysfs_path);

		hal_util_set_int_from_file (d, "usb.interface.number", sysfs_path, "bInterfaceNumber", 10);

		hal_util_set_int_from_file (d, "usb.interface.class", sysfs_path, "bInterfaceClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.subclass", sysfs_path, "bInterfaceSubClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.protocol", sysfs_path, "bInterfaceProtocol", 16);

		usbif_set_name (d, 
				hal_device_property_get_int (d, "usb.interface.class"),
				hal_device_property_get_int (d, "usb.interface.subclass"),
				hal_device_property_get_int (d, "usb.interface.protocol"));

		/* Add to temporary device store */
		hal_device_store_add (hald_get_tdl (), d);
		
		/* Merge properties from .fdi files */
		di_search_and_merge (d);
		
		/* TODO: Run callouts */
		
		/* Compute UDI */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_if%d",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_int (d, "usb.interface.number"));
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
		
		/* Move from temporary to global device store */
		hal_device_store_remove (hald_get_tdl (), d);
		hal_device_store_add (hald_get_gdl (), d);
	}

	hotplug_event_end (end_token);
	return TRUE;
}

static gboolean
physdev_remove (HalDevice *d, void *end_token)
{
	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	hotplug_event_end (end_token);
	return TRUE;
}

typedef struct
{
	const gchar *subsystem;
	gboolean (*add) (const gchar *sysfs_path, HalDevice *parent, void *end_token);
	gboolean (*remove) (HalDevice *d, void *end_token);
} PhysDevHandler;

static PhysDevHandler physdev_handler_pci = { 
	"pci",
	pci_add,
	physdev_remove
};

static PhysDevHandler physdev_handler_usb = { 
	"usb",
	usb_add,
	physdev_remove
};
	

static PhysDevHandler *phys_handlers[] = {
	&physdev_handler_pci,
	&physdev_handler_usb,
	NULL
};


void
hotplug_event_begin_add_physdev (const gchar *subsystem, const gchar *sysfs_path, HalDevice *parent, void *end_token)
{
	guint i;

	HAL_INFO (("phys_add: subsys=%s sysfs_path=%s, parent=0x%08x", subsystem, sysfs_path, parent));

	for (i = 0; phys_handlers [i] != NULL; i++) {
		PhysDevHandler *handler;

		handler = phys_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			if (handler->add (sysfs_path, parent, end_token)) {
				/* let the handler end the event */
				goto out;
			}
		}
	}

	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out:
	;
}

void
hotplug_event_begin_remove_physdev (const gchar *subsystem, const gchar *sysfs_path, void *end_token)
{
	guint i;
	HalDevice *d;

	HAL_INFO (("phys_rem: subsys=%s sysfs_path=%s", subsystem, sysfs_path));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), 
						     "linux.sysfs_path_device", 
						     sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device with sysfs path %s - not found", sysfs_path));
		goto out;
	}

	for (i = 0; phys_handlers [i] != NULL; i++) {
		PhysDevHandler *handler;

		handler = phys_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			if (handler->remove (d, end_token)) {
				/* let the handler end the event */
				goto out2;
			}
		}
	}

out:
	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out2:
	;
}
