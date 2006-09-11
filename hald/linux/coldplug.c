/***************************************************************************
 * CVSID: $Id$
 *
 * coldplug.c : Synthesize hotplug events when starting up
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../hald.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"

#include "osspec_linux.h"
#include "hotplug.h"

#include "coldplug.h"

#define DMPREFIX "dm-"

/* For debugging */
#define HAL_COLDPLUG_VERBOSE

static GHashTable *sysfs_to_udev_map;
static char dev_root[HAL_PATH_MAX];

/* Returns the path of the udevinfo program 
 *
 * @return                      Path or NULL if udevinfo program is not found
 */
static const gchar *
hal_util_get_udevinfo_path (void)
{
	guint i;
	struct stat s;
	static gchar *path = NULL;
	gchar *possible_paths[] = {
		"/usr/bin/udevinfo",
		"/bin/udevinfo",
		"/usr/sbin/udevinfo",
		"/sbin/udevinfo",
	};

	if (path != NULL)
		 return path;

	for (i = 0; i < sizeof (possible_paths) / sizeof (char *); i++) {
		if (stat (possible_paths[i], &s) == 0 && S_ISREG (s.st_mode)) {
			path = possible_paths[i];
			break;
		}
	}
	return path;
}

static gboolean
hal_util_init_sysfs_to_udev_map (void)
{
	char *udevdb_export_argv[] = { "/usr/bin/udevinfo", "-e", NULL };
	char *udevroot_argv[] = { "/usr/bin/udevinfo", "-r", NULL };
	char *udevinfo_stdout;
	int udevinfo_exitcode;
	HotplugEvent *hotplug_event = NULL;
	char *p;

	sysfs_to_udev_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	udevdb_export_argv[0] = (char *) hal_util_get_udevinfo_path ();
	udevroot_argv[0] = (char *) hal_util_get_udevinfo_path ();

	/* get udevroot */
	if (g_spawn_sync ("/", udevroot_argv, NULL, 0, NULL, NULL,
			  &udevinfo_stdout,
			  NULL,
			  &udevinfo_exitcode,
			  NULL) != TRUE) {
		HAL_ERROR (("Couldn't invoke %s", udevroot_argv[0]));
		goto error;
	}
	if (udevinfo_exitcode != 0) {
		HAL_ERROR (("%s returned %d", udevroot_argv[0], udevinfo_exitcode));
		goto error;
	}

	g_strlcpy(dev_root, udevinfo_stdout, sizeof(dev_root));
	p = strchr(dev_root, '\n');
	if (p != NULL)
		p[0] = '\0';
	g_free(udevinfo_stdout);
	HAL_INFO (("dev_root is %s", dev_root));

	/* get udevdb export */
	if (g_spawn_sync ("/", udevdb_export_argv, NULL, 0, NULL, NULL,
			  &udevinfo_stdout,
			  NULL,
			  &udevinfo_exitcode,
			  NULL) != TRUE) {
		HAL_ERROR (("Couldn't invoke %s", udevdb_export_argv[0]));
		g_free(udevinfo_stdout);
		goto error;
	}

	if (udevinfo_exitcode != 0) {
		HAL_ERROR (("%s returned %d", udevdb_export_argv[0], udevinfo_exitcode));
		goto error;
	}

	/* read the export of the udev database */
	p = udevinfo_stdout;
	while (p[0] != '\0') {
		char *line, *end;
		gchar *str;

		/* get line, terminate and move to next line */
		line = p;
		end = strchr(line, '\n');
		if (end == NULL)
			break;
		end[0] = '\0';
		p = &end[1];

		/* insert device */
		if (line[0] == '\0') {
			if (hotplug_event != NULL) {
				g_hash_table_insert (sysfs_to_udev_map, g_strdup (hotplug_event->sysfs.sysfs_path), hotplug_event);
#ifdef HAL_COLDPLUG_VERBOSE
				printf ("Got '%s' -> '%s'\n", hotplug_event->sysfs.sysfs_path, hotplug_event->sysfs.device_file);
#endif
				hotplug_event = NULL;
			}
			continue;
		}

		/* new device */
		if (strncmp(line, "P: ", 3) == 0) {
			hotplug_event = g_new0 (HotplugEvent, 1);
			g_strlcpy (hotplug_event->sysfs.sysfs_path, get_hal_sysfs_path (), sizeof(hotplug_event->sysfs.sysfs_path));
			g_strlcat (hotplug_event->sysfs.sysfs_path, &line[3], sizeof(hotplug_event->sysfs.sysfs_path));
			continue;
		}

		/* only valid if we have an actual device */
		if (hotplug_event == NULL)
			continue;

		if (strncmp(line, "N: ", 3) == 0) {
			g_snprintf (hotplug_event->sysfs.device_file, sizeof(hotplug_event->sysfs.device_file),
				    "%s/%s", dev_root, &line[3]);
		} else if (strncmp(line, "E: ID_VENDOR=", 13) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[13])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.vendor, str, sizeof(hotplug_event->sysfs.vendor));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_MODEL=", 12) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[12])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.model, str, sizeof(hotplug_event->sysfs.model));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_REVISION=", 15) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[15])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.revision, str, sizeof(hotplug_event->sysfs.revision));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_SERIAL=", 13) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[13])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.serial, str, sizeof(hotplug_event->sysfs.serial));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_FS_USAGE=", 15) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[15])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.fsusage, str, sizeof(hotplug_event->sysfs.fsusage));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_FS_TYPE=", 14) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[14])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.fstype, str, sizeof(hotplug_event->sysfs.fstype));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_FS_VERSION=", 17) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[17])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.fsversion, str, sizeof(hotplug_event->sysfs.fsversion));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_FS_UUID=", 14) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[14])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.fsuuid, str, sizeof(hotplug_event->sysfs.fsuuid));
				g_free (str);
			}
		} else if (strncmp(line, "E: ID_FS_LABEL=", 15) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&line[15])) != NULL) {
				g_strlcpy (hotplug_event->sysfs.fslabel, str, sizeof(hotplug_event->sysfs.fslabel));
				g_free (str);
			}
		}
	}

	g_free(udevinfo_stdout);
	return TRUE;

error:
	g_free(udevinfo_stdout);
	g_hash_table_destroy (sysfs_to_udev_map);
	return FALSE;
}

static HotplugEvent
*coldplug_get_hotplug_event(const gchar *sysfs_path, const gchar *subsystem)
{
	HotplugEvent *hotplug_event, *hotplug_event_udev;
	const char *pos;
	gchar path[HAL_PATH_MAX];
	struct stat statbuf;

	hotplug_event = g_new0 (HotplugEvent, 1);
	if (hotplug_event == NULL)
		return NULL;

	/* lookup if udev has something stored in its database */
	hotplug_event_udev = (HotplugEvent *) g_hash_table_lookup (sysfs_to_udev_map, sysfs_path);
	if (hotplug_event_udev != NULL) {
		memcpy(hotplug_event, hotplug_event_udev, sizeof(HotplugEvent));
		HAL_INFO (("found in udevdb '%s' '%s'", hotplug_event->sysfs.sysfs_path, hotplug_event->sysfs.device_file));
	} else {
		/* device is not in udev database */
		g_strlcpy(hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof(hotplug_event->sysfs.sysfs_path));

		/* look if a device node is expected */
		g_strlcpy(path, sysfs_path, sizeof(path));
		g_strlcat(path, "/dev", sizeof(path));
		if (stat (path, &statbuf) != 0)
			goto no_node;

		/* look if the node exists */
		pos = strrchr(sysfs_path, '/');
		if (pos == NULL)
			goto no_node;
		g_strlcpy(path, dev_root, sizeof(path));
		g_strlcat(path, pos, sizeof(path));
		if (stat (path, &statbuf) != 0)
			goto no_node;
		if (!S_ISBLK (statbuf.st_mode) && !S_ISCHR (statbuf.st_mode))
			goto no_node;

		HAL_INFO (("found device_file %s for sysfs_path %s", path, sysfs_path));
		g_strlcpy(hotplug_event->sysfs.device_file, path, sizeof(hotplug_event->sysfs.device_file));
	}

no_node:
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	hotplug_event->sysfs.net_ifindex = -1;
	return hotplug_event;
}

static gboolean
coldplug_synthesize_block_event(const gchar *f);

static void
coldplug_compute_visit_device (const gchar *path, 
			       GHashTable *sysfs_to_bus_map, 
			       GHashTable *sysfs_to_class_in_devices_map);

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

	if (hal_util_init_sysfs_to_udev_map () == FALSE) {
		HAL_ERROR (("Unable to get sysfs to dev map"));
		goto error;
	}

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
			HAL_ERROR (("Unable to open %s/class/%s: %s", get_hal_sysfs_path (), f, err->message));
			g_error_free (err);
			goto error;
		}
		while ((f1 = g_dir_read_name (dir1)) != NULL) {
			gchar *target;
			gchar *normalized_target;

			g_snprintf (path2, HAL_PATH_MAX, "%s/class/%s/%s", get_hal_sysfs_path (), f, f1);

			/* check if we find a symlink here pointing to a device _inside_ a class device,
			 * like "input" in 2.6.15. This kernel sysfs layout will change again in the future,
			 * for now resolve the link to the "real" device path, like real hotplug events
			 * devpath would have
			 */
			if ((target = g_file_read_link (path2, NULL)) != NULL) {
				char *pos = strrchr(path2, '/');

				if (pos)
					pos[0] = '\0';
				normalized_target = hal_util_get_normalized_path (path2, target);
				g_free (target);
				g_strlcpy(path2, normalized_target, sizeof(path2));
				g_free (normalized_target);
			}

			/* Accept net devices without device links too, they may be coldplugged PCMCIA devices */
			g_snprintf (path1, HAL_PATH_MAX, "%s/device", path2);
			if (((target = g_file_read_link (path1, NULL)) == NULL)) {
				/* no device link */
				sysfs_other_class_dev = g_slist_append (sysfs_other_class_dev, g_strdup (path2));
				sysfs_other_class_dev = g_slist_append (sysfs_other_class_dev, g_strdup (f));
			} else {
				GSList *classdev_strings;

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
		hotplug_event = coldplug_get_hotplug_event (sysfs_path, subsystem);
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
		if (coldplug_synthesize_block_event (li->data) == FALSE)
			goto error;
		g_free (li->data);
	}
	g_slist_free (sysfs_dm_dev);
	g_dir_close (dir);

	g_hash_table_destroy (sysfs_to_udev_map);

	return TRUE;
error:
	HAL_ERROR (("Error building the ordered list of sysfs paths"));
	return FALSE;
}

static gboolean
coldplug_synthesize_block_event(const gchar *f)
{
	GDir *dir1;
	HotplugEvent *hotplug_event;
	GError *err = NULL;
	gchar path[HAL_PATH_MAX];
	gchar path1[HAL_PATH_MAX];
	const gchar *f1;

	g_snprintf (path, HAL_PATH_MAX, "%s/block/%s", get_hal_sysfs_path (), f);
#ifdef HAL_COLDPLUG_VERBOSE
	printf ("block: %s (block)\n",  path);
#endif
	hotplug_event = coldplug_get_hotplug_event (path, "block");
	hotplug_event_enqueue (hotplug_event);

	if ((dir1 = g_dir_open (path, 0, &err)) == NULL) {
		HAL_ERROR (("Unable to open %s: %s", path, err->message));
		g_error_free (err);
		goto error;
	}
	while ((f1 = g_dir_read_name (dir1)) != NULL) {
		if (strncmp (f, f1, strlen (f)) == 0) {
			g_snprintf (path1, HAL_PATH_MAX, "%s/%s", path, f1);
#ifdef HAL_COLDPLUG_VERBOSE
			printf ("block: %s (block)\n", path1);
#endif
			hotplug_event = coldplug_get_hotplug_event (path1, "block");
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
#ifdef HAL_COLDPLUG_VERBOSE
		printf ("bus:   %s (%s)\n", path, bus);
#endif
		hotplug_event = coldplug_get_hotplug_event (path, bus);
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
		hotplug_event = coldplug_get_hotplug_event (sysfs_path, subsystem);
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

