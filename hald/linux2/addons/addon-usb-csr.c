/***************************************************************************
 * CVSID: $Id$
 *
 * hal_addon_usb_csr.c : daemon handling CSR-based wireless mice
 *
 * Copyright (C) 2004 Sergey V. Udaltsov <svu@gnome.org>
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

#include <config.h>

#include <stdio.h>

#include <usb.h>

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../probing/shared.h"

#define CMD_IFACE_PROPERTY "battery.command_interface"
#define BUS_NO_PROPERTY "usb_device.bus_number"
/* linux!!! */
#define PORT_NO_PROPERTY "usb_device.linux.device_number"


#define CSR_IS_DUAL_PROPERTY "battery.csr.is_dual"
#define CURRENT_CHARGE_PROPERTY "battery.charge_level.current"

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

static PropertyCacheItem * dev_props = NULL;

static LibHalContext * hal_context = NULL;

static GMainLoop* main_loop;

static const char* the_device_udi;

#define TIMEOUT         0x1000

static void update_properties (void)
{
#if 0
          <merge key="info.category" type="string">battery</merge>
          <merge key="info.capabilities" type="string">battery</merge>
          <merge key="battery.charge_level.maximum.specified" type="int">7</merge>
          <merge key="battery.is_rechargeable" type="bool">false</merge>
#endif
	DBusError err;
	dbus_error_init (&err);

	libhal_device_set_property_bool (hal_context, 
					 the_device_udi, 
					 "battery.present", TRUE,
					 &err);
	if (!libhal_device_property_exists (hal_context,
					   the_device_udi,
					   "battery.is_rechargeable",
					    &err)) {
		libhal_device_set_property_bool (hal_context, 
						 the_device_udi, 
						 "battery.is_rechargeable", FALSE,
						 &err);
	}
	libhal_device_set_property_int (hal_context, 
					the_device_udi, 
					"battery.charge_level.design", 7,
					&err);
	libhal_device_set_property_int (hal_context, 
					the_device_udi, 
					"battery.charge_level.last_full", 7,
					&err);

	libhal_device_set_property_string (hal_context, 
					   the_device_udi, 
					   "info.category", "battery",
					   &err);

	libhal_device_set_property_string (hal_context, 
					   the_device_udi, 
					   "battery.command_interface", "csr",
					   &err);
}

static void add_capability (void)
{
	DBusError err;
	dbus_error_init (&err);
	libhal_device_add_capability (hal_context, 
				      the_device_udi, 
				      "battery",
				      &err);
}

static PropertyCacheItem* 
property_cache_item_get (const char * hal_device_udi)
{
	PropertyCacheItem * pci = g_new0 (PropertyCacheItem,1);
	DBusError err;
	dbus_error_init (&err);

	pci->bus_no_present = libhal_device_property_exists (hal_context,
							     hal_device_udi,
							     BUS_NO_PROPERTY,
							     &err);
	if (dbus_error_is_set (&err))
	{
		fprintf (stderr, "Error: [%s]/[%s]\n", err.name, err.message);	
	}

	if (pci->bus_no_present)
		pci->bus_no = libhal_device_get_property_int (hal_context,
							      hal_device_udi,
							      BUS_NO_PROPERTY,
							      &err);

	pci->port_no_present = libhal_device_property_exists (hal_context,
							      hal_device_udi,
							      PORT_NO_PROPERTY,
							      &err);
	if (pci->port_no_present)
		pci->port_no = libhal_device_get_property_int (hal_context,
							       hal_device_udi,
							       PORT_NO_PROPERTY,
							       &err);


	pci->csr_is_dual_present = libhal_device_property_exists (hal_context,
								  hal_device_udi,
								  CSR_IS_DUAL_PROPERTY,
								  &err);
	if (pci->csr_is_dual_present)
		pci->csr_is_dual = libhal_device_get_property_bool (hal_context,
								    hal_device_udi,
								    CSR_IS_DUAL_PROPERTY,
								    &err);


	pci->current_charge_present = libhal_device_property_exists (hal_context,
								     hal_device_udi,
								     CURRENT_CHARGE_PROPERTY,
							             &err);
	if (pci->current_charge_present)
		pci->current_charge = libhal_device_get_property_int (hal_context,
								      hal_device_udi,
								      CURRENT_CHARGE_PROPERTY,
							              &err);

	return pci;
}

static void
property_cache_item_free (PropertyCacheItem* pci)
{
	if (pci == NULL)
		return;
	g_free (pci);
}

static struct usb_device* 
find_device (const char * hal_device_udi, PropertyCacheItem * pci);

/* Thanks to lmctl code. I'd LOVE, REALLY LOVE to see some docs though... */
static void 
check_battery (const char * hal_device_udi, PropertyCacheItem * pci)
{
	struct usb_device *current_usb_device;
        usb_dev_handle * handle;
        unsigned char buf[80];
	DBusError err;
/* Internal CSR registered, I presume - for some reason not addressed directly */
#define P6  (buf[0])
#define P0  (buf[1])
#define P4  (buf[2])
#define P5  (buf[3])
#define P8  (buf[4])
#define P9  (buf[5])
#define PB0 (buf[6])
#define PB1 (buf[7])
        unsigned int  addr;
	int is_dual = 0;

	if (pci == NULL)
		return;

	dbg ("CSR device: [%s]\n", hal_device_udi);

	is_dual = pci->csr_is_dual;

	/* Which of subdevices to address */
	dbg ("Is dual: %d\n", is_dual);
        addr = is_dual? 1<<8 : 0;

	current_usb_device = find_device (hal_device_udi, pci);
	if (current_usb_device == NULL)
	{
		fprintf (stderr, "Device %s not found\n", hal_device_udi);
		return;
	}

        handle = usb_open (current_usb_device);
	if (handle == NULL)
	{
		perror ("Could not open usb device\n");
		return;
	}

	if (!usb_control_msg (handle, 0xc0, 0x09, 0x03|addr, 0x00|addr,
			      buf, 8, TIMEOUT) != 8)
	{
		if ((P0 == 0x3b) && (P4 == 0))
		{
			dbg ("Receiver busy, trying again later\n");
		} else
		{
			int current_charge = P5 & 0x07;

			dbg ("Charge level: %d->%d\n", 
				pci->current_charge, current_charge);
			if (current_charge != pci->current_charge)
			{
				pci->current_charge = current_charge;
				dbus_error_init (&err);
		    		libhal_device_set_property_int (hal_context, 
							        hal_device_udi, 
							        CURRENT_CHARGE_PROPERTY,
							        current_charge,
							        &err);
			}
		}
	} else
	{
		perror ("Writing to USB device");
        }

	usb_close (handle);
}

/* TODO: Is it linux-specific way to find the device? */
static struct usb_device* 
find_device (const char * hal_device_udi, PropertyCacheItem * pci)
{
	struct usb_bus* current_usb_bus;
	char LUdirname[5];
	char LUfname[5];

	if (!(pci->bus_no_present && pci->port_no_present))
	{
		/* no sysfs path */
		fprintf (stderr, "No hal bus number and/or port number\n");
		return NULL;
	}
	snprintf (LUdirname, sizeof (LUdirname), "%03d", pci->bus_no);
	snprintf (LUfname, sizeof (LUfname), "%03d",pci->port_no);
	dbg ("Looking for: [%s][%s]\n", LUdirname, LUfname);

	for (current_usb_bus = usb_busses; 
	     current_usb_bus != NULL; 
	     current_usb_bus = current_usb_bus->next)
	{
        	struct usb_device *current_usb_device;
		/* dbg ("Checking bus: [%s]\n", current_usb_bus->dirname); */
		if (g_strcasecmp (LUdirname, current_usb_bus->dirname))
			continue;

        	for (current_usb_device = current_usb_bus->devices; 
		     current_usb_device != NULL; 
		     current_usb_device = current_usb_device->next) 
		{
			/* dbg ("Checking port: [%s]\n", current_usb_device->filename); */
			if (g_strcasecmp (LUfname, current_usb_device->filename))
				continue;
			dbg ("Matched device: [%s][%s][%04X:%04X]\n", 
				current_usb_bus->dirname, 
				current_usb_device->filename,
				current_usb_device->descriptor.idVendor,
				current_usb_device->descriptor.idProduct);
			return current_usb_device;
		}
	}
	return NULL;
}

static gboolean
check_all_batteries (gpointer data)
{
	dbg ("** Check batteries\n");

	/* TODO: make it configurable (not to rescan every time) */
	usb_find_busses ();
	usb_find_devices ();

	check_battery (the_device_udi, dev_props);

	return TRUE;
}

static gboolean 
is_the_device (const char *hal_device_udi)
{
	return !g_ascii_strcasecmp (the_device_udi, hal_device_udi);
}

static void
device_removed (LibHalContext *ctx, const char *hal_device_udi)
{
	/* this device is removed */
	if (is_the_device (hal_device_udi))
	{
		dbg ("** The device %s removed, exit\n", the_device_udi);
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
	if (!g_ascii_strcasecmp (key, CMD_IFACE_PROPERTY))
	{
		if (is_removed)
		{
			dbg ("** Main Property %s removed: %s\n", key, hal_device_udi);
			/* probably we'll have to exit if this is our device */
			device_removed (ctx, hal_device_udi);
		}
	} else
	/* "Secondary" property modified */
	if (is_the_device (hal_device_udi))
	{
		if (!(g_ascii_strcasecmp (key, BUS_NO_PROPERTY) &&
		      g_ascii_strcasecmp (key, PORT_NO_PROPERTY) &&
	    	      g_ascii_strcasecmp (key, CSR_IS_DUAL_PROPERTY)))
		{
			dbg ("** Property %s added/changed: %s\n", 
				key, hal_device_udi);
			property_cache_item_free (dev_props);
			dev_props = property_cache_item_get (hal_device_udi);
		}
	}
}

static void
initial_fillup (void)
{
	dbg ("** Initial fillup\n");
	dev_props = property_cache_item_get (the_device_udi);
	dbg ("** Initial fillup done\n");
}

int
main (int argc, char *argv[])
{
	/* TODO: make it configurable*/
	long check_interval = 10L;
        DBusConnection *conn;
	DBusError err;
	
        if ((getenv ("HALD_VERBOSE")) != NULL)
                is_verbose = TRUE;

	the_device_udi = getenv ("UDI");

	dbg ("device:[%s]\n", the_device_udi);

	if (the_device_udi == NULL)
	{
		fprintf (stderr, "No device specified\n");
		return -2;
	}

	dbus_error_init (&err);

        if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &err)) == NULL)
	{
		fprintf (stderr, "Could not obtain dbus connection\n");
		return -3;
	}
	dbg ("DBUS connection: %p\n", conn);

	hal_context = libhal_ctx_new ();
        if (!libhal_ctx_set_dbus_connection (hal_context, conn))
	{
		fprintf (stderr, "Could not bind to DBUS\n");
		return -4;
	}
	dbg ("New context: %p\n", hal_context);

	libhal_ctx_init (hal_context, &err);
	libhal_ctx_set_device_removed (hal_context, device_removed);

	update_properties ();

	libhal_ctx_set_device_property_modified (hal_context, property_modified);

	initial_fillup ();

	usb_init ();
	
	dbg ("** Addon started\n");

	/* do coldplug */
	check_all_batteries (NULL);

	/* only add capability when initial charge_level key has been set */
	add_capability ();

	main_loop = g_main_loop_new (NULL, FALSE);

	g_timeout_add (1000L * check_interval, check_all_batteries, NULL);

	g_main_loop_run (main_loop);

	libhal_ctx_shutdown (hal_context, &err);
	dbg ("** Addon exits normally\n");
	return 0;
}
