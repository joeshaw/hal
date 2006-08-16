/***************************************************************************
 * CVSID: $Id$
 *
 * physdev.c : Handling of physical kernel devices 
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.1
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../hald.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"

#include "coldplug.h"
#include "hotplug.h"
#include "hotplug_helper.h"
#include "ids.h"
#include "osspec_linux.h"

#include "physdev.h"

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pci_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	gint device_class;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "pci");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_device_property_set_string (d, "pci.linux.sysfs_path", sysfs_path);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

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

	return d;
}

static gboolean
pci_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pci_%x_%x",
			      hal_device_property_get_int (d, "pci.vendor_id"),
			      hal_device_property_get_int (d, "pci.product_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

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

static HalDevice *
usb_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	}

	/* only USB interfaces got a : in the bus_id */
	bus_id = hal_util_get_last_element (sysfs_path);
	if (strchr (bus_id, ':') == NULL) {
		gint bmAttributes;

		hal_device_property_set_string (d, "info.bus", "usb_device");

		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

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

	} else {
		hal_device_property_set_string (d, "info.bus", "usb");

		/* take all usb_device.* properties from parent and make them usb.* on this object */
		if (parent != NULL)
			hal_device_merge_with_rewrite (d, parent, "usb.", "usb_device.");

		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		hal_device_property_set_string (d, "usb.linux.sysfs_path", sysfs_path);

		hal_util_set_int_from_file (d, "usb.interface.number", sysfs_path, "bInterfaceNumber", 10);

		hal_util_set_int_from_file (d, "usb.interface.class", sysfs_path, "bInterfaceClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.subclass", sysfs_path, "bInterfaceSubClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.protocol", sysfs_path, "bInterfaceProtocol", 16);

		usbif_set_name (d, 
				hal_device_property_get_int (d, "usb.interface.class"),
				hal_device_property_get_int (d, "usb.interface.subclass"),
				hal_device_property_get_int (d, "usb.interface.protocol"));
	}

	return d;
}

static gboolean
usb_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_has_property (d, "usb.interface.number")) {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_if%d",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_int (d, "usb.interface.number"));
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
	} else {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "/org/freedesktop/Hal/devices/usb_device_%x_%x_%s",
				      hal_device_property_get_int (d, "usb_device.vendor_id"),
				      hal_device_property_get_int (d, "usb_device.product_id"),
				      hal_device_has_property (d, "usb_device.serial") ?
				        hal_device_property_get_string (d, "usb_device.serial") :
				        "noserial");
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
	}

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
ide_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	guint host, channel;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "ide");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);

	sscanf (bus_id, "%d.%d", &host, &channel);
	hal_device_property_set_int (d, "ide.host", host);
	hal_device_property_set_int (d, "ide.channel", channel);

	if (channel == 0) {
		hal_device_property_set_string (d, "info.product", "IDE device (master)");
	} else {
		hal_device_property_set_string (d, "info.product", "IDE device (slave)");
	}
	
	return d;
}

static gboolean
ide_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_ide_%d_%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_int (d, "ide.host"),
			      hal_device_property_get_int (d, "ide.channel"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pnp_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "pnp");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_string_from_file (d, "pnp.id", sysfs_path, "id");
	if (hal_device_has_property (d, "pnp.id")) {
		gchar *pnp_description;
		ids_find_pnp (hal_device_property_get_string (d, "pnp.id"), &pnp_description);
		if (pnp_description != NULL) {
			hal_device_property_set_string (d, "pnp.description", pnp_description);
			hal_device_property_set_string (d, "info.product", pnp_description);
		}
	}

	if (!hal_device_has_property (d, "info.product")) {
		gchar buf[64];
		g_snprintf (buf, sizeof (buf), "PnP Device (%s)", hal_device_property_get_string (d, "pnp.id"));
		hal_device_property_set_string (d, "info.product", buf);
	}

	
	return d;
}

static gboolean
pnp_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pnp_%s",
			      hal_device_property_get_string (d, "pnp.id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
platform_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "platform");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);

	hal_device_property_set_string (d, "platform.id", dev_id);

	g_snprintf (buf, sizeof (buf), "Platform Device (%s)", hal_device_property_get_string (d, "platform.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
platform_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/platform_%s",
			      hal_device_property_get_string (d, "platform.id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
serio_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "serio");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "serio.id", bus_id);
	if (!hal_util_set_string_from_file (d, "serio.description", sysfs_path, "description")) {
		hal_device_property_set_string (d, "serio.description", hal_device_property_get_string (d, "serio.id"));
	}
	hal_device_property_set_string (d, "info.product", hal_device_property_get_string (d, "serio.description"));
	
	return d;
}

static gboolean
serio_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_%s",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_string (d, "serio.description"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pcmcia_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	guint socket, function;
	const char *prod_id1;
	const char *prod_id2;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "pcmcia");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	bus_id = hal_util_get_last_element (sysfs_path);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	/* not sure if %d.%d means socket function - need to revisit */
	sscanf (bus_id, "%d.%d", &socket, &function);
	hal_device_property_set_int (d, "pcmcia.socket_number", socket);

	hal_util_set_string_from_file (d, "pcmcia.prod_id1", sysfs_path, "prod_id1");
	hal_util_set_string_from_file (d, "pcmcia.prod_id2", sysfs_path, "prod_id2");
	hal_util_set_string_from_file (d, "pcmcia.prod_id3", sysfs_path, "prod_id3");
	hal_util_set_string_from_file (d, "pcmcia.prod_id4", sysfs_path, "prod_id4");

	hal_util_set_int_from_file (d, "pcmcia.manf_id", sysfs_path, "manf_id", 16);
	hal_util_set_int_from_file (d, "pcmcia.card_id", sysfs_path, "card_id", 16);
	hal_util_set_int_from_file (d, "pcmcia.func_id", sysfs_path, "func_id", 16);

	prod_id1 = hal_device_property_get_string (d, "pcmcia.prod_id1");
	prod_id2 = hal_device_property_get_string (d, "pcmcia.prod_id2");

	/* Provide best-guess of vendor, goes in Vendor property */
	if (prod_id1 != NULL) {
		hal_device_property_set_string (d, "info.vendor", prod_id1);
	} else {
		char buf[50];
		g_snprintf (buf, sizeof(buf), "Unknown (0x%04x)", hal_device_property_get_int (d, "pcmcia.manf_id"));
		hal_device_property_set_string (d, "info.vendor", buf);
	}

	/* Provide best-guess of name, goes in Product property */
	if (prod_id2 != NULL) {
		hal_device_property_set_string (d, "info.product", prod_id2);
	} else {
		char buf[50];
		g_snprintf (buf, sizeof(buf), "Unknown (0x%04x)", hal_device_property_get_int (d, "pcmcia.card_id"));
		hal_device_property_set_string (d, "info.product", buf);
	}

	return d;
}

static gboolean
pcmcia_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pcmcia_%d_%d",
			      hal_device_property_get_int (d, "pcmcia.manfid1"),
			      hal_device_property_get_int (d, "pcmcia.manfid2"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
scsi_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	gint host_num, bus_num, target_num, lun_num;
	int type;

	if (parent == NULL) {
		d = NULL;
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "scsi");
	hal_device_property_set_string (d, "info.parent", parent->udi);

	bus_id = hal_util_get_last_element (sysfs_path);
	sscanf (bus_id, "%d:%d:%d:%d", &host_num, &bus_num, &target_num, &lun_num);
	hal_device_property_set_int (d, "scsi.host", host_num);
	hal_device_property_set_int (d, "scsi.bus", bus_num);
	hal_device_property_set_int (d, "scsi.target", target_num);
	hal_device_property_set_int (d, "scsi.lun", lun_num);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "info.product", "SCSI Device");

	hal_util_set_string_from_file (d, "scsi.model", sysfs_path, "model");
	hal_util_set_string_from_file (d, "scsi.vendor", sysfs_path, "vendor");
	hal_util_get_int_from_file (sysfs_path, "type", &type, 0);
	HAL_INFO (("%s/type -> %d (-> scsi.type)", sysfs_path, type));
	switch (type) {
	case 0:
		/* Disk */
	case 14:
		/* TYPE_RBC (Reduced Block Commands)
		 * Simple Direct Access Device, set it to disk
		 * (some Firewire Disks use it)
		 */
		hal_device_property_set_string (d, "scsi.type", "disk");
		break;
	case 1:
		/* Tape */
		hal_device_property_set_string (d, "scsi.type", "tape");
		break;
	case 4:
		/* WORM */
	case 5:
		/* CD-ROM */
		hal_device_property_set_string (d, "scsi.type", "cdrom");
		break;
	default:
		hal_device_property_set_string (d, "scsi.type", "unknown");
	}

out:
	return d;
}

static gboolean
scsi_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_scsi_device_lun%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_int (d, "scsi.lun"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
mmc_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	gint host_num, rca, manfid, oemid;
	gchar *scr;

	if (parent == NULL) {
		d = NULL;
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "mmc");
	hal_device_property_set_string (d, "info.parent", parent->udi);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);
	sscanf (bus_id, "mmc%d:%x", &host_num, &rca);
	hal_device_property_set_int (d, "mmc.rca", rca);
	
	hal_util_set_string_from_file (d, "mmc.cid", sysfs_path, "cid");
	hal_util_set_string_from_file (d, "mmc.csd", sysfs_path, "csd");
	
	scr = hal_util_get_string_from_file (sysfs_path, "scr");
	if (scr != NULL) {
		if (strcmp (scr, "0000000000000000") == 0)
			scr = NULL;
		else
			hal_device_property_set_string (d, "mmc.scr", scr);
	}

	if (!hal_util_set_string_from_file (d, "info.product", sysfs_path, "name")) {
		if (scr != NULL)
			hal_device_property_set_string (d, "info.product", "SD Card");
		else
			hal_device_property_set_string (d, "info.product", "MMC Card");
	}
	
	if (hal_util_get_int_from_file (sysfs_path, "manfid", &manfid, 16)) {
		/* Here we should have a mapping to a name */
		char vendor[256];
		snprintf(vendor, 256, "Unknown (%d)", manfid);
		hal_device_property_set_string (d, "info.vendor", vendor);
	}
	if (hal_util_get_int_from_file (sysfs_path, "oemid", &oemid, 16)) {
		/* Here we should have a mapping to a name */
		char oem[256];
		snprintf(oem, 256, "Unknown (%d)", oemid);
		hal_device_property_set_string (d, "mmc.oem", oem);
	}

	hal_util_set_string_from_file (d, "mmc.date", sysfs_path, "date");
	hal_util_set_int_from_file (d, "mmc.hwrev", sysfs_path, "hwrev", 16);
	hal_util_set_int_from_file (d, "mmc.fwrev", sysfs_path, "fwrev", 16);
	hal_util_set_int_from_file (d, "mmc.serial", sysfs_path, "serial", 16);

out:
	return d;
}

static gboolean
mmc_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_mmc_card_rca%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_int (d, "mmc.rca"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
xen_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *devtype;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "xen");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "xen.bus_id",
					hal_util_get_last_element (sysfs_path));

	hal_util_set_string_from_file (d, "xen.path", sysfs_path, "nodename");

	devtype = hal_util_get_string_from_file (sysfs_path, "devtype");
	hal_device_property_set_string (d, "xen.type", devtype);

	if (strcmp (devtype, "pci") == 0) {
		hal_device_property_set_string (d, "info.product", "Xen PCI Device");
	} else if (strcmp (devtype, "vbd") == 0) {
		hal_device_property_set_string (d, "info.product", "Xen Virtual Block Device");
	} else if (strcmp (devtype, "vif") == 0) {
		hal_device_property_set_string (d, "info.product", "Xen Virtual Network Device");
	} else if (strcmp (devtype, "vtpm") == 0) {
		hal_device_property_set_string (d, "info.product", "Xen Virtual Trusted Platform Module");
	} else {
		char buf[64];
		g_snprintf (buf, sizeof (buf), "Xen Device (%s)", devtype);
		hal_device_property_set_string (d, "info.product", buf);
	}

	return d;
}

static gboolean
xen_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/xen_%s",
			      hal_device_property_get_string (d, "xen.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
ieee1394_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	long long unsigned int guid;
	gint host_id;
	const gchar *bus_id;
	gchar buf[64];

	d = NULL;

	if (parent == NULL)
		goto out;

	bus_id = hal_util_get_last_element (sysfs_path);

	if (sscanf (bus_id, "fw-host%d", &host_id) == 1)
		goto out;

	if (sscanf (bus_id, "%llx-%d", &guid, &host_id) !=2 )
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "ieee1394");
	hal_device_property_set_string (d, "info.parent", parent->udi);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_uint64 (d, "ieee1394.guid", guid);
	hal_util_set_int_from_file    (d, "ieee1394.vendor_id", sysfs_path, "../vendor_id", 16);
	hal_util_set_int_from_file    (d, "ieee1394.specifier_id", sysfs_path, "specifier_id", 16);
	hal_util_set_int_from_file    (d, "ieee1394.version", sysfs_path, "version", 16);

	if (!hal_util_set_string_from_file (d, "ieee1394.vendor", sysfs_path, "../vendor_oui")) {
		g_snprintf (buf, sizeof (buf), "Unknown (0x%06x)", 
			    hal_device_property_get_int (d, "ieee1394.vendor_id"));
		hal_device_property_set_string (d, "ieee1394.vendor", buf);
	}

	/* not all devices have product_id */
	if (hal_util_set_int_from_file    (d, "ieee1394.product_id", sysfs_path, "model_id", 16)) {
		if (!hal_util_set_string_from_file (d, "ieee1394.product", sysfs_path, "model_name_kv")) {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%06x)", 
				    hal_device_property_get_int (d, "ieee1394.product_id"));
			hal_device_property_set_string (d, "ieee1394.product", buf);
		}
	} else {
		hal_device_property_set_int (d, "ieee1394.product_id", 0x000000);
		hal_device_property_set_string (d, "ieee1394.product",
						hal_device_property_get_string (d, "ieee1394.vendor"));
	}
		
	hal_device_property_set_string (d, "info.vendor",
					hal_device_property_get_string (d, "ieee1394.vendor"));
	hal_device_property_set_string (d, "info.product",
					hal_device_property_get_string (d, "ieee1394.product"));

out:
	return d;
}

static gboolean
ieee1394_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/ieee1394_guid_%0llx",
			      hal_device_property_get_uint64 (d, "ieee1394.guid"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static inline void
ccw_add_dasd_properties (HalDevice *d, const gchar *sysfs_path)
{
	const gchar *disc;
	
	hal_util_set_int_from_file (d, "ccw.dasd.use_diag", sysfs_path,
				    "use_diag", 2);
	hal_util_set_int_from_file (d, "ccw.dasd.readonly", sysfs_path,
				    "readonly", 2);
	disc = hal_util_get_string_from_file (sysfs_path, "discipline");
	if (disc)
		hal_device_property_set_string(d, "ccw.dasd.discipline", disc);
}

static inline void
ccw_add_zfcp_properties (HalDevice *d, const gchar *sysfs_path)
{
	int online;

	/* zfcp adapter properties are only valid for online devices. */
	if (!hal_util_get_int_from_file (sysfs_path, "online", &online, 2))
		return;
	if (!online)
		return;

	hal_util_set_int_from_file (d, "ccw.zfcp.in_recovery", sysfs_path,
				    "in_recovery", 2);
	hal_util_set_int_from_file (d, "ccw.zfcp.failed", sysfs_path,
				    "failed", 2);
}

static inline void
ccw_add_tape_properties (HalDevice *d, const gchar *sysfs_path)
{
	int medium_state, online;

	const gchar *state_text[3] = {"unknown", "loaded", "no medium"};

	hal_util_set_string_from_file (d, "ccw.tape.state", sysfs_path, "state");
	hal_util_set_string_from_file (d, "ccw.tape.operation", sysfs_path,
				       "operation");
	/* The following properties are only valid for online devices. */
	if (!hal_util_get_int_from_file (sysfs_path, "online", &online, 2))
		return;
	if (!online)
		return;
	hal_util_set_int_from_file (d, "ccw.tape.blocksize", sysfs_path,
				    "blocksize", 10);
	if (!hal_util_get_int_from_file (sysfs_path, "medium_state",
					&medium_state, 10))
		return;
	hal_device_property_set_string (d, "ccw.tape.medium_state",
					state_text[medium_state]);
}

static inline void
ccw_add_3270_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_int_from_file (d, "ccw.3270.model", sysfs_path,
				    "model", 10);
	hal_util_set_int_from_file (d, "ccw.3270.rows", sysfs_path, "rows", 10);
	hal_util_set_int_from_file (d, "ccw.3270.columns", sysfs_path,
				    "columns", 10);
}

static HalDevice *
ccw_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	const gchar *pimpampom;
	int pim, pam, pom;
	const gchar *chpids;
	int chpid[8];
	gchar attr[25];
	int i;
	gchar driver_name[256];

	bus_id = hal_util_get_last_element (sysfs_path);

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device",
					sysfs_path);
	hal_device_property_set_string (d, "info.bus", "ccw");
	if (parent != NULL)
                hal_device_property_set_string (d, "info.parent", parent->udi);
        else
                hal_device_property_set_string
		  (d, "info.parent",
		   "/org/freedesktop/Hal/devices/computer");

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "ccw.bus_id", bus_id);
	hal_util_set_int_from_file (d, "ccw.online", sysfs_path, "online", 2);
	hal_util_set_string_from_file (d, "ccw.availablity", sysfs_path,
				       "availability");
	hal_util_set_int_from_file (d, "ccw.cmb_enable", sysfs_path,
				    "cmb_enable", 2);
	hal_util_set_string_from_file (d, "ccw.cutype", sysfs_path, "cutype");
	hal_util_set_string_from_file (d, "ccw.devtype", sysfs_path, "devtype");

	/* Get some values from the higher level subchannel structure.*/
	pimpampom = hal_util_get_string_from_file (sysfs_path, "../pimpampom");
	if (pimpampom) {
		sscanf (pimpampom, "%x %x %x", &pim, &pam, &pom);
		hal_device_property_set_int (d, "ccw.subchannel.pim", pim);
		hal_device_property_set_int (d, "ccw.subchannel.pam", pam);
		hal_device_property_set_int (d, "ccw.subchannel.pom", pom);
	}

	chpids = hal_util_get_string_from_file (sysfs_path, "../chpids");
	if (chpids) {
		sscanf (chpids, "%x %x %x %x %x %x %x %x", &chpid[0], &chpid[1],
			&chpid[2], &chpid[3], &chpid[4], &chpid[5], &chpid[6],
			&chpid[7]);
		for (i=0; i<8 && (chpid[i] != 0); i++) {
			g_snprintf (attr, sizeof (attr),
				    "ccw.subchannel.chpid%x", i);
			hal_device_property_set_int (d, attr, chpid[i]);
		}
	}

	/* Add some special properties. */
	if (hal_util_get_driver_name (sysfs_path, driver_name)) {
		if (!strncmp (driver_name, "dasd", 4))
			/* Same attributes for dasd_eckd and dasd_fba. */
			ccw_add_dasd_properties (d, sysfs_path);
		if (!strncmp (driver_name, "zfcp", 4))
			ccw_add_zfcp_properties (d, sysfs_path);
		if (!strncmp (driver_name, "tape_3", 6))
			/* For all channel attached tapes. */
			ccw_add_tape_properties (d, sysfs_path);
		if (!strncmp (driver_name, "3270", 4))
			ccw_add_3270_properties (d, sysfs_path);
	}
	return d;
}

static gboolean
ccw_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/ccw_%s",
			      hal_device_property_get_string
			      (d, "ccw.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static inline void
ccwgroup_add_qeth_properties (HalDevice *d, const gchar *sysfs_path)
{
	int is_layer2;

	/* Some attributes are not applicable for devices in layer2 mode. */
	hal_util_get_int_from_file (sysfs_path, "layer2", &is_layer2, 2);

	hal_util_set_string_from_file (d, "ccwgroup.qeth.large_send",
				       sysfs_path, "large_send");
	hal_util_set_string_from_file (d, "ccwgroup.qeth.card_type", sysfs_path,
				       "card_type");
	hal_util_set_string_from_file (d, "ccwgroup.qeth.checksumming",
				       sysfs_path, "checksumming");
	if (!is_layer2) {
		//CH: the next two are only valid for token ring devices
		hal_util_set_int_from_file (d,
					    "ccwgroup.qeth.canonical_macaddr",
					    sysfs_path, "canonical_macaddr", 2);
		hal_util_set_string_from_file (d,
					       "ccwgroup.qeth.broadcast_mode",
					       sysfs_path, "broadcast_mode");
		hal_util_set_int_from_file (d, "ccwgroup.qeth.fake_broadcast",
					    sysfs_path, "fake_broadcast", 2);
		hal_util_set_int_from_file (d, "ccwgroup.qeth.fake_ll",
					    sysfs_path, "fake_ll", 2);
	}
	hal_device_property_set_int (d, "ccwgroup.qeth.layer2", is_layer2);
	hal_util_set_string_from_file (d, "ccwgroup.qeth.portname", sysfs_path,
				       "portname");
	hal_util_set_int_from_file (d, "ccwgroup.qeth.portno", sysfs_path,
				    "portno", 10);
	hal_util_set_int_from_file (d, "ccwgroup.qeth.buffer_count", sysfs_path,
				    "buffer_count", 10);
	hal_util_set_int_from_file (d, "ccwgroup.qeth.add_hhlen", sysfs_path,
				    "add_hhlen", 10);
	hal_util_set_string_from_file (d, "ccwgroup.qeth.priority_queueing",
				       sysfs_path, "priority_queueing");
	if (!is_layer2) {
		hal_util_set_string_from_file (d, "ccwgroup.qeth.route4",
					       sysfs_path, "route4");
		hal_util_set_string_from_file (d, "ccwgroup.qeth.route6",
					       sysfs_path, "route6");
	}
	hal_util_set_string_from_file (d, "ccwgroup.qeth.state", sysfs_path,
				       "state");
}

static inline void
ccwgroup_add_ctc_properties (HalDevice *d, const gchar *sysfs_path)
{
	//CH: use protocol descriptions?
	hal_util_set_int_from_file (d, "ccwgroup.ctc.protocol", sysfs_path,
				    "protocol", 2);
	hal_util_set_string_from_file (d, "ccwgroup.ctc.type", sysfs_path,
				       "type");
	hal_util_set_int_from_file (d, "ccwgroup.ctc.buffer", sysfs_path,
				    "buffer", 10);
}

static inline void
ccwgroup_add_lcs_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_int_from_file (d, "ccwgroup.lcs.portnumber", sysfs_path,
				    "portno", 10);
	hal_util_set_string_from_file (d, "ccwgroup.lcs.type", sysfs_path,
				       "type");
	hal_util_set_int_from_file (d, "ccwgroup.lcs.lancmd_timeout",
				    sysfs_path, "lancmd_timeout", 10);
}

static inline void
ccwgroup_add_claw_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_string_from_file (d, "ccwgroup.claw.api_type", sysfs_path,
				       "api_type");
	hal_util_set_string_from_file (d, "ccwgroup.claw.adapter_name",
				       sysfs_path, "adapter_name");
	hal_util_set_string_from_file (d, "ccwgroup.claw.host_name", sysfs_path,
				       "host_name");
	hal_util_set_int_from_file (d, "ccwgroup.claw.read_buffer", sysfs_path,
				    "read_buffer", 10);
	hal_util_set_int_from_file (d, "ccwgroup.claw.write_buffer", sysfs_path,
				    "write_buffer", 10);
}

static HalDevice *
ccwgroup_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	gchar driver_name[256];

	bus_id = hal_util_get_last_element (sysfs_path);

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device",
					sysfs_path);
	hal_device_property_set_string (d, "info.bus", "ccwgroup");
	if (parent != NULL)
                hal_device_property_set_string (d, "info.parent", parent->udi);
        else
                hal_device_property_set_string
		  (d, "info.parent",
		   "/org/freedesktop/Hal/devices/computer");

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "ccwgroup.bus_id", bus_id);
	hal_util_set_int_from_file (d, "ccwgroup.online", sysfs_path,
				    "online", 2);

	/* Some devices have extra properties. */
	if (hal_util_get_driver_name (sysfs_path, driver_name)) {
		if (!strncmp (driver_name, "qeth", 4))
			ccwgroup_add_qeth_properties (d, sysfs_path);
		if (!strncmp (driver_name, "ctc", 3))
			ccwgroup_add_ctc_properties (d, sysfs_path);
		if (!strncmp (driver_name, "lcs", 3))
			ccwgroup_add_lcs_properties (d, sysfs_path);
		if (!strncmp (driver_name, "claw", 4))
			ccwgroup_add_claw_properties (d, sysfs_path);
	}
	return d;
}

static gboolean
ccwgroup_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/ccwgroup_%s",
			      hal_device_property_get_string
			      (d, "ccwgroup.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static inline void
iucv_add_netiucv_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_string_from_file (d, "iucv.netiucv.user", sysfs_path,
				       "user");
	hal_util_set_int_from_file (d, "iucv.netiucv.buffer", sysfs_path,
				    "buffer", 10);
}

static HalDevice *
iucv_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *bus_id;
	gchar driver_name[256];

	bus_id = hal_util_get_last_element (sysfs_path);

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device",
					sysfs_path);
	hal_device_property_set_string (d, "info.bus", "iucv");
	if (parent != NULL)
                hal_device_property_set_string (d, "info.parent", parent->udi);
        else
                hal_device_property_set_string
		  (d, "info.parent",
		   "/org/freedesktop/Hal/devices/computer");

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "iucv.bus_id", bus_id);

	if (hal_util_get_driver_name (sysfs_path, driver_name)) {
		if (!strncmp (driver_name, "netiucv", 7))
			iucv_add_netiucv_properties (d, sysfs_path);
	}
	return d;
}

static gboolean
iucv_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/iucv_%s",
			      hal_device_property_get_string
			      (d, "iucv.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pseudo_add (const gchar *sysfs_path, HalDevice *parent)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "pseudo");
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "pseudo.id", dev_id);

	g_snprintf (buf, sizeof (buf), "SCSI Debug Device (%s)", hal_device_property_get_string (d, "pseudo.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
pseudo_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pseudo",
			      hal_device_property_get_string (d, "platform.id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
physdev_remove (HalDevice *d)
{
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct
{
	const gchar *subsystem;
	HalDevice *(*add) (const gchar *sysfs_path, HalDevice *parent);
	gboolean (*compute_udi) (HalDevice *d);
	gboolean (*remove) (HalDevice *d);
} PhysDevHandler;

static PhysDevHandler physdev_handler_pci = { 
	.subsystem   = "pci",
	.add         = pci_add,
	.compute_udi = pci_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_usb = { 
	.subsystem   = "usb",
	.add         = usb_add,
	.compute_udi = usb_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_ide = { 
	.subsystem   = "ide",
	.add         = ide_add,
	.compute_udi = ide_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_pnp = { 
	.subsystem   = "pnp",
	.add         = pnp_add,
	.compute_udi = pnp_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_platform = {
	.subsystem   = "platform",
	.add         = platform_add,
	.compute_udi = platform_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_serio = { 
	.subsystem   = "serio",
	.add         = serio_add,
	.compute_udi = serio_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_pcmcia = { 
	.subsystem   = "pcmcia",
	.add         = pcmcia_add,
	.compute_udi = pcmcia_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_scsi = { 
	.subsystem   = "scsi",
	.add         = scsi_add,
	.compute_udi = scsi_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_mmc = { 
	.subsystem   = "mmc",
	.add         = mmc_add,
	.compute_udi = mmc_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_ieee1394 = { 
	.subsystem   = "ieee1394",
	.add         = ieee1394_add,
	.compute_udi = ieee1394_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_xen = {
	.subsystem   = "xen",
	.add         = xen_add,
	.compute_udi = xen_compute_udi,
	.remove      = physdev_remove
};

/* s390 specific busses */
static PhysDevHandler physdev_handler_ccw = {
	.subsystem   = "ccw",
	.add         = ccw_add,
	.compute_udi = ccw_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_ccwgroup = {
	.subsystem   = "ccwgroup",
	.add         = ccwgroup_add,
	.compute_udi = ccwgroup_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler physdev_handler_iucv = {
	.subsystem   = "iucv",
	.add         = iucv_add,
	.compute_udi = iucv_compute_udi,
	.remove      = physdev_remove
};

/* SCSI debug, to test thousends of fake devices */
static PhysDevHandler physdev_handler_pseudo = {
	.subsystem   = "pseudo",
	.add         = pseudo_add,
	.compute_udi = pseudo_compute_udi,
	.remove      = physdev_remove
};

static PhysDevHandler *phys_handlers[] = {
	&physdev_handler_pci,
	&physdev_handler_usb,
	&physdev_handler_ide,
	&physdev_handler_pnp,
	&physdev_handler_platform,
	&physdev_handler_serio,
	&physdev_handler_pcmcia,
	&physdev_handler_scsi,
	&physdev_handler_mmc,
	&physdev_handler_ieee1394,
	&physdev_handler_xen,
	&physdev_handler_ccw,
	&physdev_handler_ccwgroup,
	&physdev_handler_iucv,
	&physdev_handler_pseudo,
	NULL
};

/*--------------------------------------------------------------------------------------------------------------*/

static void 
physdev_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", d->udi));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
physdev_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Remove callouts completed udi=%s", d->udi));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}
	g_object_unref (d);

	hotplug_event_end (end_token);
}


static void 
physdev_callouts_preprobing_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;
	PhysDevHandler *handler = (PhysDevHandler *) userdata2;

	if (hal_device_property_get_bool (d, "info.ignore")) {
		/* Leave the device here with info.ignore==TRUE so we won't pick up children 
		 * Also remove category and all capabilities
		 */
		hal_device_property_remove (d, "info.category");
		hal_device_property_remove (d, "info.capabilities");
		hal_device_property_set_string (d, "info.udi", "/org/freedesktop/Hal/devices/ignored-device");
		hal_device_property_set_string (d, "info.product", "Ignored Device");
		
		HAL_INFO (("Preprobing merged info.ignore==TRUE"));
		
		/* Move from temporary to global device store */
		hal_device_store_remove (hald_get_tdl (), d);
		hal_device_store_add (hald_get_gdl (), d);
		
		hotplug_event_end (end_token);
		goto out;
	}
	
	
	/* Merge properties from .fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);
	
	/* Compute UDI */
	if (!handler->compute_udi (d)) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		hotplug_event_end (end_token);
		goto out;
	}
	
	/* Run callouts */
	hal_util_callout_device_add (d, physdev_callouts_add_done, end_token, NULL);

out:
	;
}

void
hotplug_event_begin_add_physdev (const gchar *subsystem, const gchar *sysfs_path, HalDevice *parent, void *end_token)
{
	guint i;

	HAL_INFO (("phys_add: subsys=%s sysfs_path=%s, parent=0x%08x", subsystem, sysfs_path, parent));

	if (parent != NULL && hal_device_property_get_bool (parent, "info.ignore")) {
		HAL_INFO (("Ignoring phys_add since parent has info.ignore==TRUE"));
		hotplug_event_end (end_token);
		goto out;
	}

	for (i = 0; phys_handlers [i] != NULL; i++) {
		PhysDevHandler *handler;

		handler = phys_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			HalDevice *d;

			d = handler->add (sysfs_path, parent);
			if (d == NULL) {
				/* didn't find anything - thus, ignore this hotplug event */
				hotplug_event_end (end_token);
				goto out;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_SYSFS_BUS);
			hal_device_property_set_string (d, "linux.subsystem", subsystem);

			/* Add to temporary device store */
			hal_device_store_add (hald_get_tdl (), d);

			/* Process preprobe fdi files */
			di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);

			/* Run preprobe callouts */
			hal_util_callout_device_preprobe (d, physdev_callouts_preprobing_done, end_token, handler);
			goto out;
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
						     "linux.sysfs_path", 
						     sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device with sysfs path %s - not found", sysfs_path));
		goto out;
	}
	
	for (i = 0; phys_handlers [i] != NULL; i++) {
		PhysDevHandler *handler;
		
		handler = phys_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			handler->remove (d);
			
			hal_util_callout_device_remove (d, physdev_callouts_remove_done, end_token, NULL);
			goto out2;
		}
	}

out:
	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out2:
	;
}

gboolean
physdev_rescan_device (HalDevice *d)
{
	return FALSE;
}

HotplugEvent *
physdev_generate_add_hotplug_event (HalDevice *d)
{
	const char *subsystem;
	const char *sysfs_path;
	HotplugEvent *hotplug_event;

	subsystem = hal_device_property_get_string (d, "linux.subsystem");
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}

HotplugEvent *
physdev_generate_remove_hotplug_event (HalDevice *d)
{
	const char *subsystem;
	const char *sysfs_path;
	HotplugEvent *hotplug_event;

	subsystem = hal_device_property_get_string (d, "linux.subsystem");
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}
