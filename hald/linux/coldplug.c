
/*
 * Synthesize device events when starting up
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005-2006 Kay Sievers <kay.sievers@novell.com>
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
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
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

struct sysfs_device {
	char *path;
	char *subsystem;
	HotplugEventType type;
};

static GHashTable *sysfs_to_udev_map;
static GSList *device_list;
static char dev_root[HAL_PATH_MAX];

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
				HAL_INFO (("found (udevdb export) '%s' -> '%s'",
					   hotplug_event->sysfs.sysfs_path, hotplug_event->sysfs.device_file));
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
*coldplug_get_hotplug_event(const gchar *sysfs_path, const gchar *subsystem, HotplugEventType type)
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
		HAL_INFO (("new event (dev node from udev) '%s' '%s'", hotplug_event->sysfs.sysfs_path, hotplug_event->sysfs.device_file));
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

		HAL_INFO (("new event (dev node from kernel name) '%s' '%s'", sysfs_path, path));
		g_strlcpy(hotplug_event->sysfs.device_file, path, sizeof(hotplug_event->sysfs.device_file));
	}

no_node:
	if (hotplug_event->sysfs.device_file[0] == '\0')
		HAL_INFO (("new event (no dev node) '%s'", sysfs_path));
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = type;
	hotplug_event->sysfs.net_ifindex = -1;
	return hotplug_event;
}

static int device_list_insert(const char *path, const char *subsystem,
			      HotplugEventType type)
{
	char filename[HAL_PATH_MAX];
	struct stat statbuf;
	struct sysfs_device *sysfs_dev = NULL;

	/* we only have a device, if we have an uevent file */
	g_strlcpy(filename, path, sizeof(filename));
	g_strlcat(filename, "/uevent", sizeof(filename));
	if (stat(filename, &statbuf) < 0)
		goto error;
	if (!(statbuf.st_mode & S_IWUSR))
		goto error;

	sysfs_dev = g_new0 (struct sysfs_device, 1);
	if (sysfs_dev == NULL)
		goto error;

	/* resolve possible link to real target */
	if (lstat(path, &statbuf) < 0)
		goto error;
	if (S_ISLNK(statbuf.st_mode)) {
		gchar *target;

		if ((target = g_file_read_link (path, NULL)) != NULL) {
			gchar *normalized_target;

			g_strlcpy(filename, path, sizeof(filename));
			hal_util_path_ascend (filename);
			normalized_target = hal_util_get_normalized_path (filename, target);
			g_free (target);
			sysfs_dev->path = normalized_target;
			goto found;
		}
		goto error;
	}

	sysfs_dev->path = g_strdup (path);
found:
	sysfs_dev->subsystem = g_strdup (subsystem);
	device_list = g_slist_prepend (device_list, sysfs_dev);
	return 0;

error:
	g_free (sysfs_dev);
	return -1;
}

static void scan_bus(void)
{
	char base[HAL_PATH_MAX];
	DIR *dir;
	struct dirent *dent;

	g_strlcpy(base, get_hal_sysfs_path (), sizeof(base));
	g_strlcat(base, "/bus", sizeof(base));

	dir = opendir(base);
	if (dir != NULL) {
		for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
			char dirname[HAL_PATH_MAX];
			DIR *dir2;
			struct dirent *dent2;

			if (dent->d_name[0] == '.')
				continue;

			g_strlcpy(dirname, base, sizeof(dirname));
			g_strlcat(dirname, "/", sizeof(dirname));
			g_strlcat(dirname, dent->d_name, sizeof(dirname));
			g_strlcat(dirname, "/devices", sizeof(dirname));

			/* look for devices */
			dir2 = opendir(dirname);
			if (dir2 != NULL) {
				for (dent2 = readdir(dir2); dent2 != NULL; dent2 = readdir(dir2)) {
					char dirname2[HAL_PATH_MAX];

					if (dent2->d_name[0] == '.')
						continue;

					g_strlcpy(dirname2, dirname, sizeof(dirname2));
					g_strlcat(dirname2, "/", sizeof(dirname2));
					g_strlcat(dirname2, dent2->d_name, sizeof(dirname2));
					device_list_insert(dirname2, dent->d_name, HOTPLUG_EVENT_SYSFS_BUS);
				}
				closedir(dir2);
			}
		}
		closedir(dir);
	}
}

static void scan_block(void)
{
	char base[HAL_PATH_MAX];
	DIR *dir;
	struct dirent *dent;
	struct stat statbuf;

	/* skip if "block" is already a "class" */
	g_strlcpy(base, get_hal_sysfs_path (), sizeof(base));
	g_strlcat(base, "/class/block", sizeof(base));
	if (stat(base, &statbuf) == 0)
		return;

	g_strlcpy(base, get_hal_sysfs_path (), sizeof(base));
	g_strlcat(base, "/block", sizeof(base));

	dir = opendir(base);
	if (dir != NULL) {
		for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
			char dirname[HAL_PATH_MAX];
			DIR *dir2;
			struct dirent *dent2;

			if (dent->d_name[0] == '.')
				continue;

			g_strlcpy(dirname, base, sizeof(dirname));
			g_strlcat(dirname, "/", sizeof(dirname));
			g_strlcat(dirname, dent->d_name, sizeof(dirname));
			if (device_list_insert(dirname, "block", HOTPLUG_EVENT_SYSFS_BLOCK) != 0)
				continue;

			/* look for partitions */
			dir2 = opendir(dirname);
			if (dir2 != NULL) {
				for (dent2 = readdir(dir2); dent2 != NULL; dent2 = readdir(dir2)) {
					char dirname2[HAL_PATH_MAX];

					if (dent2->d_name[0] == '.')
						continue;

					if (!strcmp(dent2->d_name,"device"))
						continue;
					g_strlcpy(dirname2, dirname, sizeof(dirname2));
					g_strlcat(dirname2, "/", sizeof(dirname2));
					g_strlcat(dirname2, dent2->d_name, sizeof(dirname2));
					device_list_insert(dirname2, "block", HOTPLUG_EVENT_SYSFS_BLOCK);
				}
				closedir(dir2);
			}
		}
		closedir(dir);
	}
}

static void scan_class(void)
{
	char base[HAL_PATH_MAX];
	DIR *dir;
	struct dirent *dent;

	g_strlcpy(base, get_hal_sysfs_path (), sizeof(base));
	g_strlcat(base, "/class", sizeof(base));

	dir = opendir(base);
	if (dir != NULL) {
		for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
			char dirname[HAL_PATH_MAX];
			DIR *dir2;
			struct dirent *dent2;

			if (dent->d_name[0] == '.')
				continue;

			g_strlcpy(dirname, base, sizeof(dirname));
			g_strlcat(dirname, "/", sizeof(dirname));
			g_strlcat(dirname, dent->d_name, sizeof(dirname));
			dir2 = opendir(dirname);
			if (dir2 != NULL) {
				for (dent2 = readdir(dir2); dent2 != NULL; dent2 = readdir(dir2)) {
					char dirname2[HAL_PATH_MAX];

					if (dent2->d_name[0] == '.')
						continue;
					if (!strcmp(dent2->d_name, "device"))
						continue;

					g_strlcpy(dirname2, dirname, sizeof(dirname2));
					g_strlcat(dirname2, "/", sizeof(dirname2));
					g_strlcat(dirname2, dent2->d_name, sizeof(dirname2));
					device_list_insert(dirname2, dent->d_name, HOTPLUG_EVENT_SYSFS_CLASS);
				}
				closedir(dir2);
			}
		}
		closedir(dir);
	}
}

static void queue_events(void)
{
	GSList *dev;

	/* queue events for the devices */
	for (dev = device_list; dev != NULL; dev = g_slist_next (dev)) {
		HotplugEvent *hotplug_event;
		struct sysfs_device *sysfs_dev = dev->data;

		hotplug_event = coldplug_get_hotplug_event (sysfs_dev->path,
							    sysfs_dev->subsystem,
							    sysfs_dev->type);
		hotplug_event_enqueue (hotplug_event);

		g_free (sysfs_dev->path);
		g_free (sysfs_dev->subsystem);
	}

	g_slist_free (device_list);
	device_list = NULL;
}

static int _device_order (const void *d1, const void *d2)
{
	const struct sysfs_device *dev1 = d1;
	const struct sysfs_device *dev2 = d2;

	/* device mapper needs to be the last events, to have the other block devs already around */
	if (strstr(dev2->path, "/" DMPREFIX))
		return -1;
	if (strstr(dev1->path, "/" DMPREFIX))
		return 1;

	return strcmp(dev1->path, dev2->path);
}

gboolean
coldplug_synthesize_events (void)
{
	if (hal_util_init_sysfs_to_udev_map () == FALSE) {
		HAL_ERROR (("Unable to get sysfs to dev map"));
		goto error;
	}

	scan_bus ();
	device_list = g_slist_sort (device_list, _device_order);
	queue_events ();

	scan_class ();
	device_list = g_slist_sort (device_list, _device_order);
	queue_events ();

	scan_block ();
	device_list = g_slist_sort (device_list, _device_order);
	queue_events ();

	g_hash_table_destroy (sysfs_to_udev_map);
	return TRUE;

error:
	return FALSE;
}
