/***************************************************************************
 * CVSID: $Id$
 *
 * Generic methods for bus devices
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
#include <limits.h>
#include <glib.h>

#include "../logger.h"
#include "../callout.h"
#include "../device_store.h"
#include "../hald.h"
#include "common.h"
#include "bus_device.h"

/**
 * @defgroup HalDaemonLinuxBus Generic methods for bus devices
 * @ingroup HalDaemonLinux
 * @brief Generic methods for bus devices
 * @{
 */

/* fwd decl */
static void bus_device_got_parent (HalDeviceStore *store, HalDevice *parent,
				   gpointer user_data);

/** Generic accept function that accepts the device if and only if
 *  the bus name from sysfs equals the bus name in the class
 *
 *  @param  self                Pointer to class members
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
dbus_bool_t
bus_device_accept (BusDeviceHandler *self, const char *path, 
		   struct sysfs_device *device)
{
	/* only care about given bus name  */
	return strcmp (device->bus, self->sysfs_bus_name) == 0;
}

/** Visitor function for a bus device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  self                Pointer to class members
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void
bus_device_visit (BusDeviceHandler *self, const char *path, 
		  struct sysfs_device *device)
{
	BusAsyncData *bad;
	HalDevice *d;
	char *parent_sysfs_path;
	char buf[256];

	/* Construct a new device and add to temporary device list */
	d = hal_device_new ();
	hal_device_store_add (hald_get_tdl (), d);
	g_object_unref (d);

	hal_device_property_set_string (d, "info.bus", self->hal_bus_name);
	hal_device_property_set_string (d, "linux.sysfs_path", path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", path);

	/** Also set the sysfs path here, because otherwise we can't handle 
	 *  two identical devices per the algorithm used in a the function
	 *  rename_and_merge(). The point is that we need something unique 
	 *  in the bus namespace */
	snprintf (buf, sizeof(buf), "%s.linux.sysfs_path", self->hal_bus_name);
	hal_device_property_set_string (d, buf, path);

	parent_sysfs_path = get_parent_sysfs_path (path);

	bad = g_new0 (BusAsyncData, 1);
	bad->device = d;
	bad->handler = self;

	if (!got_parent(path)) {
		/* no need to find parent; we have none; just add directly 
		 * with parent set to NULL 
		 */
		bus_device_got_parent (hald_get_gdl(), NULL, bad);
	} else {
		/* Find parent; this happens asynchronously as our parent might
		 * be added later. If we are probing this can't happen so the
		 * timeout is set to zero in that event
		 */
		hal_device_store_match_key_value_string_async (
			hald_get_gdl (),
			"linux.sysfs_path_device",
			parent_sysfs_path,
			bus_device_got_parent, bad,
			HAL_LINUX_HOTPLUG_TIMEOUT);
	}

	free (parent_sysfs_path);
}


/** Removes the device from the TDL and adds it to the GDL when all
 *  all of the device's callouts have finished.  This is a gobject
 *  signal callback. 
 *
 *  @param  device              The device being moved
 *  @param  user_data           User data provided when connecting the signal
 *
 */
static void
bus_device_move_from_tdl_to_gdl (HalDevice *device, gpointer user_data)
{
	BusAsyncData *bad = (BusAsyncData*) user_data;

	g_object_ref (device);
	hal_device_store_remove (hald_get_tdl (), device);
	hal_device_store_add (hald_get_gdl (), device);
	g_signal_handlers_disconnect_by_func (device,
					      device_move_from_tdl_to_gdl,
					      user_data);
	g_object_unref (device);

	((bad->handler)->in_gdl) (bad->handler, device, device->udi);
}


/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  store               Device store we searched
 *  @param  parent              Async Return value from the find call
 *  @param  user_data           User data from find call
 */
static void
bus_device_got_parent (HalDeviceStore *store, HalDevice *parent,
		       gpointer user_data)
{
	const char *sysfs_path = NULL;
	char *new_udi = NULL;
	HalDevice *new_d = NULL;
	BusAsyncData *bad = (BusAsyncData*) user_data;
	HalDevice *d = (HalDevice *) bad->device;
	BusDeviceHandler *self = (BusDeviceHandler *) bad->handler;
	struct sysfs_device *device;

	/* set parent, if any */
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	}

	/* get more information about the device from the specialised 
	 * function */
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");
	assert (sysfs_path != NULL);
	device = sysfs_open_device (sysfs_path);
	if (device == NULL)
		DIE (("Coulnd't get sysfs device object for path %s", 
		      sysfs_path));
	self->pre_process (self, d, sysfs_path, device);
	sysfs_close_device (device);

	/* Compute a proper UDI (unique device id) and try to locate a 
	 * persistent unplugged device or simply add this new device...
	 */
	new_udi = rename_and_merge (d, self->compute_udi, self->hal_bus_name);
	if (new_udi != NULL) {
		HalDevice *device_to_add;

		new_d = hal_device_store_find (hald_get_gdl (), new_udi);

		device_to_add = new_d != NULL ? new_d : d;

		self->got_udi (self, device_to_add, new_udi);

		/* bad is freeded in callback */
		g_signal_connect (device_to_add,
				  "callouts_finished",
				  G_CALLBACK (bus_device_move_from_tdl_to_gdl),
				  bad);

		hal_callout_device (device_to_add, TRUE);
	} else {
		hal_device_store_remove (hald_get_tdl (), d);
		/* free bad now */
		g_free (bad);
	}
}

/** Init function for bus type
 *
 *  @param  self          Pointer to class members
 */
void
bus_device_init (BusDeviceHandler *self)
{
}

/** Shutdown function for bus type
 *
 *  @param  self          Pointer to class members
 */
void
bus_device_shutdown (BusDeviceHandler *self)
{
}


/** Called regulary (every two seconds) for polling / monitoring on devices
 *  of this bus type. 
 *
 *  @param  self          Pointer to class members
 */
void
bus_device_tick (BusDeviceHandler *self)
{
}

/** Called when the class device instance have been removed
 *
 *  @param  self          Pointer to class members
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  d             The HalDevice object of the instance of
 *                        this device
 */
void
bus_device_removed (BusDeviceHandler *self, const char *sysfs_path, 
		    HalDevice *d)
{
}

/** This method is called just before the device is added to the 
 *  GDL.
 *
 *  This is useful for adding more information about the device.
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  device        Libsysfs object representing device instance
 */
void 
bus_device_pre_process (BusDeviceHandler *self,
			HalDevice *d,
			const char *sysfs_path,
			struct sysfs_device *device)
{
}

void 
bus_device_got_udi (BusDeviceHandler *self,
		    HalDevice *d,
		    const char *udi)
{
}

void 
bus_device_in_gdl (BusDeviceHandler *self,
		   HalDevice *d,
		   const char *udi)
{
}


/** @} */
