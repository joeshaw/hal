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
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

#include "common.h"
#include "hald_helper.h"
#include "bus_device.h"
#include "class_device.h"

#include "libsysfs/libsysfs.h"

/** How many ms to sleep on first hotplug event (to queue up other hotplug events) */
#define FIRST_HOTPLUG_SLEEP 3500

/** How many seconds before we discard a missing hotplug event and move on to the next one */
#define HOTPLUG_TIMEOUT 15

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
extern ClassDeviceHandler serial_class_handler;
extern ClassDeviceHandler multimedia_class_handler;

extern BusDeviceHandler pci_bus_handler;
extern BusDeviceHandler usb_bus_handler;
extern BusDeviceHandler usbif_bus_handler;
extern BusDeviceHandler ide_host_bus_handler;
extern BusDeviceHandler ide_bus_handler;
extern BusDeviceHandler scsi_bus_handler;
extern BusDeviceHandler macio_bus_handler;
extern BusDeviceHandler platform_bus_handler;
extern BusDeviceHandler usb_serial_bus_handler;

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
	&serial_class_handler,
	&multimedia_class_handler,
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
	&usb_serial_bus_handler,
	NULL
};


static void hotplug_sem_up (void);
static void hotplug_sem_down (void);
static void hald_helper_hotplug (gchar *action, guint64 seqnum, gchar *subsystem, 
				 gchar *sysfs_path, struct hald_helper_msg *msg);
static void hald_helper_device_name (gchar *action, guint64 seqnum, gchar *subsystem, 
				     gchar *sysfs_path, gchar *device_name, struct hald_helper_msg *msg);
static gboolean hald_helper_data (GIOChannel *source, GIOCondition condition, gpointer user_data);

static HalDevice *add_device (const char *sysfs_path, const char *subsystem, struct hald_helper_msg *msg);

static void hotplug_timeout_handler (void);

/** Mount path for sysfs */
char sysfs_mount_path[SYSFS_PATH_MAX];

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

	hotplug_timeout_handler ();

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
		fprintf (stderr, "Error binding to %s: %s\n", HALD_HELPER_SOCKET_PATH, strerror(errno));
		exit (1);
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

typedef struct {
	gchar *first;
	gchar *second;
} HStringPair;

static HStringPair *
h_string_pair_new (gchar *first, gchar *second)
{
	HStringPair *pair;

	pair = g_new0 (HStringPair, 1);
	pair->first = first;
	pair->second = second;
	return pair;
}


static void
h_string_pair_delete (gpointer p)
{
	HStringPair *pair = p;
	g_free (pair->first);
	g_free (pair->second);
	g_free (pair);
}


/** Mapping from sysfs path to subsystem for bus devices. This is consulted
 *  when traversing /sys/devices
 *
 *  Example:
 *
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0/host7/7:0:0:0  -> scsi
 * /sys/devices/pci0000:00/0000:00:07.1/ide1/1.1                        -> ide
 * /sys/devices/pci0000:00/0000:00:07.1/ide1/1.0                        -> ide
 * /sys/devices/pci0000:00/0000:00:07.1/ide0/0.0                        -> ide
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0                -> usb
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1                        -> usb
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-0:1.0                    -> usb
 * /sys/devices/pci0000:00/0000:00:07.2/usb1                            -> usb
 * /sys/devices/pci0000:00/0000:00:04.1/0000:06:00.0                    -> pci
 * /sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0                    -> pci
 * /sys/devices/pci0000:00/0000:00:08.0                                 -> pci
 * /sys/devices/platform/vesafb0                                        -> platform
 */
GHashTable *sysfs_to_bus_map = NULL;

/** Mapping from sysfs path in /sys/devices to the pair (sysfs class path, classname)
 *  for class devices. 
 *
 *  Only used for class devices that appear in the /sys/devices/ tree, e.g. when traversing
 *  /sys/devices and a match wasn't found in sysfs_to_bus_map.
 *
 *  (e.g. scsi_host appear in /sys/devices tree but not in /sys/bus)
 *
 * Example:
 *
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0/host7/7:0:0:0 -> (/sys/class/scsi_device/7:0:0:0, scsi_device)
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0/host7         -> (/sys/class/scsi_host/host7, scsi_host)
 * /sys/devices/pci0000:00/0000:00:08.0                                -> (/sys/class/sound/pcmC0D0c, sound)
 * /sys/devices/pci0000:00/0000:00:08.0                                -> (/sys/class/sound/pcmC0D0p, sound)
 * /sys/devices/pci0000:00/0000:00:08.0                                -> (/sys/class/sound/midiC0D0, sound)
 * /sys/devices/pci0000:00/0000:00:04.1                      -> (/sys/class/pcmcia_socket/pcmcia_socket1, pcmcia_socket)
 * /sys/devices/pci0000:00/0000:00:04.0                      -> (/sys/class/pcmcia_socket/pcmcia_socket0, pcmcia_socket)
 * /sys/devices/pci0000:00/0000:00:04.1/0000:06:00.0         -> (/sys/class/net/eth1, net)
 * /sys/devices/pci0000:00/0000:00:07.2                      -> (/sys/class/usb_host/usb1, usb_host)
 */
GHashTable *sysfs_to_class_in_devices_map = NULL;

/** Mapping from devices in /sys/devices that has a link to a top-level
 *  block devices
 *
 * Example:
 *
 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0/host7/7:0:0:0  -> /sys/block/sda
 * /sys/devices/pci0000:00/0000:00:07.1/ide1/1.0                        -> /sys/block/hdc
 * /sys/devices/pci0000:00/0000:00:07.1/ide1/1.1                        -> /sys/block/hdd
 * /sys/devices/pci0000:00/0000:00:07.1/ide0/0.0                        -> /sys/block/hda
 */
GHashTable *sysfs_to_block_map = NULL;

/** Mapping from sysfs path in /sys/class to class type
 *
 * Example:
 *
 * /sys/class/scsi_device/7:0:0:0             -> scsi_device
 * /sys/class/scsi_host/host7                 -> scsi_host
 * /sys/class/sound/pcmC0D0c                  -> sound
 * /sys/class/sound/pcmC0D0p                  -> sound
 * /sys/class/sound/midiC0D0                  -> sound
 * /sys/class/pcmcia_socket/pcmcia_socket1    -> pcmcia_socket
 * /sys/class/pcmcia_socket/pcmcia_socket0    -> pcmcia_socket
 * /sys/class/net/eth1                        -> net
 * /sys/class/usb_host/usb1                   -> usb_host
 */
GHashTable *sysfs_to_class_map = NULL;

static gchar *
get_normalized_path (const gchar *path1, const gchar *path2)
{
	int i;
	int len1;
	int len2;
	const gchar *p1;
	const gchar *p2;
	gchar buf[SYSFS_PATH_MAX];

	len1 = strlen (path1);
	len2 = strlen (path1);

	p1 = path1 + len1;

	i = 0;
	p2 = path2;
	while (p2 < path2 + len2 && strncmp (p2, "../", 3) == 0) {
		p2 += 3;

		while (p1 >= path1 && *(--p1)!='/')
			;

	}

	strncpy (buf, path1, (p1-path1));
	buf[p1-path1] = '\0';

	return g_strdup_printf ("%s/%s", buf, p2);
}

static void compute_coldplug_visit_device (const gchar *path, GSList **ordered_sysfs_list);

static void compute_coldplug_visit_class_device (gpointer key, gpointer value, gpointer user_data);

/** This function has one major purpose : build an ordered list of pairs (sysfs path, subsystem)
 *  to process when starting up; analogue to coldplugging.
 *
 *  @return                     Ordered list of sysfs paths or NULL if there was an error
 */
static GSList *
compute_coldplug_list (void)
{
	GDir *dir;
	GError *err = NULL;
	gchar path[SYSFS_PATH_MAX];
	gchar path1[SYSFS_PATH_MAX];
	const gchar *f;
	const gchar *f1;
	const gchar *f2;
	GSList *coldplug_list = NULL;

	/* build bus map */
	sysfs_to_bus_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	g_snprintf (path, SYSFS_PATH_MAX, "%s/bus" , sysfs_mount_path);
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/bus: %s", sysfs_mount_path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		GDir *dir1;

		g_snprintf (path, SYSFS_PATH_MAX, "%s/bus/%s" , sysfs_mount_path, f);
		if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %/bus/%s: %s", sysfs_mount_path, f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {

			if (strcmp (f1, "devices") == 0) {
				GDir *dir2;

				g_snprintf (path, SYSFS_PATH_MAX, "%s/bus/%s/%s", 
					    sysfs_mount_path, f, f1);
				if ((dir2 = g_dir_open (path, 0, &err)) == NULL) {
					HAL_ERROR (("Unable to open %s/bus/%s/%s: %s", 
						    sysfs_mount_path, f, f1, err->message));
					g_error_free (err);
					goto error;
				}
				while ((f2 = g_dir_read_name (dir2)) != NULL) {
					gchar *target;
					gchar *normalized_target;
					g_snprintf (path, SYSFS_PATH_MAX, "%s/bus/%s/%s/%s", 
						    sysfs_mount_path, f, f1, f2);
					if ((target = g_file_read_link (path, &err)) == NULL) {
						HAL_ERROR (("%s/bus/%s/%s/%s is not a symlink: %s!", 
							    sysfs_mount_path, 
							    f, f1, f2, err->message));
						g_error_free (err);
						goto error;
					}

					g_snprintf (path, SYSFS_PATH_MAX, "%s/bus/%s/%s", sysfs_mount_path, f, f1);
					normalized_target = get_normalized_path (path, target);
					g_free (target);

					/*printf ("%s -> %s\n", normalized_target, f);*/
					g_hash_table_insert (sysfs_to_bus_map, normalized_target, g_strdup(f));

				}
				g_dir_close (dir2);
			}
		}
		g_dir_close (dir1);
	}
	g_dir_close (dir);

	/* build class map and class device map */
	sysfs_to_class_in_devices_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, h_string_pair_delete);
	sysfs_to_class_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	g_snprintf (path, SYSFS_PATH_MAX, "%s/class" , sysfs_mount_path);
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/class: %s", sysfs_mount_path, err->message));
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		GDir *dir1;

		g_snprintf (path, SYSFS_PATH_MAX, "%s/class/%s" , sysfs_mount_path, f);
		if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %/class/%s: %s", sysfs_mount_path, f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {
			gchar *target;
			gchar *normalized_target;

			g_snprintf (path, SYSFS_PATH_MAX, "%s/class/%s/%s/device", sysfs_mount_path, f, f1);
			if ((target = g_file_read_link (path, NULL)) != NULL) {
				g_snprintf (path1, SYSFS_PATH_MAX, "%s/class/%s/%s", sysfs_mount_path, f, f1);
				normalized_target = get_normalized_path (path1, target);
				g_free (target);

				/*printf ("%s -> (%s, %s)\n", normalized_target, path1, f);*/
				g_hash_table_insert (sysfs_to_class_in_devices_map, 
						     normalized_target, 
						     h_string_pair_new (g_strdup (path1), g_strdup(f)));

				g_hash_table_insert (sysfs_to_class_map, g_strdup (path1), g_strdup(f));

			}				
		}
		g_dir_close (dir1);
	}
	g_dir_close (dir);

	/* build block map */
	sysfs_to_block_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	g_snprintf (path, SYSFS_PATH_MAX, "%s/block" , sysfs_mount_path);
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/block: %s", sysfs_mount_path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		gchar *target;
		gchar *normalized_target;

		g_snprintf (path, SYSFS_PATH_MAX, "%s/block/%s/device", sysfs_mount_path, f);
		if ((target = g_file_read_link (path, NULL)) != NULL) {
			g_snprintf (path, SYSFS_PATH_MAX, "%s/block/%s", sysfs_mount_path, f);
			normalized_target = get_normalized_path (path, target);
			/*printf ("%s -> %s\n",  normalized_target, path);*/
			g_free (target);
			g_hash_table_insert (sysfs_to_block_map, normalized_target, g_strdup(path));
		}
		
	}
	g_dir_close (dir);

	/* The goal is to build an ordered list of (sysfs path, subsystem) to process - 
	 * one thing to keep in mind is that we may have several BusDeviceHandlers and 
	 * ClassDeviceHandlers for the same bustype or classtype. Hence, we just check
	 * if the first one matches. When we process the list the accept() method
	 * on the Handler will select only the correct one.
	 */

	/*
	 * The final list looks like this on my system
	 *
	 * bus:   /sys/devices/pci0000:00/0000:00:08.0 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.3 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.2 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.2/usb1 (usb)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1 (usb)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0 (usb)
	 * class: /sys/class/scsi_host/host7 (scsi_host)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0/host7/7:0:0:0 (scsi)
	 * block: /sys/block/sda (block)
	 * block: /sys/block/sda/sda1 (block)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.2/usb1/1-0:1.0 (usb)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.1 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.1/ide1 (ide_host)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.1/ide1/1.1 (ide)
	 * block: /sys/block/hdd (block)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.1/ide1/1.0 (ide)
	 * block: /sys/block/hdc (block)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.1/ide0 (ide_host)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.1/ide0/0.0 (ide)
	 * block: /sys/block/hda (block)
	 * block: /sys/block/hda/hda2 (block)
	 * block: /sys/block/hda/hda1 (block)
	 * bus:   /sys/devices/pci0000:00/0000:00:07.0 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:04.1 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:04.1/0000:06:00.0 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:04.0 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:01.0 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0 (pci)
	 * bus:   /sys/devices/pci0000:00/0000:00:00.0 (pci)
	 * bus:   /sys/devices/platform/vesafb0 (platform)
	 * class: /sys/class/pcmcia_socket/pcmcia_socket0 (pcmcia_socket)
	 * class: /sys/class/net/eth1 (net)
	 * class: /sys/class/pcmcia_socket/pcmcia_socket1 (pcmcia_socket)
	 */

	/* First traverse /sys/devices and consult the maps we've built; this
	 * includes adding a) bus devices; and b) class devices that sit in /sys/devices */
	g_snprintf (path, SYSFS_PATH_MAX, "%s/devices" , sysfs_mount_path);
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/devices: %s", sysfs_mount_path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		GDir *dir1;

		g_snprintf (path, SYSFS_PATH_MAX, "%s/devices/%s" , sysfs_mount_path, f);
		if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %/devices/%s: %s", sysfs_mount_path, f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {

			g_snprintf (path, SYSFS_PATH_MAX, "%s/devices/%s/%s" , sysfs_mount_path, f, f1);
			compute_coldplug_visit_device (path, &coldplug_list);

		}
		g_dir_close (dir1);
	}
	g_dir_close (dir);

	/* Then add all the class devices that doesn't sit in the /sys/devices tree */
	g_hash_table_foreach (sysfs_to_class_map, compute_coldplug_visit_class_device, &coldplug_list);

	g_hash_table_destroy (sysfs_to_bus_map);
	g_hash_table_destroy (sysfs_to_class_in_devices_map);
	g_hash_table_destroy (sysfs_to_block_map);
	g_hash_table_destroy (sysfs_to_class_map);

	return coldplug_list;
error:
	HAL_ERROR (("Error building the orderered list of sysfs paths"));
	return NULL;
}

static void
compute_coldplug_visit_class_device (gpointer key, gpointer value, gpointer user_data)
{
	int i;
	gchar *path = key;
	gchar *class = value;
	GSList **coldplug_list = user_data;;

	for (i = 0; class_device_handlers[i] != NULL; i++) { 
		ClassDeviceHandler *ch = class_device_handlers[i];
		
		if (ch->merge_or_add && strcmp (class, ch->sysfs_class_name) == 0) {

			/*printf ("class: %s (%s)\n", path, class);*/
			*coldplug_list = g_slist_append (*coldplug_list, 
							 h_string_pair_new (g_strdup (path), g_strdup (class)));
			return;
		}
	}
}

static void
compute_coldplug_visit_device (const gchar *path, GSList **coldplug_list) 
{
	int i;
	gchar *bus;
	HStringPair *pair;
	gchar *block;
	GError *err;
	GDir *dir;
	const gchar *f;
		
	bus = g_hash_table_lookup (sysfs_to_bus_map, path);
	if (bus != NULL) {
		for (i = 0; bus_device_handlers[i] != NULL; i++) {	
			BusDeviceHandler *bh = bus_device_handlers[i];

			if (strcmp (bus, bh->sysfs_bus_name) == 0) {
				/*printf ("bus:   %s (%s)\n", path, bus);*/
				*coldplug_list = g_slist_append (*coldplug_list, 
								 h_string_pair_new (g_strdup (path), g_strdup (bus)));
				goto found;
			}
		}
	}

	pair = g_hash_table_lookup (sysfs_to_class_in_devices_map, path);
	if (pair != NULL) {
		gchar *classpath;
		gchar *class;

		classpath = pair->first;
		class = pair->second;

		for (i = 0; class_device_handlers[i] != NULL; i++) { 
			ClassDeviceHandler *ch = class_device_handlers[i];

			if (!ch->merge_or_add && strcmp (class, ch->sysfs_class_name) == 0) {
				/*printf ("class: %s (%s)\n", classpath, class);*/
				*coldplug_list = g_slist_append (*coldplug_list, 
								 h_string_pair_new (g_strdup (classpath),
										    g_strdup (class)));
				goto found;
			}
		}
 	}

	/* know entries in sysfs that isn't in /sys/bus or /sys/class but we support
	 * anyway so we need to fake them here
	 *
	 * - ide_host (the ide%d entries)
	 * 
	 *  bus:   /sys/devices/pci0000:00/0000:00:07.1/ide1 -> ide_host
	 *  bus:   /sys/devices/pci0000:00/0000:00:07.1/ide0 -> ide_host
	 */
	if (sscanf (get_last_element (path), "ide%d", &i) == 1) {
		bus = "ide_host";
		/*printf ("bus:   %s (%s)\n", path, bus);*/
		*coldplug_list = g_slist_append (*coldplug_list, 
						 h_string_pair_new (g_strdup (path), g_strdup (bus)));
		goto found;
	}
	return;

found:

	/* visit children */
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %: %s", path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		gchar path_child[SYSFS_PATH_MAX];
		
		g_snprintf (path_child, SYSFS_PATH_MAX, "%s/%s", path, f);
		
		compute_coldplug_visit_device (path_child, coldplug_list);
	}
	g_dir_close (dir);

error:
	/* check for block devices */
	block = g_hash_table_lookup (sysfs_to_block_map, path);
	if (block != NULL) {
		const char *dev;
		int devlen;
		
		/*printf ("block: %s (block)\n", block);*/
		*coldplug_list = g_slist_append (*coldplug_list, 
						 h_string_pair_new (g_strdup (block), g_strdup("block")));

		dev = get_last_element (block);
		devlen = strlen (dev);

		/* process block children */
		if ((dir = g_dir_open (block, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %: %s", path, err->message));
			g_error_free (err);
			goto error1;
		}
		while ((f = g_dir_read_name (dir)) != NULL) {
			gchar path_child[SYSFS_PATH_MAX];
			
			g_snprintf (path_child, SYSFS_PATH_MAX, "%s/%s", block, f);
			if (strncmp (f, dev, devlen) == 0) {
				/*printf ("block: %s (block)\n", path_child);*/
				*coldplug_list = g_slist_append (*coldplug_list, 
								 h_string_pair_new (g_strdup (path_child),
										    g_strdup ("block")));
			}
		}
		g_dir_close (dir);

 	}
error1:
	return;
}

/* global list of devices to be coldplugged */
static GSList *coldplug_list;

static void process_coldplug_list ();

static void process_coldplug_list_device_cancelled (HalDevice *device, gpointer user_data);

static void
process_coldplug_list_on_gdl_store_add (HalDeviceStore *store, HalDevice *device, gboolean is_added, gpointer user_data)
{
	HalDevice *waiting_device = (HalDevice *) user_data;

	if (waiting_device == device && is_added) {
		g_signal_handlers_disconnect_by_func (store, process_coldplug_list_on_gdl_store_add, user_data);
		g_signal_handlers_disconnect_by_func (device, process_coldplug_list_device_cancelled, user_data);

		process_coldplug_list ();
	}
}

static void
process_coldplug_list_device_cancelled (HalDevice *device, gpointer user_data)
{
	g_signal_handlers_disconnect_by_func (hald_get_gdl (), process_coldplug_list_on_gdl_store_add, user_data);
	g_signal_handlers_disconnect_by_func (device, process_coldplug_list_device_cancelled, user_data);

	process_coldplug_list ();
}

static void
process_coldplug_list ()
{
	gchar *path;
	gchar *subsystem;
	HalDevice *device;

	if (coldplug_list != NULL) {
		HStringPair *pair;

		pair = coldplug_list->data;
		path = pair->first;
		subsystem = pair->second;
		coldplug_list = g_slist_delete_link (coldplug_list, coldplug_list);

		HAL_INFO (("handling %s %s", path, subsystem));
		device = add_device (path, subsystem, NULL);//visit_device (path, NULL, FALSE, 0);
		g_free (path);
		g_free (subsystem);
		g_free (pair);

		if (device != NULL && hal_device_store_find(hald_get_gdl (), device->udi) == NULL) {

			/* wait until we are in GDL */
			g_signal_connect (hald_get_gdl (), "store_changed", 
					  G_CALLBACK (process_coldplug_list_on_gdl_store_add), device);

			/* or until we are cancelled */
			g_signal_connect (device, "cancelled",
					  G_CALLBACK (process_coldplug_list_device_cancelled), device);
		} else {
			process_coldplug_list ();
		}


	} else {
		/* Inform the generic part of hald that we are done with probing */
		osspec_probe_done ();

		/* Enabling handling of hotplug events */
		hotplug_sem_down ();
	}
}


static void
add_computer_callouts_done (HalDevice *device, gpointer user_data)
{
	g_object_ref (device);
	hal_device_store_remove (hald_get_tdl (), device);
	hal_device_store_add (hald_get_gdl (), device);
	g_signal_handlers_disconnect_by_func (device,
					      add_computer_callouts_done,
					      user_data);
	g_object_unref (device);

	process_coldplug_list ();
}

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>

#if 0
static int get_selinux_removable_context(security_context_t *newcon)
{
	FILE *fp;
	char buf[255], *ptr;
	size_t plen;
	
	HAL_INFO (("selinux_removable_context_path '%s'", selinux_removable_context_path()));
	fp = fopen(selinux_removable_context_path(), "r");
	if (!fp)
		return -1;
	
	ptr = fgets_unlocked(buf, sizeof buf, fp);
	fclose(fp);
	
	if (!ptr)
		return -1;
	plen = strlen(ptr);
	if (buf[plen-1] == '\n') 
		buf[plen-1] = 0;
	
	*newcon=strdup(buf);
	/* If possible, check the context to catch
	   errors early rather than waiting until the
	   caller tries to use setexeccon on the context.
	   But this may not always be possible, e.g. if
	   selinuxfs isn't mounted. */
	if (security_check_context(*newcon) && errno != ENOENT) {
		free(*newcon);
		*newcon = 0;
		return -1;
	}
	
	HAL_INFO (("removable context is %s", *newcon));
	return 0;
}
#endif
#endif /* HAVE_SELINUX */

/* This function is documented in ../osspec.h */
void
osspec_probe (void)
{
	HalDevice *root;
	struct utsname un;
	
	/* build the coldplug list */
	coldplug_list = compute_coldplug_list ();

	/* disable handling of hotplug events */
	hotplug_sem_up ();

	/* Create the toplevel "Computer" device, which will contain
	 * system-wide info and also provide a parent device for devices
	 * which don't have sysdevices in sysfs.
	 */
	root = hal_device_new ();
	hal_device_property_set_string (root, "info.bus", "unknown");
	hal_device_property_set_string (root, "linux.sysfs_path_device", "(none)");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_property_set_string (root, "info.udi", "/org/freedesktop/Hal/devices/computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");
	
	if (uname (&un) >= 0) {
		hal_device_property_set_string (root, "kernel.name", un.sysname);
		hal_device_property_set_string (root, "kernel.version", un.release);
		hal_device_property_set_string (root, "kernel.machine", un.machine);
	}

#ifdef HAVE_SELINUX
	if (is_selinux_enabled()) {
/*
		char buf[256];
		security_context_t scontext;
*/
		hal_device_property_set_bool (root, "linux.is_selinux_enabled", TRUE);

/*
		if (get_selinux_removable_context(&scontext)==0) {
			snprintf (buf, sizeof (buf), "storage.policy.default.mount_option.fscontext=%s", scontext);
			freecon(scontext);
			hal_device_property_set_bool (root, buf, TRUE);
		} else {
			HAL_ERROR (("Could not get selinux removable fscontext"));
		}
*/
	}
#endif /* HAVE_SELINUX */

	hal_device_store_add (hald_get_tdl (), root);

	/* Search for device information file and attempt merge */
	if (di_search_and_merge (root)) {
		HAL_INFO (("Found a .fdi file for %s", root->udi));
	}
	
	/* add possible saved properties for this udi from disk*/
	if (hald_get_conf ()->persistent_device_list)
		hal_pstore_load_device (hald_get_pstore_sys (), root);

	/* begin processing the coldplug_list when computer is added */
	g_signal_connect (root,
			  "callouts_finished",
			  G_CALLBACK (add_computer_callouts_done),
			  coldplug_list);

	hal_callout_device (root, TRUE);
}

static gboolean
recover_net_device (int net_ifindex, char *sysfs_path, size_t sysfs_path_size)
{
	GDir *dir;
	char path[SYSFS_PATH_MAX];
	char path1[SYSFS_PATH_MAX];
	GError *err = NULL;
	const gchar *f;

	g_snprintf (path, SYSFS_PATH_MAX, "%s/class/net" , sysfs_mount_path);
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/class/net: %s", sysfs_mount_path, err->message));
		g_error_free (err);
		return FALSE;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		FILE *file;

		g_snprintf (path1, SYSFS_PATH_MAX, "%s/class/net/%s/ifindex" , sysfs_mount_path, f);
		HAL_INFO (("Looking at %s", path1));

		file = fopen (path1, "r");
		if (file != NULL) {
			char buf[80];
			int found_ifindex;

			memset (buf, 0, sizeof (buf));
			fread (buf, sizeof (buf) - 1, 1, file);
			fclose (file);

			found_ifindex = atoi(buf);
			if (found_ifindex == net_ifindex) {
				g_snprintf (sysfs_path, sysfs_path_size, "%s/class/net/%s" , sysfs_mount_path, f);
				g_dir_close (dir);
				return TRUE;
			}
		}

	}
	g_dir_close (dir);	

	return FALSE;
}

static HalDevice *
add_device (const char *given_sysfs_path, const char *subsystem, struct hald_helper_msg *msg) /* msg may be NULL */
{
	int i;
	int len1;
	int len2;
	char buf1[SYSFS_PATH_MAX];
	char buf2[SYSFS_PATH_MAX];
	char sysfs_path[SYSFS_PATH_MAX];
	HalDevice *hal_device = NULL;

	if (given_sysfs_path == NULL) {
		HAL_WARNING (("given_sysfs_path is NULL, cannot add this device" ));
		return NULL;
	}

	strncpy (sysfs_path, given_sysfs_path, SYSFS_PATH_MAX);

	len1 = snprintf (buf1, SYSFS_PATH_MAX, "%s/block", sysfs_mount_path);
	len2 = snprintf (buf2, SYSFS_PATH_MAX, "%s/class", sysfs_mount_path);
	if (strncmp (sysfs_path, buf1, len1) == 0 || strncmp (sysfs_path, buf2, len2) == 0) {
		for (i=0; class_device_handlers[i] != NULL; i++) {
			ClassDeviceHandler *ch = class_device_handlers[i];
			struct sysfs_class_device *class_device;

			class_device = sysfs_open_class_device_path (sysfs_path);
			if (class_device == NULL) {

				if (msg != NULL && msg->net_ifindex >= 0 && strcmp (subsystem, "net") == 0) {
					/* Hey, special case net devices; some hotplug script might have renamed it..
					 * So, our hotplug helper reads the ifindex file for us so we can recover it.
					 *
					 * This is quite a hack; once hotplug and netlink networking messages are 
					 * properly interleaved (with SEQNUM and stuff) this problem will go away;
					 * until then we need to live with this hack :-/
					 */
					HAL_INFO (("Attempting to recover renamed netdevice with ifindex=%d",
						   msg->net_ifindex));
					/* traverse all /sys/class/net/<netdevice> and check ifindex file */
					if (recover_net_device (msg->net_ifindex, sysfs_path, sizeof (sysfs_path))) {
						class_device = sysfs_open_class_device_path (sysfs_path);
						if (class_device == NULL) {
							HAL_INFO (("%s didn't work...", sysfs_path));
							return NULL;
						} else {
							HAL_INFO (("%s worked!", sysfs_path));
						}
					}

				} else {
					HAL_WARNING (("Coulnd't get sysfs class device object at path %s", sysfs_path));
					return NULL;
				}
			}
			sysfs_get_classdev_device(class_device);
			sysfs_get_classdev_driver(class_device);

			if (strcmp (ch->sysfs_class_name, subsystem) == 0) {
				if (ch->accept (ch, sysfs_path, class_device)) {
					hal_device = ch->visit (ch, sysfs_path, class_device);
					if (hal_device != NULL) {
						break;
					}
				}
			}
			sysfs_close_class_device (class_device);
		}
	} else {
		for (i=0; bus_device_handlers[i] != NULL; i++) {
			BusDeviceHandler *bh = bus_device_handlers[i];
			struct sysfs_device *device;

			device = sysfs_open_device_path (sysfs_path);
			if (device == NULL) {
				HAL_WARNING (("Coulnd't get sysfs class device object at "
					      "path %s", sysfs_path));
				return NULL;
			}
			if (strcmp (bh->sysfs_bus_name, subsystem) == 0) {
				if (bh->accept (bh, sysfs_path, device)) {
					hal_device = bh->visit (bh, sysfs_path, device);
					if (hal_device != NULL)
						break;
				}
			}
			sysfs_close_device (device);
		}
	}

	return hal_device;
}

static void
rem_device_callouts_finished (HalDevice *d, gpointer user_data)
{
	HAL_INFO (("in remove_callouts_finished for udi=%s", d->udi));
	hal_device_store_remove (hald_get_gdl (), d);
}

static HalDevice *
rem_device (const char *sysfs_path, const char *subsystem, struct hald_helper_msg *msg) /* msg may be NULL */
{
	int i;
	int len1;
	int len2;
	char buf1[SYSFS_PATH_MAX];
	char buf2[SYSFS_PATH_MAX];
	HalDevice *hal_device = NULL;

	/* @todo TODO FIXME: invoked removed() method on d */

	hal_device = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);

	len1 = snprintf (buf1, SYSFS_PATH_MAX, "%s/block", sysfs_mount_path);
	len2 = snprintf (buf2, SYSFS_PATH_MAX, "%s/class", sysfs_mount_path);
	if (strncmp (sysfs_path, buf1, len1) == 0 || strncmp (sysfs_path, buf2, len2) == 0) {
		if (hal_device == NULL) {
		        /* What we need to do here is to unmerge the device from the
			 * sysdevice it belongs to. Ughh.. It's only a big deal when
			 * loading/unloading drivers and this should never happen
			 * on a desktop anyway?
			 *
			 * @todo FIXME
			 */
			HAL_WARNING (("Removal of class device at sysfs path %s is not yet implemented", sysfs_path));
			goto out;
		} else {
			dbus_bool_t really_remove = TRUE;

			for (i=0; class_device_handlers[i] != NULL; i++) {
				ClassDeviceHandler *ch = class_device_handlers[i];
				/** @todo TODO FIXME: just use ->accept() once we get rid of libsysfs */
				if (strcmp (ch->hal_class_name, subsystem) == 0) {
					really_remove = ch->removed (ch, sysfs_path, hal_device);
				}

			}

			if (really_remove) {
				g_signal_connect (hal_device, "callouts_finished",
						  G_CALLBACK (rem_device_callouts_finished), NULL);
				HAL_INFO (("in remove_device for udi=%s", hal_device->udi));
				hal_callout_device (hal_device, FALSE);
			} else {
				hal_device = NULL;
			}
			goto out;
		}
	} else {
		if (hal_device == NULL) {
			HAL_WARNING (("Couldn't remove device at sysfs path %s. No device found.", sysfs_path));
			goto out;
		}

		g_signal_connect (hal_device, "callouts_finished",
				  G_CALLBACK (rem_device_callouts_finished), NULL);
		HAL_INFO (("in remove_device for udi=%s", hal_device->udi));
		hal_callout_device (hal_device, FALSE);
	}

out:
	return hal_device;
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
handle_udev_node_created_found_device (HalDevice * d, void *data1, void *data2)
{
	int i;
	const char *sysfs_class_name;
	char *dev_file = (char *) data1;

	if (d != NULL) {
		HAL_INFO (("dev_file=%s is for udi=%s", dev_file, d->udi));

		/*hal_device_print (d);*/

		sysfs_class_name = hal_device_property_get_string (d, ".udev.class_name");

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

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/*****************************************************************************************************/


static void process_shutdown_list (GSList *coldplug_list);

static void
process_shutdown_list_callouts_done_for_device (HalDevice *device, gpointer user_data)
{
	GSList *shutdown_list = user_data;

	g_signal_handlers_disconnect_by_func (device, process_shutdown_list_callouts_done_for_device, user_data);

	process_shutdown_list (shutdown_list);
}

static void
process_shutdown_list (GSList *shutdown_list)
{


	if (shutdown_list != NULL) {
		HalDevice *device;

		device = (HalDevice *) shutdown_list->data;

		shutdown_list = g_slist_delete_link (shutdown_list, shutdown_list);

		HAL_INFO (("handling %s", device->udi));

		g_signal_connect (device, "callouts_finished",
				  G_CALLBACK (process_shutdown_list_callouts_done_for_device), shutdown_list);
		hal_callout_device (device, FALSE);

	} else {
		/* Inform the generic part of hald that we are done with probing */
		osspec_shutdown_done ();
	}
}


static void 
shutdown_add_recursively_to_list (GSList **shutdown_list, HalDevice *device)
{
	GSList *i;
	GSList *devices;
	
	/* add children before ourselves */
	devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), "info.parent", device->udi);
	for (i = devices; i != NULL; i = i->next) {
		HalDevice *child = (HalDevice *) i->data;
		shutdown_add_recursively_to_list (shutdown_list, child);
	}

	*shutdown_list = g_slist_append (*shutdown_list, device);

	g_slist_free (devices);
}

/* This function is documented in ../osspec.h */
void
osspec_shutdown ()
{
	GSList *shutdown_list = NULL;
	HalDevice *computer;
	HAL_INFO (("entering"));

	/* disabled hotplug processing */
	hotplug_sem_up ();

	/* build list of UDI's we want to shutdown ... */       
	computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	shutdown_add_recursively_to_list (&shutdown_list, computer);

	/* ... and then process sequentially */  
	process_shutdown_list (shutdown_list);
}

static void reenable_hotplug_proc_on_device_cancel (HalDevice *d, gpointer user_data);

static void
reenable_hotplug_on_gdl_store_add (HalDeviceStore *store, HalDevice *device, gboolean is_added, gpointer user_data)
{
	HalDevice *waiting_device = (HalDevice *) user_data;

	if (waiting_device == device && is_added) {
		/* unregister signal handlers */
		g_signal_handlers_disconnect_by_func (store, reenable_hotplug_on_gdl_store_add, user_data);
		g_signal_handlers_disconnect_by_func (device, reenable_hotplug_proc_on_device_cancel, user_data);

		/* continue processing */
		hotplug_sem_down ();
	}
}

static void
reenable_hotplug_proc_on_device_cancel (HalDevice *d, gpointer user_data)
{
	/* unregister signal handlers */
	g_signal_handlers_disconnect_by_func (hald_get_gdl (), reenable_hotplug_on_gdl_store_add, user_data);
        g_signal_handlers_disconnect_by_func (d, reenable_hotplug_proc_on_device_cancel, user_data);

	/* continue processing */
        hotplug_sem_down ();
}

static void
reenable_hotplug_on_gdl_store_remove (HalDeviceStore *store, HalDevice *device, gboolean is_added, gpointer user_data)
{
	HalDevice *waiting_device = (HalDevice *) user_data;

	if (waiting_device == device && !is_added) {
		/* unregister signal handlers */
		g_signal_handlers_disconnect_by_func (store, reenable_hotplug_on_gdl_store_remove, user_data);

		/* continue processing */
		hotplug_sem_down ();
	}
}


static void
hald_helper_hotplug (gchar *action, guint64 seqnum, gchar *subsystem, gchar *sysfs_path, struct hald_helper_msg *msg)
{
	HalDevice *d = NULL;
	char sysfs_path_full[SYSFS_PATH_MAX];

	snprintf (sysfs_path_full, SYSFS_PATH_MAX, "%s%s", sysfs_mount_path, sysfs_path);

	HAL_INFO (("action=%s seqnum=%llu subsystem=%s sysfs_path=%s",
		   action, seqnum, subsystem, sysfs_path_full));

	if (strcmp(action, "add") == 0)  {
		d = add_device (sysfs_path_full, subsystem, msg);

		/* if device is not already added, disable hotplug processing 
		 * and enable it again when the device has processed all the
		 * callouts
		 */
		if (d != NULL && hal_device_store_find(hald_get_gdl (), d->udi) == NULL) {
			
			/* Disable hotplug processing for now */
			hotplug_sem_up ();

			/* wait until we are in GDL */
			g_signal_connect (hald_get_gdl (), "store_changed", 
					  G_CALLBACK (reenable_hotplug_on_gdl_store_add), d);

			/* or until we are cancelled */
			g_signal_connect (d, "cancelled", G_CALLBACK (reenable_hotplug_proc_on_device_cancel), d);

		} else {
			if (d != NULL) {
				HAL_ERROR (("d = 0x%08x", d->udi));
			} else {
				HAL_ERROR (("d is NULL!"));
			}
		}
	} else if (strcmp(action, "remove") == 0){
		d = rem_device (sysfs_path_full, subsystem, msg);

		/* if device is not already removed, disable hotplug processing 
		 * and enable it again when the device has processed all the
		 * callouts
		 */
		if (d != NULL && hal_device_store_find(hald_get_gdl (), d->udi) != NULL) {
			
			/* Disable hotplug processing for now */
			hotplug_sem_up ();

			/* wait until we are out of the GDL */
			g_signal_connect (hald_get_gdl (), "store_changed", 
					  G_CALLBACK (reenable_hotplug_on_gdl_store_remove), d);
		}
	}

	g_free (subsystem);
	g_free (sysfs_path);
}

static void
hald_helper_device_name (gchar *action, guint64 seqnum, gchar *subsystem, 
			 gchar *sysfs_path, gchar *device_name, struct hald_helper_msg *msg)
{
	char sysfs_path_full[SYSFS_PATH_MAX];

	snprintf (sysfs_path_full, SYSFS_PATH_MAX, "%s%s", sysfs_mount_path, sysfs_path);

	HAL_INFO (("action=%s, seqnum=%llu  subsystem=%s devpath=%s devname=%s",
		   action, seqnum, subsystem, sysfs_path, device_name));

	if (strcmp(action, "add") == 0) {

		/* When udev gives the SEQNUM this can be synchronous. */
		hal_device_store_match_key_value_string_async (
			hald_get_tdl (),
			".udev.sysfs_path",
			sysfs_path_full,
			udev_node_created_cb, 
			g_strdup (device_name), /* will be freed in udev_node_created_cb */
			HAL_LINUX_HOTPLUG_TIMEOUT);
	}

	g_free (subsystem);
	g_free (sysfs_path);
	g_free (device_name);
}


/** queue of hotplug events (struct hald_helper_msg pointers) */
static GList *hotplug_queue = NULL;

/** queue of hotplug events received when sleeping on the first hotplug event (struct hald_helper_msg pointers) */
static GList *hotplug_queue_first = NULL;

/** Last hotplug sequence number */
static guint64 last_hotplug_seqnum = 0;

/** Timestamp of last hotplug */
static time_t last_hotplug_time_stamp = 0;

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

	/* Empty the list of events received while sleeping on the first hotplug event (this list is sorted) */
	if (hotplug_queue_first != NULL) {
		msg = (struct hald_helper_msg *) hotplug_queue_first->data;
		HAL_INFO (("Processing event around first hotplug with SEQNUM=%llu", msg->seqnum));
		hald_helper_hotplug (msg->action, msg->seqnum, g_strdup (msg->subsystem), 
				     g_strdup (msg->sysfs_path), msg);
		g_free (msg);
		hotplug_queue_first = g_list_delete_link (hotplug_queue_first, hotplug_queue_first);
		goto trynext;
	}

	for (i = hotplug_queue; i != NULL; i = g_list_next (i)) {
		msg = (struct hald_helper_msg *) i->data;

		/* check for dupes (user may have several hal.hotplug helpers (!) */
		if (msg->seqnum == last_hotplug_seqnum) {
			HAL_WARNING (("******************************************"));
			HAL_WARNING (("Ignoring duplicate event with SEQNUM=%d", msg->seqnum));
			HAL_WARNING (("******************************************"));
			g_free (msg);
			hotplug_queue = g_list_delete_link (hotplug_queue, i);
			goto trynext;
		}


		if (msg->seqnum == last_hotplug_seqnum + 1) {
			/* yup, found it */
			last_hotplug_seqnum = msg->seqnum;
			last_hotplug_time_stamp = msg->time_stamp;
			hald_helper_hotplug (msg->action, msg->seqnum, g_strdup (msg->subsystem), 
					     g_strdup (msg->sysfs_path), msg);
			g_free (msg);
			hotplug_queue = g_list_delete_link (hotplug_queue, i);
			goto trynext;
		}
	}
}

/** Check the queue and do timeout handling on missing hotplug events */
static void 
hotplug_timeout_handler (void)
{
	time_t now;

	now = time (NULL);

	/* See if there was a last hotplug event */
	if (last_hotplug_time_stamp > 0) {
		/* See if it's too long ago we processed */
		if (now - last_hotplug_time_stamp > HOTPLUG_TIMEOUT) {
			/* And if anything is actually waiting to be processed */
			if (hotplug_queue != NULL) {
				/* also log this to syslog */
				syslog (LOG_ERR, "Timed out waiting for hotplug event %lld", last_hotplug_seqnum + 1);
				HAL_ERROR (("Timed out waiting for hotplug event %lld", last_hotplug_seqnum + 1));
				
				/* Go to next seqnum and try again */
				last_hotplug_seqnum++;
				hald_helper_hotplug_process_queue ();
			}
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
	HAL_INFO (("******************************************"));
	HAL_INFO (("**** hotplug_counter is now %d", hotplug_counter));
	HAL_INFO (("******************************************"));
}

/** Decrement the hotplug semaphore. 
 *
 */
static void 
hotplug_sem_down (void)
{
	--hotplug_counter;

	HAL_INFO (("=========================================="));
	HAL_INFO (("==== hotplug_counter is now %d", hotplug_counter));
	HAL_INFO (("=========================================="));

	if (hotplug_counter < 0) {
		HAL_ERROR (("****************************************"));
		HAL_ERROR (("****************************************"));
		HAL_ERROR (("DANGER WILL ROBINSON! hotplug semaphore<0!"));
		HAL_ERROR (("****************************************"));
		HAL_ERROR (("****************************************"));
		hotplug_counter = 0;
	}

	/* Process remaining hotplug events */
	if (hotplug_counter == 0)
		hald_helper_hotplug_process_queue ();
}

/** This variable is TRUE exactly when we are sleeping on the first hotplug event */
static gboolean hotplug_sleep_first_event = FALSE;

static gboolean
hald_helper_first_hotplug_event (gpointer data)
{
	GList *i;
	struct hald_helper_msg *msg;

	HAL_INFO (("Slept %dms, now processing events", FIRST_HOTPLUG_SLEEP));

	/* First process the queue of events receieved while sleeping on the first hotplug events
	 * in order to find the last seqnum
	 *
	 * This list is sorted.
	 */
	for (i = hotplug_queue_first; i != NULL; i = g_list_next (i)) {
		msg = (struct hald_helper_msg *) i->data;
		HAL_INFO (("*** msg->seqnum = %lld", msg->seqnum));
		last_hotplug_seqnum = msg->seqnum;
		last_hotplug_time_stamp = msg->time_stamp;
	}

	/* Done sleeping on the first hotplug event */
	hotplug_sleep_first_event = FALSE;

	HAL_INFO (("Starting with last_hotplug_seqnum=%llu", last_hotplug_seqnum));

	hotplug_sem_down ();

	/* no further timer event */
	return FALSE;
}

static gint
hald_helper_msg_compare (gconstpointer pa, gconstpointer pb)
{
	const struct hald_helper_msg *a = (const struct hald_helper_msg *) pa;
	const struct hald_helper_msg *b = (const struct hald_helper_msg *) pb;

	return (gint) (a->seqnum - b->seqnum);
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

	HAL_INFO (("SEQNUM=%lld, TIMESTAMP=%d", msg.seqnum, msg.time_stamp));

	/* For DEBUG: Drop every 16th hotplug event */
#if 0
	if ((msg.seqnum&0x0f) == 0x00) {
		HAL_INFO (("=========================================="));
		HAL_INFO (("=========================================="));
		HAL_INFO (("NOTE NOTE: For debugging, deliberately ignoring hotplug event with SEQNUM=%d", msg.seqnum));
		HAL_INFO (("=========================================="));
		HAL_INFO (("=========================================="));
		goto out;
	}
#endif

	switch (msg.type) {
	case HALD_DEVD:
		hald_helper_device_name (msg.action, msg.seqnum, g_strdup (msg.subsystem),
					 g_strdup (msg.sysfs_path), g_strdup (msg.device_name), &msg);
		break;
	case HALD_HOTPLUG:
		/* need to process hotplug events in proper sequence */

		/*HAL_INFO (("Before reordering, SEQNUM=%d, last_hotplug_seqnum=%llu, subsystem=%s, sysfs=%s",
		  msg.seqnum, last_hotplug_seqnum, msg.subsystem, msg.sysfs_path));*/

		if (last_hotplug_seqnum == 0 ) {
			/* gotta start somewhere; however sleep some time to allow
			 * some more hotplug events to propagate so we know where
			 * we're at.
			 *
			 * @todo TODO: read SEQNUM from sysfs
			 */

			HAL_INFO (("First SEQNUM=%llu; sleeping %dms to get a few more events", 
				   msg.seqnum, FIRST_HOTPLUG_SLEEP));

			hotplug_sem_up ();
			g_timeout_add (FIRST_HOTPLUG_SLEEP, hald_helper_first_hotplug_event, NULL);
			hotplug_sleep_first_event = TRUE;

			hotplug_queue_first = g_list_insert_sorted (hotplug_queue_first, 
								    g_memdup (&msg, sizeof (struct hald_helper_msg)),
								    hald_helper_msg_compare);

			/* so we only setup one timer */
			last_hotplug_seqnum = msg.seqnum;
			last_hotplug_time_stamp = msg.time_stamp;

			goto out;
		}

		/* If sleeping on the first hotplug event queue up the events in a special queue */
		if (hotplug_sleep_first_event) {
			HAL_INFO (("first hotplug sleep; got SEQNUM=%d", msg.seqnum));
			hotplug_queue_first = g_list_insert_sorted (hotplug_queue_first, 
								    g_memdup (&msg, sizeof (struct hald_helper_msg)),
								    hald_helper_msg_compare);
		} else {

			if (msg.seqnum < last_hotplug_seqnum) {
				/* yikes, this means were started during a hotplug */
				HAL_WARNING (("Got SEQNUM=%llu, but last_hotplug_seqnum=%llu", 
					      msg.seqnum, last_hotplug_seqnum));
                                /* have to process immediately other we may deadlock due to the hotplug semaphore */
				hald_helper_hotplug (msg.action, msg.seqnum, g_strdup (msg.subsystem), 	 
						     g_strdup (msg.sysfs_path), &msg); 	 
				/* still process the queue though */
				hald_helper_hotplug_process_queue ();

			} else {
				/* Queue up this hotplug event and process the queue */
				HAL_INFO (("Queing up seqnum=%llu, sysfspath=%s, subsys=%s", 
					   msg.seqnum, msg.sysfs_path, msg.subsystem));
				hotplug_queue = g_list_append (hotplug_queue, 
							       g_memdup (&msg, sizeof (struct hald_helper_msg)));
				hald_helper_hotplug_process_queue ();
			}
		}
		break;
	}
out:
	return TRUE;
}
