/***************************************************************************
 * CVSID: $Id$
 *
 * Detection and monitoring of devices on Linux 2.6 + udev
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

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"

#include "common.h"
#include "bus_device.h"
#include "class_device.h"

#include "libsysfs/libsysfs.h"

extern ClassDeviceHandler input_class_handler;
extern ClassDeviceHandler scsi_host_class_handler;
extern ClassDeviceHandler scsi_device_class_handler;
extern ClassDeviceHandler block_class_handler;

extern BusDeviceHandler pci_bus_handler;
extern BusDeviceHandler usb_bus_handler;
extern BusDeviceHandler usbif_bus_handler;
extern BusDeviceHandler ide_host_bus_handler;
extern BusDeviceHandler ide_bus_handler;

static ClassDeviceHandler* class_device_handlers[] = {
	&input_class_handler,
	&scsi_host_class_handler,
	&scsi_device_class_handler,
	&block_class_handler,
	NULL
};

static BusDeviceHandler* bus_device_handlers[] = {
	&pci_bus_handler,
	&usb_bus_handler,
	&usbif_bus_handler,
	&ide_host_bus_handler,
        &ide_bus_handler,
	NULL
};

/**
 * @defgroup HalDaemonLinux Linux 2.6 support
 * @ingroup HalDaemon
 * @brief Device detection and monitoring code using Linux 2.6 + udev
 * @{
 */

/** Mount path for sysfs */
char sysfs_mount_path[SYSFS_PATH_MAX];

/** Visitor function for any class device.
 *
 *  This function determines the class of the device and call the
 *  appropriate visit_class_device_<classtype> function if matched.
 *
 *  @param  path                Sysfs-path for class device, e.g.
 *                              /sys/class/scsi_host/host7
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void
visit_class_device (const char *path, dbus_bool_t visit_children)
{
	int i;
	struct sysfs_class_device *class_device;
	struct sysfs_directory *subdir;

	class_device = sysfs_open_class_device (path);
	if (class_device == NULL)
		DIE (("Coulnd't get sysfs class device object for path %s",
		      path));

	HAL_INFO (("*** visit_class_device classname=%s name=%s path=%s\n",
		   class_device->classname,
		   class_device->name,
		   class_device->path));

	for (i=0; class_device_handlers[i] != NULL; i++) {
		ClassDeviceHandler *ch = class_device_handlers[i];
		ch->visit (ch, path, class_device, is_probing);
	}

	/* Visit children */
	if (visit_children && class_device->directory != NULL &&
	    class_device->directory->subdirs != NULL) {
		dlist_for_each_data (class_device->directory->subdirs,
				     subdir, struct sysfs_directory) {
			char newpath[SYSFS_PATH_MAX];
			snprintf (newpath, SYSFS_PATH_MAX, "%s/%s", path,
				  subdir->name);
			visit_class_device (newpath, TRUE);
		}
	}

	sysfs_close_class_device (class_device);
}

/** Visit all devices of a given class
 *
 *  @param  class_name          Name of class, e.g. scsi_host or block
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void
visit_class (const char *class_name, dbus_bool_t visit_children)
{
	struct sysfs_class *cls = NULL;
	struct sysfs_class_device *cur = NULL;

	cls = sysfs_open_class (class_name);
	if (cls == NULL) {
		HAL_ERROR (("Error opening class %s\n", class_name));
		return;
	}

	if (cls->devices != NULL) {
		dlist_for_each_data (cls->devices, cur,
				     struct sysfs_class_device) {
			visit_class_device (cur->path, visit_children);
		}
	}

	sysfs_close_class (cls);
}

/** Visitor function for any device.
 *
 *  This function determines the bus-type of the device and call the
 *  appropriate visit_device_<bustype> function if matched.
 *
 *  @param  path                Sysfs-path for device
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void
visit_device (const char *path, dbus_bool_t visit_children)
{
	int i;
	struct sysfs_device *device;
	struct sysfs_directory *subdir;

	device = sysfs_open_device (path);
	if (device == NULL)
		DIE (("Coulnd't get sysfs device object for path %s", path));

	/*HAL_INFO ((" path=%s", path));*/

	for (i=0; bus_device_handlers[i] != NULL; i++) {
		BusDeviceHandler *bh = bus_device_handlers[i];
		if (bh->accept (bh, path, device, is_probing))
			bh->visit (bh, path, device, is_probing);
	}

	/* Visit children */
	if (visit_children && device->directory->subdirs != NULL) {
		dlist_for_each_data (device->directory->subdirs, subdir,
				     struct sysfs_directory) {
			char newpath[SYSFS_PATH_MAX];
			snprintf (newpath, SYSFS_PATH_MAX, "%s/%s", path,
				  subdir->name);
			visit_device (newpath, TRUE);
		}
	}

	sysfs_close_device (device);
}


/** Timeout handler for polling
 *
 *  @param  data                User data when setting up timer
 *  @return                     TRUE iff timer should be kept
 */
static gboolean
osspec_timer_handler (gpointer data)
{
	int i;

	for (i=0; bus_device_handlers[i] != NULL; i++) {
		BusDeviceHandler *bh = bus_device_handlers[i];
		bh->tick (bh);
	}

	for (i=0; class_device_handlers[i] != NULL; i++) {
		ClassDeviceHandler *ch = class_device_handlers[i];
		ch->tick (ch);
	}

	return TRUE;
}

/* This function is documented in ../osspec.h */
void
osspec_init (DBusConnection * dbus_connection)
{
	int i;
	int rc;
	DBusError error;

	/* get mount path for sysfs */
	rc = sysfs_get_mnt_path (sysfs_mount_path, SYSFS_PATH_MAX);
	if (rc != 0) {
		DIE (("Couldn't get mount path for sysfs"));
	}
	HAL_INFO (("Mountpoint for sysfs is %s", sysfs_mount_path));

	for (i=0; bus_device_handlers[i] != NULL; i++) {
		BusDeviceHandler *bh = bus_device_handlers[i];
		bh->init (bh);
	}

	for (i=0; class_device_handlers[i] != NULL; i++) {
		ClassDeviceHandler *ch = class_device_handlers[i];
		ch->init (ch);
	}

	/* Add match for signals from udev */
	dbus_error_init (&error);
	dbus_bus_add_match (dbus_connection,
			    "type='signal',"
			    "interface='org.kernel.udev.NodeMonitor',"
			    /*"sender='org.kernel.udev'," until dbus is fixed*/
			    "path='/org/kernel/udev/NodeMonitor'", &error);
	if (dbus_error_is_set (&error)) {
		HAL_WARNING (("Cannot subscribe to udev signals, error=%s",
			      error.message));
	}

	/* Setup timer */
	g_timeout_add (2000, osspec_timer_handler, NULL);	
}

/** This is set to #TRUE if we are probing and #FALSE otherwise */
dbus_bool_t is_probing;

/* This function is documented in ../osspec.h */
void
osspec_probe ()
{
	int i;
	char path[SYSFS_PATH_MAX];
	struct sysfs_directory *current;
	struct sysfs_directory *dir;

	is_probing = TRUE;

	/* traverse /sys/devices */
	strncpy (path, sysfs_mount_path, SYSFS_PATH_MAX);
	strncat (path, SYSFS_DEVICES_DIR, SYSFS_PATH_MAX);

	dir = sysfs_open_directory (path);
	if (dir == NULL) {
		DIE (("Error opening sysfs directory at %s\n", path));
	}
	if (sysfs_read_directory (dir) != 0) {
		DIE (("Error reading sysfs directory at %s\n", path));
	}
	if (dir->subdirs != NULL) {
		dlist_for_each_data (dir->subdirs, current,
				     struct sysfs_directory) {
			visit_device (current->path, TRUE);
		}
	}
	sysfs_close_directory (dir);

	for (i=0; class_device_handlers[i] != NULL; i++) {
		ClassDeviceHandler *ch = class_device_handlers[i];
		visit_class (ch->sysfs_class_name, TRUE);
		/** @todo FIXME how to select TRUE/FALSE above (see below) */
	}

	is_probing = FALSE;

	/* Notify various device and class types that detection is done, so 
	 * they can do some (optional) batch processing
	 */
	for (i=0; bus_device_handlers[i] != NULL; i++) {
		BusDeviceHandler *bh = bus_device_handlers[i];
		bh->detection_done (bh);
	}

	for (i=0; class_device_handlers[i] != NULL; i++) {
		ClassDeviceHandler *ch = class_device_handlers[i];
		ch->detection_done (ch);
	}
}

/** Handle a org.freedesktop.Hal.HotplugEvent message. This message
 *  origins from the hal.hotplug program, tools/linux/hal_hotplug.c,
 *  and is basically just a D-BUS-ification of the hotplug event.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult
handle_hotplug (DBusConnection * connection, DBusMessage * message)
{
	int i;
	DBusMessageIter iter;
	DBusMessageIter dict_iter;
	dbus_bool_t is_add;
	char *subsystem;
	char sysfs_devpath[SYSFS_PATH_MAX];

	sysfs_devpath[0] = '\0';

	dbus_message_iter_init (message, &iter);

	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRING) {
		/** @todo Report error */
		dbus_message_unref (message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	subsystem = dbus_message_iter_get_string (&iter);

	dbus_message_iter_next (&iter);
	dbus_message_iter_init_dict_iterator (&iter, &dict_iter);

	is_add = FALSE;

	do {
		char *key;
		char *value;

		key = dbus_message_iter_get_dict_key (&dict_iter);
		value = dbus_message_iter_get_string (&dict_iter);

		HAL_INFO (("key/value : %s=%s", key, value));

		if (strcmp (key, "ACTION") == 0) {
			if (strcmp (value, "add") == 0) {
				is_add = TRUE;
			}
		} else if (strcmp (key, "DEVPATH") == 0) {
			strncpy (sysfs_devpath, sysfs_mount_path,
				 SYSFS_PATH_MAX);
			strncat (sysfs_devpath, value, SYSFS_PATH_MAX);
		}
	} while (dbus_message_iter_has_next (&dict_iter) &&
		 dbus_message_iter_next (&dict_iter));

	/* ignore events without DEVPATH */
	if (sysfs_devpath[0] == '\0')
		goto out;

	HAL_INFO (("HotplugEvent %s, subsystem=%s devpath=%s",
		   (is_add ? "add" : "remove"), subsystem,
		   sysfs_devpath[0] != '\0' ? sysfs_devpath : "(none)"));

	/* class device */
	if (is_add) {
		/* add */
		for (i=0; class_device_handlers[i] != NULL; i++) {
			ClassDeviceHandler *ch = class_device_handlers[i];
			if (strcmp (subsystem, ch->sysfs_class_name) == 0) {
				/* @todo call ch stuff directly */
				visit_class_device (sysfs_devpath, FALSE);
				goto out;
			}
		}
	} else {
		/* remove */
		for (i=0; class_device_handlers[i] != NULL; i++) {
			ClassDeviceHandler *ch = class_device_handlers[i];
			if (strcmp (subsystem, ch->sysfs_class_name) == 0) {
				if (!ch->merge_or_add) {
					/* was actually added, so it makes sense to remove */

					HalDevice *d;
					d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_devpath);
					if (d == NULL) {
						HAL_WARNING (("Couldn't remove classdevice @ %s on hotplug remove", sysfs_devpath));
					} else {
						HAL_INFO (("Removing classdevice @ sysfspath %s, udi %s", sysfs_devpath, d->udi));
						ch->removed (ch, sysfs_devpath, d);

						hal_device_store_remove (hald_get_gdl (), d);
						goto out;
					}

				}
				HAL_INFO (("sysfs_devpath=%s by %s", sysfs_devpath, subsystem));
			}
		}
	}

	/* bus device */
	if (is_add) {
		/* add */
		for (i=0; bus_device_handlers[i] != NULL; i++) {
			BusDeviceHandler *bh = bus_device_handlers[i];
			if (strcmp (subsystem, bh->sysfs_bus_name) == 0) {
				/* @todo call bh stuff directly */
				visit_device (sysfs_devpath, FALSE);
				goto out;
			}
		}
	} else {
		/* remove */

		/* @todo refine me and add support for persistent devices */

		HalDevice *d;

		d = hal_device_store_match_key_value_string (
			hald_get_gdl (), "linux.sysfs_path",
			sysfs_devpath);

		if (d == NULL) {
			HAL_WARNING (("Couldn't remove device @ %s "
				      "on hotplug remove", 
				      sysfs_devpath));
		} else {
			HAL_INFO (("Removing device @ sysfspath %s, "
				   "udi %s", sysfs_devpath, d->udi));

			hal_device_store_remove (hald_get_gdl (), d);
			goto out;
		}
	}

#if 0
	if (sysfs_devpath[0] != '\0' &&
	    (strcmp (subsystem, "usb") == 0 ||
	     strcmp (subsystem, "pci") == 0 ||
	     strcmp (subsystem, "ieee1394") == 0 ||
	     strcmp (subsystem, "i2c") == 0)) {

		if (is_add) {
			HAL_INFO (("Adding device @ sysfspath %s",
				   sysfs_devpath));
			visit_device (sysfs_devpath, FALSE);
		} else {
			HalDevice *d;

			d = hal_device_store_match_key_value_string (
				hald_get_gdl (), "linux.sysfs_path",
				sysfs_devpath);

			if (d == NULL) {
				HAL_WARNING (("Couldn't remove device @ %s "
					      "on hotplug remove", 
					      sysfs_devpath));
			} else {
				HAL_INFO (("Removing device @ sysfspath %s, "
					   "udi %s", sysfs_devpath, d->udi));

				if (ds_property_exists
				    (d, "info.persistent")
				    && hal_device_property_get_bool (
					    d, "info.persistent"))
				{
					ds_property_set_bool (
						d, "info.not_available", TRUE);
					/* Remove enough specific details so 
					 * we are not found by child devices 
					 * when being plugged in again.. 
					 */
					ds_property_remove (d, "info.parent");
					ds_property_remove (
						d, "info.physical_device");
					ds_property_remove (
						d, "linux.sysfs_path");
					ds_property_remove (
						d, "linux.sysfs_path_device");
					HAL_INFO (("Device %s is persistent, "
						   "so not removed", d->udi));
				} else {
					hal_device_store_remove (
						hald_get_gdl (), d);
				}
			}
		}
	}
#endif

out:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* fwd decl */
static void handle_udev_node_created_found_device (HalDevice * d,
						   void *data1,
						   void *data2);

static void
udev_node_created_cb (HalDeviceStore *store, HalDevice *device,
		      gpointer user_data)
{
	const char *filename = user_data;

	handle_udev_node_created_found_device (device, (void*) filename, NULL);
}

/** Handle a org.freedesktop.Hal.HotplugEvent message. This message
 *  origins from the hal.hotplug program, tools/linux/hal_hotplug.c,
 *  and is basically just a D-BUS-ification of the hotplug event.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult
handle_udev_node_created (DBusConnection * connection,
			  DBusMessage * message)
{
	char *filename;
	char *sysfs_path;
	char sysfs_dev_path[SYSFS_PATH_MAX];

	if (dbus_message_get_args (message, NULL,
				   DBUS_TYPE_STRING, &filename,
				   DBUS_TYPE_STRING, &sysfs_path,
				   DBUS_TYPE_INVALID)) {
		strncpy (sysfs_dev_path, sysfs_mount_path, SYSFS_PATH_MAX);
		strncat (sysfs_dev_path, sysfs_path, SYSFS_PATH_MAX);
		HAL_INFO (("udev NodeCreated: %s %s\n", filename,
			   sysfs_dev_path));

		/* Find class device; this happens asynchronously as our it 
		 * might be added later..
		 */
#if 0
		ds_device_async_find_by_key_value_string (
			".udev.sysfs_path", sysfs_dev_path, FALSE,
			/* note: it doesn't need to be in the GDL */
			handle_udev_node_created_found_device,
			(void *) filename, NULL,
			HAL_LINUX_HOTPLUG_TIMEOUT);
#elif 0
		handle_udev_node_created_found_device (
			hal_device_store_match_key_value_string (
				hald_get_gdl (), ".udev.sysfs_path",
				sysfs_dev_path),
			filename, NULL);
#else
		hal_device_store_match_key_value_string_async (
			hald_get_tdl (),
			".udev.sysfs_path",
			sysfs_dev_path,
			udev_node_created_cb, filename,
			HAL_LINUX_HOTPLUG_TIMEOUT);
#endif

		/* NOTE NOTE NOTE: we will free filename in async 
		 * result function 
		 */

		dbus_free (sysfs_path);
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Callback when the block device is found or if there is none..
 *
 *  @param  d                   Async Return value from the find call
 *  @param  data1               User data, in this case the filename
 *  @param  data2               User data
 */
static void
handle_udev_node_created_found_device (HalDevice * d,
				       void *data1, void *data2)
{
	int i;
	const char *sysfs_class_name;
	char *dev_file = (char *) data1;

	if (d != NULL) {
		HAL_INFO (("Got dev_file=%s for udi=%s", dev_file, d->udi));

		sysfs_class_name = 
			hal_device_property_get_string (d, ".udev.class_name");

		HAL_INFO ((".udev.class_name = %s", sysfs_class_name));

		for (i=0; class_device_handlers[i] != NULL; i++) {
			ClassDeviceHandler *ch = class_device_handlers[i];
			if (strcmp (ch->sysfs_class_name, sysfs_class_name) == 0) {
				ch->udev_event (ch, d, dev_file);
			}
		}
	} else {
		HAL_WARNING (("No HAL device corresponding to device %s",
			      dev_file));
	}

	dbus_free (dev_file);
}

/** Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @param  user_data           User data
 *  @return                     What to do with the message
 */
DBusHandlerResult
osspec_filter_function (DBusConnection * connection,
			DBusMessage * message, void *user_data)
{

/*
    HAL_INFO(("obj_path=%s interface=%s method=%s", 
              dbus_message_get_path(message), 
              dbus_message_get_interface(message),
              dbus_message_get_member(message)));
*/

	if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Linux.Hotplug",
					 "HotplugEvent") &&
	    strcmp (dbus_message_get_path (message),
		    "/org/freedesktop/Hal/Linux/Hotplug") == 0) {
		return handle_hotplug (connection, message);
	} else if (dbus_message_is_signal (message,
					   "org.kernel.udev.NodeMonitor",
					   "NodeCreated")) {
		return handle_udev_node_created (connection, message);
	} else if (dbus_message_is_signal (message, 
					   "org.kernel.udev.NodeMonitor", 
					   "NodeDeleted")) {
		    /* This is left intentionally blank */
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** @} */
