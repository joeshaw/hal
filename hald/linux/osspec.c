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
#include <stddef.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../callout.h"

#include "common.h"
#include "hald_helper.h"
#include "bus_device.h"
#include "class_device.h"

#include "libsysfs/libsysfs.h"

extern ClassDeviceHandler input_class_handler;
extern ClassDeviceHandler net_class_handler;
extern ClassDeviceHandler printer_class_handler;
extern ClassDeviceHandler scsi_host_class_handler;
extern ClassDeviceHandler scsi_device_class_handler;
extern ClassDeviceHandler scsi_generic_class_handler;
extern ClassDeviceHandler block_class_handler;
extern ClassDeviceHandler pcmcia_socket_class_handler;
extern ClassDeviceHandler ieee1394_class_handler;
extern ClassDeviceHandler ieee1394_node_class_handler;
extern ClassDeviceHandler ieee1394_host_class_handler;

extern BusDeviceHandler pci_bus_handler;
extern BusDeviceHandler usb_bus_handler;
extern BusDeviceHandler usbif_bus_handler;
extern BusDeviceHandler ide_host_bus_handler;
extern BusDeviceHandler ide_bus_handler;
extern BusDeviceHandler scsi_bus_handler;
extern BusDeviceHandler macio_bus_handler;
extern BusDeviceHandler platform_bus_handler;

/*
 * NOTE!  Order can be significant here, especially at startup time
 * when we're probing.  If we're expecting to find a parent device
 * and it's not there, things will complain.  (scsi_generic needs to
 * be after scsi_host, for example.)
 */
static ClassDeviceHandler* class_device_handlers[] = {
	&input_class_handler,
	&net_class_handler,
	&printer_class_handler,
	&scsi_host_class_handler,
	/*&scsi_device_class_handler,*/
	&scsi_generic_class_handler,
	&block_class_handler,
	&pcmcia_socket_class_handler,
	&ieee1394_host_class_handler,
	&ieee1394_node_class_handler,
	&ieee1394_class_handler,
	NULL
};

static BusDeviceHandler* bus_device_handlers[] = {
	&pci_bus_handler,
	&usb_bus_handler,
	&usbif_bus_handler,
	&ide_host_bus_handler,
        &ide_bus_handler,
	&macio_bus_handler,
	&platform_bus_handler,
	&scsi_bus_handler,
	NULL
};


static void hotplug_sem_up (void);
static void hotplug_sem_down (void);
static void hald_helper_hotplug (gboolean is_add, int seqnum, char *subsystem, char *sysfs_path);
static void hald_helper_device_node (gboolean is_add, char *subsystem, char *sysfs_path, char *device_node);
static gboolean hald_helper_data (GIOChannel *source, GIOCondition condition, gpointer user_data);

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
 *  @param  handler             A ClassDeviceHandler object to use or NULL to
 *                              try all handlers
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 *  @return                     A HalDevice pointer if the device is going
 *                              to be added to the GDL. The caller can
 *                              track the device by listening to signals
 *                              from this object. Returns NULL if the
 *                              device wasn't matched by any handler or
 *                              if it isn't going to be a separate hal
 *                              device object.
 */
static HalDevice *
visit_class_device (const char *path, ClassDeviceHandler *handler,
		    dbus_bool_t visit_children)
{
	int i;
	HalDevice *hal_device = NULL;
	struct sysfs_class_device *class_device;

	class_device = sysfs_open_class_device_path (path);
	if (class_device == NULL) {
		HAL_WARNING (("Coulnd't get sysfs class device object at "
			      "path %s", path));
		return NULL;
	}
	sysfs_get_classdev_device(class_device);
	sysfs_get_classdev_driver(class_device);

	/*HAL_INFO (("*** classname=%s path=%s",
		   class_device->classname,
		   class_device->path));*/

	if (handler != NULL) {
		if (handler->accept (handler, path, class_device)) {
			hal_device = handler->visit (handler, path, class_device);
		}
	} else {
		for (i=0; class_device_handlers[i] != NULL; i++) {
			ClassDeviceHandler *ch = class_device_handlers[i];
			if (ch->accept (ch, path, class_device)) {
				hal_device = ch->visit (ch, path, class_device);
				if (hal_device != NULL)
					break;
			}
		}
	}

	sysfs_close_class_device (class_device);

	if (visit_children) {
		struct sysfs_directory *dir;
		struct sysfs_directory *subdir;
		struct dlist *subdirs;

		dir = sysfs_open_directory(path);
		if (dir == NULL)
			return NULL;

		subdirs = sysfs_get_dir_subdirs(dir);
		if (subdirs == NULL) {
			sysfs_close_directory(dir);
			return NULL;
		}

		dlist_for_each_data (subdirs, subdir,
			     struct sysfs_directory)
			visit_class_device(subdir->path, handler, TRUE);

		sysfs_close_directory(dir);
	}

	return hal_device;
}

/** Visit all devices of a given class
 *
 *  @param  class_name          Name of class, e.g. scsi_host or block
 *  @param  handler             A ClassDeviceHandler object to use or NULL to
 *                              try all handlers
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void
visit_class (const char *class_name, ClassDeviceHandler *handler,
	     dbus_bool_t visit_children)
{
	struct sysfs_class *cls = NULL;
	struct sysfs_class_device *cur = NULL;
	struct dlist *class_devices;

	cls = sysfs_open_class (class_name);
	if (cls == NULL) {
		HAL_ERROR (("Error opening class %s\n", class_name));
		return;
	}

	class_devices = sysfs_get_class_devices(cls);
	if (class_devices != NULL) {
		dlist_for_each_data (class_devices, cur,
				     struct sysfs_class_device) {
			visit_class_device (cur->path, handler,
					    visit_children);
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
 *  @param  handler             A BusDeviceHandler object to use or NULL to try
 *                              all handlers
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 *  @return                     A HalDevice pointer if the device is going
 *                              to be added to the GDL. The caller can
 *                              track the device by listening to signals
 *                              from this object. Returns NULL if the
 *                              device wasn't matched by any handler.
 */
static HalDevice *
visit_device (const char *path, BusDeviceHandler *handler, 
	      dbus_bool_t visit_children)
{
	struct sysfs_device *device;
	HalDevice *hal_device = NULL;

	device = sysfs_open_device_path (path);
	if (device == NULL) {
		HAL_WARNING (("Coulnd't get sysfs device at path %s", path));
		return NULL;
	}

	if (handler != NULL ) {
		if (handler->accept (handler, device->path, device))
			hal_device = handler->visit (handler, device->path, device);
	} else {
		int i;
		for (i=0; bus_device_handlers[i] != NULL; i++) {
			BusDeviceHandler *bh = bus_device_handlers[i];
			if (bh->accept (bh, device->path, device)) {
				hal_device = bh->visit (bh, device->path, device);
				if (hal_device != NULL)
					break;
			}
		}
	}
	sysfs_close_device(device);

	if (visit_children) {
		struct sysfs_directory *dir;
		struct sysfs_directory *subdir;
		struct dlist *subdirs;

		dir = sysfs_open_directory(path);
		if (dir == NULL)
			return NULL;

		subdirs = sysfs_get_dir_subdirs(dir);
		if (subdirs == NULL) {
			sysfs_close_directory(dir);
			return NULL;
		}

		dlist_for_each_data (subdirs, subdir,
			     struct sysfs_directory)
			visit_device(subdir->path, handler, TRUE);

		sysfs_close_directory(dir);
	}

	return hal_device;
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
osspec_init (void)
{
	int i;
	int rc;
	int socketfd;
	struct sockaddr_un saddr;
	socklen_t addrlen;
	GIOChannel *channel;	
	const int on = 1;

	/* setup socket for listening from datagrams from the
	 * hal.hotplug and hal.dev helpers.
	 */
	memset(&saddr, 0x00, sizeof(saddr));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy(&saddr.sun_path[1], HALD_HELPER_SOCKET_PATH);
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(saddr.sun_path+1) + 1;

	socketfd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (socketfd == -1) {
		DIE (("Couldn't open socket"));
	}

	if (bind(socketfd, (struct sockaddr *) &saddr, addrlen) < 0) {
		DIE (("bind failed, exit"));
	}

	/* enable receiving of the sender credentials */
	setsockopt(socketfd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	channel = g_io_channel_unix_new (socketfd);
	g_io_add_watch (channel, G_IO_IN, hald_helper_data, NULL);
	g_io_channel_unref (channel);


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

	/* Setup timer */
	g_timeout_add (2000, osspec_timer_handler, NULL);	
}

/** This is set to #TRUE if we are probing and #FALSE otherwise */
dbus_bool_t hald_is_initialising;

/* This function is documented in ../osspec.h */
void
osspec_probe ()
{
	HalDevice *root;
	int i;

	hald_is_initialising = TRUE;

	/*
	 * Create the toplevel "Computer" device, which will contain
	 * system-wide info and also provide a parent device for devices
	 * which don't have sysdevices in sysfs.
	 */
	root = hal_device_new ();
	hal_device_property_set_string (root, "info.bus", "unknown");
	hal_device_property_set_string (root,
					"linux.sysfs_path_device",
					"(none)");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");
	hal_device_store_add (hald_get_gdl (), root);
	g_object_unref (root);

	/** @todo When the kernel has all devices in /sys/devices
	 *        under either /sys/bus or /sys/class then we can
	 *        have code like this

	for (i=0; bus_device_handlers[i] != NULL; i++) {
		BusDeviceHandler *bh = bus_device_handlers[i];
		visit_bus (bh->sysfs_bus_name, bh);
	}
	*/

	{
		char path[SYSFS_PATH_MAX];
		struct sysfs_directory *current;
		struct sysfs_directory *dir;
		struct dlist *subdirs;

		/* traverse /sys/devices */
		strncpy (path, sysfs_mount_path, SYSFS_PATH_MAX);
		strncat (path, "/", SYSFS_PATH_MAX);
		strncat (path, SYSFS_DEVICES_NAME, SYSFS_PATH_MAX);

		dir = sysfs_open_directory (path);
		if (dir == NULL) {
			DIE (("Error opening sysfs directory at %s\n", path));
		}
		subdirs = sysfs_get_dir_subdirs(dir);
		if (subdirs != NULL) {
			dlist_for_each_data (dir->subdirs, current,
					     struct sysfs_directory) {
				visit_device (current->path, NULL, TRUE);
			}
		}
		sysfs_close_directory (dir);
	}

	for (i=0; class_device_handlers[i] != NULL; i++) {
		ClassDeviceHandler *ch = class_device_handlers[i];
		visit_class (ch->sysfs_class_name, ch, TRUE);
	}

	hald_is_initialising = FALSE;
}

static void
remove_callouts_finished (HalDevice *d, gpointer user_data)
{
	HAL_INFO (("in remove_callouts_finished for udi=%s", d->udi));
	hal_device_store_remove (hald_get_gdl (), d);
}

static HalDevice *
remove_device (const char *path, const char *subsystem)

{
	HalDevice *d;

	d = hal_device_store_match_key_value_string (hald_get_gdl (), 
						     "linux.sysfs_path",
						     path);

	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device @ %s on hotplug remove", 
			      path));
	} else {
		/*HAL_INFO (("Removing device @ sysfspath %s, udi %s", 
		  path, d->udi));*/

		g_signal_connect (d, "callouts_finished",
				  G_CALLBACK (remove_callouts_finished), NULL);

		HAL_INFO (("in remove_device for udi=%s", d->udi));
		hal_callout_device (d, FALSE);
	}

	return d;
}

static HalDevice *
remove_class_device (const char *path, const char *subsystem)
{
	int i;
	const char *bus_name;
	HalDevice *d;

	d = hal_device_store_match_key_value_string (hald_get_gdl (), 
						     "linux.sysfs_path",
						     path);

	if (d == NULL) {
		/* Right now we only handle class devices that are put in the
		 * tree rather than merged, ie. merge_or_add is FALSE. That
		 * happens in the other branch below.
		 *
		 * What we need to do here is to unmerge the device from the
		 * sysdevice it belongs to. Ughh.. It's only a big deal when
		 * loading/unloading drivers and this should never happen
		 * on a desktop anyway?
		 *
		 * @todo FIXME
		 */

		HAL_WARNING (("Removal of class device @ %s on "
			      "hotplug remove is not yet implemented", path));

	} else {
		/*HAL_INFO (("Removing device @ sysfspath %s, udi %s", 
		  path, d->udi));*/

		bus_name = hal_device_property_get_string (d, "info.bus");

		for (i=0; class_device_handlers[i] != NULL; i++) {
			ClassDeviceHandler *ch = class_device_handlers[i];
			
			/* See class_device_visit() where this is merged */
			if (strcmp (ch->hal_class_name, bus_name) == 0) {
				ch->removed (ch, path, d);
			}
		}

		g_signal_connect (d, "callouts_finished",
				  G_CALLBACK (remove_callouts_finished), NULL);

		hal_callout_device (d, FALSE);
	}

	/* For now, just call the normal remove_device */
	/*remove_device (path, subsystem);*/

	return d;
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
		HAL_INFO (("dev_file=%s is for udi=%s", dev_file, d->udi));

		/*hal_device_print (d);*/

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
		HAL_WARNING (("No HAL device corresponding to device file %s",
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
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}



/* number of devices for whom the shutdown callouts are pending */
static int num_shutdown_devices_remaining;

static void
shutdown_callouts_finished (HalDevice *d, gpointer user_data)
{
	HAL_INFO (("entering for udi=%s", d->udi));

	num_shutdown_devices_remaining--;

	if (num_shutdown_devices_remaining == 0) {
		HAL_INFO (("All devices shutdown callouts done"));
		/* @todo Should return to hald.c though a gobject signal */
		exit (0);
	}
}


static gboolean
do_shutdown_callouts (HalDeviceStore *store, HalDevice *device,
		      gpointer user_data)
{
	HAL_INFO (("doing shutdown callouts for udi %s", device->udi));

	num_shutdown_devices_remaining++;

	g_signal_connect (device, "callouts_finished",
			  G_CALLBACK (shutdown_callouts_finished), NULL);
	hal_callout_device (device, FALSE);
	return TRUE;
}

/* This function is documented in ../osspec.h */
void
osspec_shutdown ()
{
	HAL_INFO (("entering"));

	num_shutdown_devices_remaining = 0;
	hal_device_store_foreach (hald_get_gdl (),
				  do_shutdown_callouts,
				  NULL);
}


static void
reenable_hotplug_proc (HalDevice *d, gpointer user_data)
{
	g_signal_handlers_disconnect_by_func (d, reenable_hotplug_proc, user_data);
	hotplug_sem_down ();
}

static void
hald_helper_hotplug (gboolean is_add, int seqnum, gchar *subsystem, gchar *sysfs_path)
{
	HalDevice *d = NULL;
	char sysfs_path_full[SYSFS_PATH_MAX];

	snprintf (sysfs_path_full, SYSFS_PATH_MAX, "%s%s", sysfs_mount_path, sysfs_path);

	HAL_INFO (("entering %s, SEQNUM=%d subsystem=%s devpath=%s devpath_full=%s",
		   (is_add ? "add" : "rem"), seqnum, subsystem, sysfs_path, sysfs_path_full));

	/* See if this is a class device or a bus device */
	if (strncmp (sysfs_path, "/block", 6)==0 ||
	    strncmp (sysfs_path, "/class", 6)==0 ) {
		/* handle class devices */
		if (is_add) {
			/* dunno what handler to use; try all */
			d = visit_class_device (sysfs_path_full, NULL, FALSE);
		} else {
			d = remove_class_device (sysfs_path_full, subsystem);
		}
	} else {
		/* handle bus devices */
		if (is_add) {
			/* Try to add the device */
			d = visit_device (sysfs_path_full, NULL, FALSE);
		} else {
			d = remove_device (sysfs_path_full, subsystem);
		}
	}

	if (d != NULL) {
		/* Ok, this leads to something; this hotplug event is going
		 * to result in adding/removing a device object to the GDL.
		 *
		 * Disable hotplug processing for now
		 */
		hotplug_sem_up ();
		
		/* and enable it when our device has processed all the
		 * callouts
		 */
		g_signal_connect (d, "callouts_finished",
				  G_CALLBACK (reenable_hotplug_proc), NULL);
	}


	g_free (subsystem);
	g_free (sysfs_path);
}

static void
hald_helper_device_node (gboolean is_add, gchar *subsystem, gchar *sysfs_path, gchar *device_node)
{
	char sysfs_path_full[SYSFS_PATH_MAX];

	snprintf (sysfs_path_full, SYSFS_PATH_MAX, "%s%s", sysfs_mount_path, sysfs_path);

	HAL_INFO (("entering %s, subsystem=%s devpath=%s devnode=%s",
		   (is_add ? "add" : "rem"), subsystem, sysfs_path, device_node));

	if (is_add ) {
		
		hal_device_store_match_key_value_string_async (
			hald_get_tdl (),
			".udev.sysfs_path",
			sysfs_path_full,
			udev_node_created_cb, 
			g_strdup (device_node),
			HAL_LINUX_HOTPLUG_TIMEOUT);

		/* NOTE: we will free the dupped device_node in the 
		 * async result function */

	} else {
		/* TODO FIXME: do something here :-) */
	}

	g_free (subsystem);
	g_free (sysfs_path);
	g_free (device_node);
}



/** queue of hotplug events (struct hald_helper_msg pointers) */
static GList *hotplug_queue = NULL;

/** Last hotplug sequence number */
static gint last_hotplug_seqnum = -1;

/** Hotplug semaphore */
static gint hotplug_counter = 0;

static void 
hald_helper_hotplug_process_queue (void)
{
	GList *i;
	struct hald_helper_msg *msg;

trynext:
	if (hotplug_counter > 0)
		return;

	for (i = hotplug_queue; i != NULL; i = g_list_next (i)) {
		msg = (struct hald_helper_msg *) i->data;

		if (msg->seqnum == last_hotplug_seqnum + 1) {
			/* yup, found it */
			last_hotplug_seqnum = msg->seqnum;
			hald_helper_hotplug (msg->is_add, msg->seqnum, g_strdup (msg->subsystem), 
					     g_strdup (msg->sysfs_path));
			g_free (msg);
			hotplug_queue = g_list_delete_link (hotplug_queue, i);
			goto trynext;
		}
	}
}

/** Increment the hotplug semaphore; useful when not wanting to process
 *  hotplug events for a while, like when e.g. adding a hal device
 *  object (which is an asynchronous operation).
 *
 *  Remember to release with hotplug_sem_down.
 */
static void 
hotplug_sem_up (void)
{
	++hotplug_counter;
}

/** Decrement the hotplug semaphore. 
 *
 */
static void 
hotplug_sem_down (void)
{
	--hotplug_counter;

	if (hotplug_counter < 0) {
		HAL_ERROR (("****************************************"));
		HAL_ERROR (("****************************************"));
		HAL_ERROR (("DANGER WILL ROBISON! hotplug semaphore<0!"));
		HAL_ERROR (("****************************************"));
		HAL_ERROR (("****************************************"));
		hotplug_counter = 0;
	}

	/* Process remaining hotplug events */
	if (hotplug_counter == 0)
		hald_helper_hotplug_process_queue ();
}

static gboolean
hald_helper_first_hotplug_event (gpointer data)
{
	GList *i;
	struct hald_helper_msg *msg;

	last_hotplug_seqnum = G_MAXINT;
	/* find the seqnum we should start with */
	for (i = hotplug_queue; i != NULL; i = g_list_next (i)) {
		msg = (struct hald_helper_msg *) i->data;
		if (msg->seqnum < last_hotplug_seqnum)
			last_hotplug_seqnum = msg->seqnum;
	}
	--last_hotplug_seqnum;

	HAL_INFO (("Starting with SEQNUM=%d", last_hotplug_seqnum+1));

	hotplug_sem_down ();

	/* no further timer event */
	return FALSE;
}

static gboolean
hald_helper_data (GIOChannel *source, 
		  GIOCondition condition, 
		  gpointer user_data)
{
	struct hald_helper_msg msg;
	int fd;
	int retval;
	struct msghdr smsg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct ucred *cred;
	char cred_msg[CMSG_SPACE(sizeof(struct ucred))];

	fd = g_io_channel_unix_get_fd (source);

	iov.iov_base = &msg;
	iov.iov_len = sizeof (struct hald_helper_msg);

	memset(&smsg, 0x00, sizeof (struct msghdr));
	smsg.msg_iov = &iov;
	smsg.msg_iovlen = 1;
	smsg.msg_control = cred_msg;
	smsg.msg_controllen = sizeof (cred_msg);

	retval = recvmsg (fd, &smsg, 0);
	if (retval <  0) {
		if (errno != EINTR)
			HAL_INFO (("Unable to receive message, errno=%d", errno));
		goto out;
	}
	cmsg = CMSG_FIRSTHDR (&smsg);
	cred = (struct ucred *) CMSG_DATA (cmsg);

	if (cmsg == NULL || cmsg->cmsg_type != SCM_CREDENTIALS) {
		HAL_INFO (("No sender credentials received, message ignored"));
		goto out;
	}

	if (cred->uid != 0) {
		HAL_INFO (("Sender uid=%i, message ignored", cred->uid));
		goto out;
	}

	if (msg.magic != HALD_HELPER_MAGIC) {
		HAL_INFO (("Magic is wrong, message ignored", cred->uid));
		goto out;
	}

	if (!msg.is_hotplug_or_dev) {
		/* device events doesn't have seqnum on them, however udev also respect sequence numbers */
		hald_helper_device_node (msg.is_add, g_strdup (msg.subsystem), g_strdup (msg.sysfs_path), 
					 g_strdup (msg.device_node));
		goto out;
	}

	/* need to process hotplug events in proper sequence */

	/*HAL_INFO (("Before reordering, SEQNUM=%d, last_hotplug_seqnum=%d, subsystem=%s, sysfs=%s", 
	  msg.seqnum, last_hotplug_seqnum, msg.subsystem, msg.sysfs_path));*/

	if (last_hotplug_seqnum == -1 ) {
		/* gotta start somewhere; however sleep one second to allow  
		 * some more hotplug events to propagate so we know where
		 * we're at.
		 */

		HAL_WARNING (("First SEQNUM=%d; sleeping 2500ms to get a few more events", msg.seqnum));

		hotplug_sem_up ();
		g_timeout_add (2500, hald_helper_first_hotplug_event, NULL);

		/* so we only setup one timer */
		last_hotplug_seqnum = -2;
	}

	if (msg.seqnum < last_hotplug_seqnum) {
		/* yikes, this means were started during a hotplug */
		HAL_WARNING (("Got SEQNUM=%d, but last_hotplug_seqnum=%d", msg.seqnum, last_hotplug_seqnum));

		/* have to process immediately other we may deadlock due to
		 * the hotplug semaphore */
		hald_helper_hotplug (msg.is_add, msg.seqnum, g_strdup (msg.subsystem), 
				     g_strdup (msg.sysfs_path));
		/* still need to process the queue though */
		hald_helper_hotplug_process_queue ();
		goto out;
	}

	/* Queue up this hotplug event and process the queue */
	hotplug_queue = g_list_append (hotplug_queue, g_memdup (&msg, sizeof (struct hald_helper_msg)));
	hald_helper_hotplug_process_queue ();

out:
	return TRUE;
}

/** @} */
