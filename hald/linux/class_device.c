/***************************************************************************
 * CVSID: $Id$
 *
 * Generic methods for class devices
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

#include "../callout.h"
#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "common.h"
#include "class_device.h"

/**
 * @defgroup HalDaemonLinuxClass Generic methods for class devices
 * @ingroup HalDaemonLinux
 * @brief Generic methods for class devices
 * @{
 */

static void
class_device_got_device_file (HalDevice *d, gpointer user_data, 
			      gboolean prop_exists);

static void
class_device_final (ClassDeviceHandler* self, HalDevice *d,
		    gboolean merge_or_add);


/** Generic accept function that accepts the device if and only if
 *  the class name from sysfs equals the class name in the class
 *
 *  @param  self                Pointer to class members
 *  @param  path                Sysfs-path for device
 *  @param  class_device        libsysfs object for class device
 */
dbus_bool_t
class_device_accept (ClassDeviceHandler *self,
		     const char *path,
		     struct sysfs_class_device *class_device)
{

	/*HAL_INFO (("path = %s, classname = %s", 
	  path, self->sysfs_class_name));*/

	/* Don't care if there's no sysdevice */
	if (sysfs_get_classdev_device (class_device) == NULL)
		return FALSE;

	/* only care about given sysfs class name */
	if (strcmp (class_device->classname, self->sysfs_class_name) == 0) {
		return TRUE;
	}

	return FALSE;
}

/** Generic visitor method for class devices.
 *
 *  This function parses the attributes present and merges more information
 *  into the HAL device this class device points to
 *
 *  @param  self               Pointer to class members
 *  @param  path                Sysfs-path for class device
 *  @param  class_device        Libsysfs object for device
 */
HalDevice *
class_device_visit (ClassDeviceHandler *self,
		    const char *path,
		    struct sysfs_class_device *class_device)
{
	HalDevice *d;
	char dev_file[SYSFS_PATH_MAX];
	char dev_file_prop_name[SYSFS_PATH_MAX];
	gboolean merge_or_add;
	struct sysfs_device *sysdevice;

	sysdevice = sysfs_get_classdev_device (class_device);

	if (sysdevice == NULL) {
		merge_or_add = FALSE;
	} else {
		merge_or_add = self->merge_or_add;
	}

	/* Construct a new device and add to temporary device list */
	d = hal_device_new ();
	hal_device_store_add (hald_get_tdl (), d);
	g_object_unref (d);

	/* Need some properties if we are to appear in the tree on our own */
	if (!merge_or_add) {
	        /* 
		 * Kind of a hack... we only want to set the bus if
		 * our handler's merge_or_add is normally false.  Otherwise
		 * we want the bus to just be "unknown"
		 */
	        if (!self->merge_or_add) {
		        hal_device_property_set_string (d, "info.bus", 
							self->hal_class_name);
		} else {
		        hal_device_property_set_string (d, "info.bus",
							"unknown");
		}

		hal_device_property_set_string (d, "linux.sysfs_path", path);

		if (sysdevice != NULL) {
			hal_device_property_set_string (
				d, "linux.sysfs_path_device", 
				sysdevice->path);
		}
	} 

	/* We may require a device file */
	if (self->require_device_file) {

		/* Find the property name we should store the device file in */
		self->get_device_file_target (self, d, path, class_device,
					      dev_file_prop_name, 
					      SYSFS_PATH_MAX);

		/* Temporary property used for _udev_event() */
		hal_device_property_set_string (d, ".target_dev", 
						dev_file_prop_name);
	}

	hal_device_property_set_string (
		d, ".udev.class_name", self->sysfs_class_name);
	hal_device_property_set_string (
		d, ".udev.sysfs_path", path);

	if (self->require_device_file) {
		/* Ask udev about the device file if we are probing */
		if (hald_is_initialising) {

			if (!class_device_get_device_file (path, dev_file, 
							   SYSFS_PATH_MAX)) {
				HAL_WARNING (("Couldn't get device file for "
					      "sysfs path %s", path));
				return NULL;
			}

			/* If we are not probing this function will be called 
			 * upon receiving a dbus event */
			self->udev_event (self, d, dev_file);
		} 
	}

	/* Now find the physical device; this happens asynchronously as it
	 * might be added later. */
	if (merge_or_add) {
		ClassAsyncData *cad = g_new0 (ClassAsyncData, 1);
		cad->device = d;
		cad->handler = self;
		cad->merge_or_add = merge_or_add;

		/* find the sysdevice */
		hal_device_store_match_key_value_string_async (
			hald_get_gdl (),
			"linux.sysfs_path_device",
			sysdevice->path,
			class_device_got_sysdevice, cad,
			HAL_LINUX_HOTPLUG_TIMEOUT);
	} else {
		char *parent_sysfs_path;
		ClassAsyncData *cad = g_new0 (ClassAsyncData, 1);

		if (sysdevice != NULL) {
			parent_sysfs_path = 
				get_parent_sysfs_path (sysdevice->path);
		} else {
			parent_sysfs_path = "(none)";
		}

		cad->device = d;
		cad->handler = self;
		cad->merge_or_add = merge_or_add;

		/* find the parent */
		hal_device_store_match_key_value_string_async (
			hald_get_gdl (),
			"linux.sysfs_path_device",
			parent_sysfs_path,
			class_device_got_parent_device, cad,
			HAL_LINUX_HOTPLUG_TIMEOUT);
	}

	if (!merge_or_add)
		return d;
	else
		return NULL;
}

/** Called when the class device instance have been removed
 *
 *  @param  self               Pointer to class members
 *  @param  sysfs_path         The path in sysfs (including mount point) of
 *                             the class device in sysfs
 *  @param  d                  The HalDevice object of the instance of
 *                             this device class
 */
void
class_device_removed (ClassDeviceHandler* self, const char *sysfs_path, 
		      HalDevice *d)
{
	HAL_INFO (("sysfs_path = '%s'", sysfs_path));
}

/** Called when a device file (e.g. a file in /dev) have been created by udev 
 *
 *  @param  self               Pointer to class members
 *  @param  d                   The HalDevice object of the instance of
 *                              this device class
 *  @param  dev_file            device file, e.g. /dev/input/event4
 */
void
class_device_udev_event (ClassDeviceHandler *self, HalDevice *d, 
			 char *dev_file)
{
	const char *target_dev;
	char *target_dev_copy;

	/* merge the device file name into the name determined above */
	target_dev = hal_device_property_get_string (d, ".target_dev");
	assert (target_dev != NULL);

	/* hmm, have to make a copy because somewhere asynchronously we
	 * remove .target_dev */
	target_dev_copy = strdup (target_dev);
	assert (target_dev_copy != NULL);

	/* this will invoke _got_device_file per the _async_wait_for_  below */
	hal_device_property_set_string (d, target_dev_copy, dev_file);

	free (target_dev_copy);
}


/** Callback when the parent device is found or if timeout for search occurs.
 *  (only applicable if self->merge_or_add==FALSE)
 *
 *  @param  sysdevice           Async Return value from the find call or NULL
 *                              if timeout
 *  @param  data1               User data (or own device in this case)
 *  @param  data2               User data
 */
void
class_device_got_parent_device (HalDeviceStore *store, HalDevice *parent, 
				gpointer user_data)
{
	ClassAsyncData *cad = user_data;
	HalDevice *d = (HalDevice *) cad->device;
	ClassDeviceHandler *self = cad->handler;
	gboolean merge_or_add = cad->merge_or_add;

	if (parent == NULL) {
		HAL_WARNING (("No parent for class device at sysfs path %s",
			      d->udi));
		/* get rid of temporary device */
		hal_device_store_remove (hald_get_tdl (), d);
		return;
	}

	/* set parent */
	hal_device_property_set_string (d, "info.parent", parent->udi);

	/* wait for the appropriate property for the device file */
	if (self->require_device_file) {
		const char *target_dev;
		target_dev = hal_device_property_get_string (d, ".target_dev");
		assert (target_dev != NULL);

		hal_device_async_wait_property (
			d, target_dev, 
			class_device_got_device_file,
			(gpointer) cad,
			HAL_LINUX_HOTPLUG_TIMEOUT);
	} else {
		class_device_final (self, d, merge_or_add);

		g_free (cad);
	}
}


/** Callback when the sysdevice is found or if timeout for search occurs.
 *  (only applicable if self->merge_or_add==TRUE)
 *
 *  @param  sysdevice           Async Return value from the find call or NULL
 *                              if timeout
 *  @param  data1               User data (or own device in this case)
 *  @param  data2               User data
 */
void
class_device_got_sysdevice (HalDeviceStore *store, 
			    HalDevice *sysdevice, 
			    gpointer user_data)
{
	ClassAsyncData *cad = user_data;
	HalDevice *d = (HalDevice *) cad->device;
	ClassDeviceHandler *self = cad->handler;
	gboolean merge_or_add = cad->merge_or_add;

	/*HAL_INFO (("Entering d=0x%0x, sysdevice=0x%0x!", d, sysdevice));*/

	if (sysdevice == NULL) {
		HAL_WARNING (("Sysdevice for a class device never appeared!"));
		/* get rid of temporary device */
		hal_device_store_remove (hald_get_tdl (), d);
		return;
	}

	/* special case : merge onto the usb device, not the usb interface */
	if (hal_device_has_property (sysdevice, "info.bus") &&
	    hal_device_has_property (sysdevice, "info.parent") &&
	    (strcmp (hal_device_property_get_string (sysdevice, "info.bus"),
						     "usbif") == 0)) {
		const char *parent_udi;
		HalDevice *parent_device;

		parent_udi = hal_device_property_get_string (sysdevice, 
							     "info.parent");
		parent_device = hal_device_store_find (hald_get_gdl (),
						       parent_udi);
		if (parent_device != NULL) {
			sysdevice = parent_device;
		}
	}
	
	/* store the name of the sysdevice in a temporary property */
	hal_device_property_set_string (d, ".sysdevice", sysdevice->udi);

	/* wait for the appropriate property for the device file */
	if (self->require_device_file) {
		const char *target_dev;
		target_dev = hal_device_property_get_string (d, ".target_dev");
		assert (target_dev != NULL);

		hal_device_async_wait_property (
			d, target_dev, 
			class_device_got_device_file,
			(gpointer) cad,
			HAL_LINUX_HOTPLUG_TIMEOUT);
	} else {
		class_device_final (self, d, merge_or_add);
		
		g_free (cad);
	}
}

static void
class_device_got_device_file (HalDevice *d, gpointer user_data, 
			      gboolean prop_exists)
{
	ClassAsyncData *cad = (ClassAsyncData *) user_data;
	ClassDeviceHandler *self = cad->handler;
	gboolean merge_or_add = cad->merge_or_add;

	/*HAL_INFO (("entering"));*/

	g_free (cad);

	if (!prop_exists) {
		HAL_WARNING (("Never got device file for class device at %s", 
			      hal_device_property_get_string (d, ".udev.sysfs_path")));
		hal_device_store_remove (hald_get_tdl (), d);
		return;
	}

	class_device_final (self, d, merge_or_add);
}



/** Removes the device from the TDL and adds it to the GDL when all
 *  all of the device's callouts have finished.  This is a gobject
 *  signal callback. 
 *
 *  @param  device              The device being moved
 *  @param  user_data           User data provided when connecting the signal
 *
 */
void
class_device_move_from_tdl_to_gdl (HalDevice *device, gpointer user_data)
{
	ClassAsyncData *cad = (ClassAsyncData*) user_data;

	if (!hal_device_has_property (device, "info.parent")) {
		hal_device_property_set_string (
			device, "info.parent",
			"/org/freedesktop/Hal/devices/computer");
	}

	g_object_ref (device);
	hal_device_store_remove (hald_get_tdl (), device);
	hal_device_store_add (hald_get_gdl (), device);
	g_signal_handlers_disconnect_by_func (device,
					      class_device_move_from_tdl_to_gdl,
					      user_data);
	g_object_unref (device);

	((cad->handler)->in_gdl) (cad->handler, device, device->udi);
}


static void
class_device_final (ClassDeviceHandler* self, HalDevice *d,
		    gboolean merge_or_add)
{
	const char *sysfs_path = NULL;
	struct sysfs_class_device *class_device;

	/* get more information about the device from the specialised 
	 * function */
	sysfs_path = hal_device_property_get_string (d, ".udev.sysfs_path");
	assert (sysfs_path != NULL);
	class_device = sysfs_open_class_device_path (sysfs_path);
	if (class_device == NULL)
		DIE (("Coulnd't get sysfs class device object for path %s", 
		      sysfs_path));
	self->pre_process (self, d, sysfs_path, class_device);
	sysfs_close_class_device (class_device);

	if (merge_or_add) {
		const char *sysdevice_udi;
		HalDevice *sysdevice;

		/* get the sysdevice from the temporary cookie */
		sysdevice_udi = hal_device_property_get_string (d, ".sysdevice");
		assert (sysdevice_udi != NULL);
		sysdevice = hal_device_store_find (hald_get_gdl (),
						   sysdevice_udi);
		assert (sysdevice != NULL);

		/* remove various temporary properties */
		hal_device_property_remove (d, ".udev.sysfs_path");
		hal_device_property_remove (d, ".udev.class_name");
		hal_device_property_remove (d, ".sysdevice");
		hal_device_property_remove (d, ".target_dev");
	      
		/* merge information from temporary device onto the physical
		 * device */
		hal_device_merge (sysdevice, d);

		HAL_INFO (("Merged udi=%s onto %s", 
			   hal_device_get_udi (d),
			   hal_device_get_udi (sysdevice)));

		/* get rid of temporary device */
		hal_device_store_remove (hald_get_tdl (), d);

		self->post_merge (self, sysdevice);
	} else {
		char *new_udi;
		HalDevice *new_d;

		/* remove various temporary properties */
		hal_device_property_remove (d, ".udev.sysfs_path");
		hal_device_property_remove (d, ".udev.class_name");
		hal_device_property_remove (d, ".sysdevice");
		hal_device_property_remove (d, ".target_dev");

		/* Compute a proper UDI (unique device id) and try to locate a 
		 * persistent unplugged device or simply add this new device...
		 */
		new_udi = rename_and_merge (d, self->compute_udi, self->hal_class_name);
		if (new_udi != NULL) {
			HalDevice *device_to_add;
			ClassAsyncData *cad = g_new0 (ClassAsyncData, 1);

			new_d = hal_device_store_find (hald_get_gdl (),
						       new_udi);

			device_to_add = new_d != NULL ? new_d : d;

			self->got_udi (self, device_to_add, new_udi);

			cad->device = d;
			cad->handler = self;
			cad->merge_or_add = merge_or_add;


			g_signal_connect (device_to_add,
					  "callouts_finished",
					  G_CALLBACK (class_device_move_from_tdl_to_gdl),
					  cad);

			hal_callout_device (device_to_add, TRUE);
		} else {
			hal_device_store_remove (hald_get_tdl (), d);
		}
	}
}


/** Init function for block device handling. Does nothing.
 *
 */
void
class_device_init (ClassDeviceHandler *self)
{
}

/** Shutdown function for block device handling. Does nothing.
 *
 */
void
class_device_shutdown (ClassDeviceHandler *self)
{
}


/** This method is called just before the device is either merged
 *  onto the sysdevice or added to the GDL (cf. merge_or_add). 
 *  This is useful for extracting more information about the device
 *  through e.g. ioctl's using the device file property and also
 *  for setting info.category|capability.
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  class_device  Libsysfs object representing class device
 *                        instance
 */
void 
class_device_pre_process (ClassDeviceHandler *self,
			  HalDevice *d,
			  const char *sysfs_path,
			  struct sysfs_class_device *class_device)
{
	/* this function is left intentionally blank */
}

/** Called when an inferior HalDevice is merged.  This is the 
 *  last step when merging in devices.  This is only invoked if
 *  merge_or_add is TRUE.
 *
 *  @param  self          Pointer to the class members
 *  @param  d             The HalDevice object recently merged
 *
 */
void
class_device_post_merge (ClassDeviceHandler *self,
			 HalDevice *d)
{
	/* this function is left intentionally blank */
}

/** Called regulary (every two seconds) for polling / monitoring on devices
 *  of this class
 *
 *  @param  self          Pointer to class members
 */
void
class_device_tick (ClassDeviceHandler *self)
{
}

/** Get the name of that the property that the device file should
 *  be put in. This generic implementation just uses sysfs_class_name
 *  appended with '.device'.
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  class_device  Libsysfs object representing class device
 *                        instance
 *  @param  dev_file_prop Device file property name (out)
 *  @param  dev_file_prop_len  Maximum length of string
 */
void
class_device_get_device_file_target (ClassDeviceHandler *self,
				     HalDevice *d,
				     const char *sysfs_path,
				     struct sysfs_class_device *class_device,
				     char* dev_file_prop,
				     int dev_file_prop_len)
{
	snprintf (dev_file_prop, dev_file_prop_len, 
		  "%s.device", self->hal_class_name);
}

void
class_device_got_udi (ClassDeviceHandler *self,
		      HalDevice *d,
		      const char *udi)
{
}

void
class_device_in_gdl (ClassDeviceHandler *self,
		     HalDevice *d,
		     const char *udi)
{
}

/** @} */
