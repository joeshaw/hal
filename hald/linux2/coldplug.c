/***************************************************************************
 * CVSID: $Id$
 *
 * coldplug.c : Synthesize hotplug events when starting up
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
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../device_info.h"
#include "../util.h"

#include "osspec_linux.h"

#include "coldplug.h"
#include "hotplug.h"

#define DMPREFIX "dm-"

static gboolean
coldplug_synthesize_block_event(const gchar *f);

static void
coldplug_compute_visit_device (const gchar *path, 
			       GHashTable *sysfs_to_bus_map, 
			       GHashTable *sysfs_to_class_in_devices_map);

/* For debugging */
/*#define HAL_COLDPLUG_VERBOSE*/

static void
free_hash_sys_to_class_in_dev (gpointer key, gpointer value, gpointer user_data)
{
	GSList *i;
	GSList *list = (GSList *) value;

	for (i = list; i != NULL; i = g_slist_next (i))
		g_free (i->data);
	g_slist_free (list);
}

/** This function serves one major purpose : build an ordered list of
 *  pairs (sysfs path, subsystem) to process when starting up:
 *  coldplugging. The ordering is arranged such that all bus-devices
 *  are visited in the same order as performing a traversal through
 *  the tree; e.g. bus-device A is not processed before bus-device B
 *  if B is a parent of A connection-wise.
 *
 *  After all bus-devices are added to the list, then all block devices are
 *  processed in the order they appear.
 *
 *  Finally, all class devices are added to the list.
 *
 *  @return                     Ordered list of sysfs paths or NULL 
 *                              if there was an error
 */
gboolean 
coldplug_synthesize_events (void)
{
	GDir *dir;
	GError *err = NULL;
	gchar path[HAL_PATH_MAX];
	gchar path1[HAL_PATH_MAX];
	gchar path2[HAL_PATH_MAX];
	const gchar *f;
	const gchar *f1;
	const gchar *f2;
	GSList *li;

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

        /** Mapping from sysfs path in /sys/devices to the pairs (sysfs class path, classname)
	 *  for class devices; note that more than one class device might map to a physical device
	 *
	 * Example:
	 *
	 * /sys/devices/pci0000:00/0000:00:07.2/usb1/1-1/1-1:1.0/host7  -> (/sys/class/scsi_host/host7, scsi_host)
	 * /sys/devices/platform/i8042/serio0/serio2 -> (/sys/class/input/event2, input, /sys/class/input/mouse1, input)
	 */
	GHashTable *sysfs_to_class_in_devices_map = NULL;

	/* Class devices without device links; string list; example
	 *
	 * (/sys/class/input/mice, mouse, /sys/class/mem/null, mem, ...)
	 */
	GSList *sysfs_other_class_dev = NULL;

	/* Device mapper devices that should be added after all other block devices
	 *
	 * Example:
	 *
	 * (/sys/block/dm-0)
	 */
	GSList *sysfs_dm_dev = NULL;

	/* build bus map */
	sysfs_to_bus_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	g_snprintf (path, HAL_PATH_MAX, "%s/bus", get_hal_sysfs_path ());
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/bus: %s", get_hal_sysfs_path (), err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		GDir *dir1;

		g_snprintf (path, HAL_PATH_MAX, "%s/bus/%s", get_hal_sysfs_path (), f);
		if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %/bus/%s: %s", get_hal_sysfs_path (), f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {

			if (strcmp (f1, "devices") == 0) {
				GDir *dir2;

				g_snprintf (path, HAL_PATH_MAX, "%s/bus/%s/%s", 
					    get_hal_sysfs_path (), f, f1);
				if ((dir2 = g_dir_open (path, 0, &err)) == NULL) {
					HAL_ERROR (("Unable to open %s/bus/%s/%s: %s", 
						    get_hal_sysfs_path (), f, f1, err->message));
					g_error_free (err);
					goto error;
				}
				while ((f2 = g_dir_read_name (dir2)) != NULL) {
					gchar *target;
					gchar *normalized_target;
					g_snprintf (path, HAL_PATH_MAX, "%s/bus/%s/%s/%s", 
						    get_hal_sysfs_path (), f, f1, f2);
					if ((target = g_file_read_link (path, &err)) == NULL) {
						HAL_ERROR (("%s/bus/%s/%s/%s is not a symlink: %s!", 
							    get_hal_sysfs_path (), 
							    f, f1, f2, err->message));
						g_error_free (err);
						goto error;
					}

					g_snprintf (path, HAL_PATH_MAX, "%s/bus/%s/%s", get_hal_sysfs_path (), f, f1);
					normalized_target = hal_util_get_normalized_path (path, target);
					g_free (target);

					g_hash_table_insert (sysfs_to_bus_map, normalized_target, g_strdup(f));

				}
				g_dir_close (dir2);
			}
		}
		g_dir_close (dir1);
	}
	g_dir_close (dir);

	/* build class map and class device map (values are free in separate foreach()) */
	sysfs_to_class_in_devices_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	g_snprintf (path, HAL_PATH_MAX, "%s/class" , get_hal_sysfs_path ());
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/class: %s", get_hal_sysfs_path (), err->message));
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		GDir *dir1;

		g_snprintf (path, HAL_PATH_MAX, "%s/class/%s" , get_hal_sysfs_path (), f);
		if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %/class/%s: %s", get_hal_sysfs_path (), f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {
			gchar *target;
			gchar *normalized_target;

			g_snprintf (path1, HAL_PATH_MAX, "%s/class/%s/%s/device", get_hal_sysfs_path (), f, f1);
			/* Accept net devices without device links too, they may be coldplugged PCMCIA devices */
			if (((target = g_file_read_link (path1, NULL)) == NULL)) {
				/* no device link */
				g_snprintf (path1, HAL_PATH_MAX, "%s/class/%s/%s", get_hal_sysfs_path (), f, f1);
				sysfs_other_class_dev = g_slist_append (sysfs_other_class_dev, g_strdup (path1));
				sysfs_other_class_dev = g_slist_append (sysfs_other_class_dev, g_strdup (f));
			} else {
				GSList *classdev_strings;

				g_snprintf (path2, HAL_PATH_MAX, "%s/class/%s/%s", get_hal_sysfs_path (), f, f1);
				normalized_target = hal_util_get_normalized_path (path2, target);
				g_free (target);

				classdev_strings = g_hash_table_lookup (sysfs_to_class_in_devices_map,
									normalized_target);

				classdev_strings = g_slist_append (classdev_strings, g_strdup (path2));
				classdev_strings = g_slist_append (classdev_strings, g_strdup (f));
				g_hash_table_replace (sysfs_to_class_in_devices_map,
						      normalized_target, classdev_strings);
			}				
		}
		g_dir_close (dir1);
	}
	g_dir_close (dir);

	/* Now traverse /sys/devices and consult the map we've just
	 * built; this includes adding a) bus devices; and b) class
	 * devices that sit in /sys/devices */
	g_snprintf (path, HAL_PATH_MAX, "%s/devices", get_hal_sysfs_path ());
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %/devices: %s", get_hal_sysfs_path (), err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		GDir *dir1;

		g_snprintf (path, HAL_PATH_MAX, "%s/devices/%s", get_hal_sysfs_path (), f);
		if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
			HAL_ERROR (("Unable to open %/devices/%s: %s", get_hal_sysfs_path (), f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {

			g_snprintf (path, HAL_PATH_MAX, "%s/devices/%s/%s", get_hal_sysfs_path (), f, f1);
			coldplug_compute_visit_device (path, sysfs_to_bus_map, sysfs_to_class_in_devices_map);
		}
		g_dir_close (dir1);
	}
	g_dir_close (dir);

	g_hash_table_destroy (sysfs_to_bus_map);
	/* free keys and values in this complex hash */
	g_hash_table_foreach (sysfs_to_class_in_devices_map, free_hash_sys_to_class_in_dev, NULL);
	g_hash_table_destroy (sysfs_to_class_in_devices_map);

	/* we are guaranteed, per construction, that the len of this list is even */
	for (li = sysfs_other_class_dev; li != NULL; li = g_slist_next (g_slist_next (li))) {
		gchar *sysfs_path;
		gchar *subsystem;
		HotplugEvent *hotplug_event;

		sysfs_path = (gchar *) li->data;
		subsystem = (gchar *) li->next->data;

#ifdef HAL_COLDPLUG_VERBOSE
		printf ("class: %s (%s) (no device link)\n", sysfs_path, subsystem);
#endif
		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = TRUE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
		hal_util_get_device_file (sysfs_path, hotplug_event->sysfs.device_file, sizeof (hotplug_event->sysfs.device_file));
		hotplug_event->sysfs.net_ifindex = -1;
		
		hotplug_event_enqueue (hotplug_event);

		g_free (li->data);
		g_free (li->next->data);
	}
	g_slist_free (sysfs_other_class_dev);

	/* add block devices */
	g_snprintf (path, HAL_PATH_MAX, "%s/block", get_hal_sysfs_path ());
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %s: %s", path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		if (g_str_has_prefix (f, DMPREFIX)) {
			/* defer dm devices */
			sysfs_dm_dev = g_slist_append(sysfs_dm_dev, g_strdup(f));
			continue;
		}
		if (coldplug_synthesize_block_event(f) == FALSE)
			goto error;
	}
	/* process all dm devices last so that their backing devices exist */
	for (li = sysfs_dm_dev; li != NULL; li = g_slist_next (g_slist_next (li))) {
		if (coldplug_synthesize_block_event(li->data) == FALSE)
			goto error;
		g_free (li->data);
	}
	g_slist_free (sysfs_dm_dev);
	g_dir_close (dir);
       
	return TRUE;
error:
	HAL_ERROR (("Error building the orderered list of sysfs paths"));
	return FALSE;
}

static gboolean
coldplug_synthesize_block_event(const gchar *f)
{
	GDir *dir1;
	gsize flen;
	HotplugEvent *hotplug_event;
	gchar *target;
	gchar *normalized_target;
	GError *err = NULL;
	gchar path[HAL_PATH_MAX];
	gchar path1[HAL_PATH_MAX];
	const gchar *f1;

	g_snprintf (path, HAL_PATH_MAX, "%s/block/%s", get_hal_sysfs_path (), f);
#ifdef HAL_COLDPLUG_VERBOSE
	printf ("block: %s (block)\n",  path);
#endif

	g_snprintf (path1, HAL_PATH_MAX, "%s/block/%s/device", get_hal_sysfs_path (), f);
	if (((target = g_file_read_link (path1, NULL)) != NULL)) {
		normalized_target = hal_util_get_normalized_path (path1, target);
		g_free (target);
	} else {
		normalized_target = NULL;
	}

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->is_add = TRUE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, path, sizeof (hotplug_event->sysfs.sysfs_path));
	hal_util_get_device_file (path, hotplug_event->sysfs.device_file, sizeof (hotplug_event->sysfs.device_file));
	if (normalized_target != NULL)
		g_strlcpy (hotplug_event->sysfs.wait_for_sysfs_path, normalized_target, sizeof (hotplug_event->sysfs.wait_for_sysfs_path));
	else
		hotplug_event->sysfs.wait_for_sysfs_path[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;
	hotplug_event_enqueue (hotplug_event);
	g_free (normalized_target);

	flen = strlen (f);

	if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %s: %s", path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f1 = g_dir_read_name (dir1)) != NULL) {
		if (strncmp (f, f1, flen) == 0) {
			g_snprintf (path1, HAL_PATH_MAX, "%s/%s", path, f1);
#ifdef HAL_COLDPLUG_VERBOSE
			printf ("block: %s (block)\n", path1);
#endif

			hotplug_event = g_new0 (HotplugEvent, 1);
			hotplug_event->is_add = TRUE;
			hotplug_event->type = HOTPLUG_EVENT_SYSFS;
			g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
			g_strlcpy (hotplug_event->sysfs.sysfs_path, path1, sizeof (hotplug_event->sysfs.sysfs_path));
			g_strlcpy (hotplug_event->sysfs.wait_for_sysfs_path, path, sizeof (hotplug_event->sysfs.wait_for_sysfs_path));
			hal_util_get_device_file (path1, hotplug_event->sysfs.device_file, sizeof (hotplug_event->sysfs.device_file));
			hotplug_event->sysfs.net_ifindex = -1;
			hotplug_event_enqueue (hotplug_event);
		}
	}
	g_dir_close (dir1);		
       
	return TRUE;
error:
	return FALSE;
}


static void
coldplug_compute_visit_device (const gchar *path, 
			       GHashTable *sysfs_to_bus_map, 
			       GHashTable *sysfs_to_class_in_devices_map)
{
	gchar *bus;
	GError *err = NULL;
	GDir *dir;
	const gchar *f;
	/*HStringPair *pair;*/
	GSList *class_devs;
	GSList *i;

	bus = g_hash_table_lookup (sysfs_to_bus_map, path);
	if (bus != NULL) {
		HotplugEvent *hotplug_event;
		gchar *parent_sysfs_path;

#ifdef HAL_COLDPLUG_VERBOSE
		printf ("bus:   %s (%s)\n", path, bus);
#endif

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = TRUE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, bus, sizeof (hotplug_event->sysfs.subsystem));
		g_strlcpy (hotplug_event->sysfs.sysfs_path, path, sizeof (hotplug_event->sysfs.sysfs_path));
		hotplug_event->sysfs.net_ifindex = -1;

		parent_sysfs_path = hal_util_get_parent_path (path);
		g_strlcpy (hotplug_event->sysfs.wait_for_sysfs_path, parent_sysfs_path, sizeof (hotplug_event->sysfs.wait_for_sysfs_path));
		g_free (parent_sysfs_path);

		hotplug_event->sysfs.device_file[0] = '\0';
		hotplug_event_enqueue (hotplug_event);
	}

	/* we are guaranteed, per construction, that the len of this list is even */
	class_devs = g_hash_table_lookup (sysfs_to_class_in_devices_map, path);
	for (i = class_devs; i != NULL; i = g_slist_next (g_slist_next (i))) {
		gchar *sysfs_path;
		gchar *subsystem;
		HotplugEvent *hotplug_event;

		sysfs_path = (gchar *) i->data;
		subsystem = (gchar *) i->next->data;

#ifdef HAL_COLDPLUG_VERBOSE
		printf ("class: %s (%s) (%s)\n", path, subsystem, sysfs_path);
#endif
		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = TRUE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
		hal_util_get_device_file (sysfs_path, hotplug_event->sysfs.device_file, sizeof (hotplug_event->sysfs.device_file));
		if (path != NULL)
			g_strlcpy (hotplug_event->sysfs.wait_for_sysfs_path, path, sizeof (hotplug_event->sysfs.wait_for_sysfs_path));
		else
			hotplug_event->sysfs.wait_for_sysfs_path[0] = '\0';
		hotplug_event->sysfs.net_ifindex = -1;
		hotplug_event_enqueue (hotplug_event);
	}

	/* visit children; dont follow symlinks though.. */
	err = NULL;
	if ((dir = g_dir_open (path, 0, &err)) == NULL) {
		/*HAL_ERROR (("Unable to open directory: %s", path, err->message));*/
		g_error_free (err);
		goto error;
	}
	while ((f = g_dir_read_name (dir)) != NULL) {
		gchar path_child[HAL_PATH_MAX];
		struct stat statbuf;
	
		g_snprintf (path_child, HAL_PATH_MAX, "%s/%s", path, f);

		if (lstat (path_child, &statbuf) == 0) {

			if (!S_ISLNK (statbuf.st_mode)) {
				/* recursion fun */
				coldplug_compute_visit_device (path_child, 
							       sysfs_to_bus_map, 
							       sysfs_to_class_in_devices_map);
			}
		}
	}
	g_dir_close (dir);

error:
	return;
}

