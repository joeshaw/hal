/***************************************************************************
 * CVSID: $Id$
 *
 * hal_addon_usb_csr.c : daemon handling CSR-based wireless mice
 *
 * Copyright (C) 2004 Sergey V. Udaltsov <svu@gnome.org>
 * Copyright (C) 2005 Richard Hughes <richard@hughsie.com>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <usb.h>

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../../logger.h"
#include "../../util_helper.h"

#define TIMEOUT         30L

/* Internal CSR registered, I presume - for some reason not addressed directly */
#define P6  (buf[0])
#define P0  (buf[1])
#define P4  (buf[2])
#define P5  (buf[3])
#define P8  (buf[4])
#define P9  (buf[5])
#define PB0 (buf[6])
#define PB1 (buf[7])

typedef struct _PropertyCacheItem
{
	gboolean bus_no_present;
	int bus_no;
	gboolean port_no_present;
	int port_no;
	gboolean csr_is_dual_present;
	gboolean csr_is_dual;
	gboolean current_charge_present;
	int current_charge;
} PropertyCacheItem;

/* globals */
static PropertyCacheItem *dev_props = NULL;
static LibHalContext *halctx = NULL;
static GMainLoop *main_loop;
static const char *device_udi;

/* prototypes */
static struct usb_device *find_device (PropertyCacheItem *pci);

static PropertyCacheItem* 
property_cache_item_get (const char *hal_device_udi)
{
	PropertyCacheItem * pci = g_new0 (PropertyCacheItem,1);
	DBusError err;
	dbus_error_init (&err);

	pci->bus_no_present = libhal_device_property_exists (halctx, hal_device_udi, 
			"usb_device.bus_number", &err);

	if (dbus_error_is_set (&err)) {
		HAL_ERROR (("Error: [%s]/[%s]", err.name, err.message));	
		dbus_error_free (&err);
	}

	if (pci->bus_no_present)
		pci->bus_no = libhal_device_get_property_int (halctx, hal_device_udi, 
			"usb_device.bus_number", &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	pci->port_no_present = libhal_device_property_exists (halctx, hal_device_udi, 
			"usb_device.linux.device_number", &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	if (pci->port_no_present)
		pci->port_no = libhal_device_get_property_int (halctx, hal_device_udi, 
			"usb_device.linux.device_number", &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	pci->csr_is_dual_present = libhal_device_property_exists (halctx, hal_device_udi,
			"battery.csr.is_dual",  &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	if (pci->csr_is_dual_present)
		pci->csr_is_dual = libhal_device_get_property_bool (halctx, hal_device_udi,
			"battery.csr.is_dual",  &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	pci->current_charge_present = libhal_device_property_exists (halctx, hal_device_udi, 
			"battery.charge_level.current", &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	if (pci->current_charge_present)
		pci->current_charge = libhal_device_get_property_int (halctx, hal_device_udi, 
			"battery.charge_level.current", &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	return pci;
}

/* Thanks to lmctl code. I'd LOVE, REALLY LOVE to see some docs though... */
static void 
check_battery (const char *hal_device_udi, PropertyCacheItem *pci)
{
	struct usb_device *curr_device;
	usb_dev_handle *handle;
	char buf[80];
	DBusError err;
	unsigned int addr;
	int is_dual = 0;
	int percentage = 0;

	if (pci == NULL)
		return;

	HAL_DEBUG (("CSR device: [%s]", hal_device_udi));
	is_dual = pci->csr_is_dual;

	/* Which of subdevices to address */
	HAL_DEBUG (("Is dual: %d", is_dual));
	addr = is_dual? 1<<8 : 0;

	curr_device = find_device (pci);
	if (curr_device == NULL)	{
		HAL_ERROR (("Device %s not found", hal_device_udi));
		return;
	}

	handle = usb_open (curr_device);
	if (handle == NULL) {
		HAL_ERROR (("Could not open usb device"));
		return;
	}

	if (!usb_control_msg (handle, 0xc0, 0x09, 0x03|addr, 0x00|addr,
			 buf, 8, TIMEOUT) != 8)	{
		if ((P0 == 0x3b) && (P4 == 0)) {
			HAL_DEBUG (("Receiver busy, trying again later"));
		} else {
			int current_charge = P5 & 0x07;

			HAL_DEBUG (("Charge level: %d->%d", pci->current_charge, current_charge));
			if (current_charge != pci->current_charge) { 
				pci->current_charge = current_charge; 
				dbus_error_init (&err);

		 		libhal_device_set_property_int (halctx, hal_device_udi, 
		 			"battery.charge_level.current", current_charge, &err);
				LIBHAL_FREE_DBUS_ERROR (&err);

		 		if (current_charge != 0) {
		 			percentage = (100.0 / 7.0) * current_charge;
		 			libhal_device_set_property_int (halctx, hal_device_udi, 
		 				"battery.charge_level.percentage", percentage, &err);
				} else {
					libhal_device_remove_property(halctx, hal_device_udi,
								      "battery.charge_level.percentage", &err);	
				}

				LIBHAL_FREE_DBUS_ERROR (&err);
			}
		}
	} else {
		perror ("Writing to USB device");
	}

	usb_close (handle);
}

/* TODO: Is it linux-specific way to find the device? */
static struct usb_device* 
find_device (PropertyCacheItem *pci)
{
	struct usb_bus* curr_bus;
	char LUdirname[5];
	char LUfname[5];

	if (!(pci->bus_no_present && pci->port_no_present)) {
		/* no sysfs path */
		HAL_ERROR (("No hal bus number and/or port number"));
		return NULL;
	}
	snprintf (LUdirname, sizeof (LUdirname), "%03d", pci->bus_no);
	snprintf (LUfname, sizeof (LUfname), "%03d",pci->port_no);
	HAL_DEBUG (("Looking for: [%s][%s]", LUdirname, LUfname));

	for (curr_bus = usb_busses; curr_bus != NULL; curr_bus = curr_bus->next) {
 		struct usb_device *curr_device;
		/* dbg ("Checking bus: [%s]", curr_bus->dirname); */
		if (g_ascii_strcasecmp (LUdirname, curr_bus->dirname))
			continue;

 		for (curr_device = curr_bus->devices; curr_device != NULL; 
		     curr_device = curr_device->next) {
			/* dbg ("Checking port: [%s]", curr_device->filename); */
			if (g_ascii_strcasecmp (LUfname, curr_device->filename))
				continue;
			HAL_DEBUG (("Matched device: [%s][%s][%04X:%04X]", curr_bus->dirname, 
				curr_device->filename, 
				curr_device->descriptor.idVendor, 
				curr_device->descriptor.idProduct));
			return curr_device;
		}
	}
	return NULL;
}

static gboolean
check_all_batteries (gpointer data)
{
	HAL_DEBUG (("** Check batteries"));
	/* TODO: make it configurable (not to rescan every time) */
	usb_find_busses ();
	usb_find_devices ();
	check_battery (device_udi, dev_props);
	return TRUE;
}

static gboolean 
is_the_device (const char *hal_device_udi)
{
	return !g_ascii_strcasecmp (device_udi, hal_device_udi);
}

static void
device_removed (const char *hal_device_udi)
{
	/* this device is removed */
	if (is_the_device (hal_device_udi)) {
		HAL_DEBUG (("** The device %s removed, exit", device_udi));
		g_main_loop_quit (main_loop);
	}
}

static void 
property_modified (LibHalContext *ctx,
		 const char *hal_device_udi,
		 const char *key,
		 dbus_bool_t is_removed,
		 dbus_bool_t is_added)
{
	/* "Key" property modified */
	if (!g_ascii_strcasecmp (key, "battery.command_interface")) {
		if (is_removed) {
			HAL_DEBUG (("** Main Property %s removed: %s", key, hal_device_udi));
			/* probably we'll have to exit if this is our device */
			device_removed (hal_device_udi);
		}
	} else
		/* "Secondary" property modified */
		if (is_the_device (hal_device_udi))
		{
			if (!(g_ascii_strcasecmp (key, "usb_device.bus_number") &&
			      g_ascii_strcasecmp (key, "usb_device.linux.device_number") &&
	 		      g_ascii_strcasecmp (key, "battery.csr.is_dual"))) {
				HAL_DEBUG (("** Property %s added/changed: %s", key, hal_device_udi));
				if (dev_props)
					g_free (dev_props);
				dev_props = property_cache_item_get (hal_device_udi);
			}
		}
}

int
main (int argc, char *argv[])
{
	DBusError err;
	int retval = 0;

	hal_set_proc_title_init (argc, argv);

	setup_logger ();	

	device_udi = getenv ("UDI");

	HAL_DEBUG (("device:[%s]", device_udi));
	if (device_udi == NULL) {
		HAL_ERROR (("No device specified"));
		return -2;
	}

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		HAL_ERROR (("Cannot connect to hald"));
		retval = -3;
		goto out;
	}


	/* update_properties */
	libhal_device_set_property_bool (halctx, device_udi, 
			"battery.present", TRUE, &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	if (!libhal_device_property_exists (halctx, device_udi, 
			"battery.is_rechargeable", &err)) {
		LIBHAL_FREE_DBUS_ERROR (&err);
		libhal_device_set_property_bool (halctx, device_udi, 
			"battery.is_rechargeable", FALSE, &err);
	}

	LIBHAL_FREE_DBUS_ERROR (&err);
	libhal_device_set_property_int (halctx, device_udi, 
			"battery.charge_level.design", 7, &err);
	LIBHAL_FREE_DBUS_ERROR (&err);
	libhal_device_set_property_int (halctx, device_udi, 
			"battery.charge_level.last_full", 7, &err);
	LIBHAL_FREE_DBUS_ERROR (&err);
	libhal_device_set_property_string (halctx, device_udi, 
			"info.category", "battery", &err);
	LIBHAL_FREE_DBUS_ERROR (&err);
	libhal_device_set_property_string (halctx, device_udi, 
			"battery.command_interface", "csr", &err);

	/* monitor change */
	libhal_ctx_set_device_property_modified (halctx, property_modified);

	/* Initial fillup */
	dev_props = property_cache_item_get (device_udi);
	HAL_ERROR (("** Initial fillup done"));

	/* init usb */
	usb_init ();
	
	/* do coldplug */
	check_all_batteries (NULL);

	/* only add capability when initial charge_level key has been set */
	LIBHAL_FREE_DBUS_ERROR (&err);
	libhal_device_add_capability (halctx, device_udi, "battery", &err);

	LIBHAL_FREE_DBUS_ERROR (&err);
	if (!libhal_device_addon_is_ready (halctx, device_udi, &err)) {
		retval = -4;
		goto out;
	}

	hal_set_proc_title ("hald-addon-usb-csr: listening on '%s'", 
			    libhal_device_get_property_string(halctx, device_udi,
							      "info.product", &err));

	main_loop = g_main_loop_new (NULL, FALSE);
#ifdef HAVE_GLIB_2_14
	g_timeout_add_seconds (TIMEOUT, check_all_batteries, NULL);
#else
	g_timeout_add (1000L * TIMEOUT, check_all_batteries, NULL);
#endif
	g_main_loop_run (main_loop);
	return 0;

out:
        HAL_DEBUG (("An error occured, exiting cleanly"));

        LIBHAL_FREE_DBUS_ERROR (&err);

        if (halctx != NULL) {
                libhal_ctx_shutdown (halctx, &err);
                LIBHAL_FREE_DBUS_ERROR (&err);
                libhal_ctx_free (halctx);
        }

        return retval;
}
