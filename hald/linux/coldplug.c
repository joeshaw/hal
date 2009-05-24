
/*
 * Synthesize device events when starting up
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005-2006 Kay Sievers <kay.sievers@vrfy.org>
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
#include "../util_helper.h"
#include "osspec_linux.h"
#include "hotplug.h"
#include "coldplug.h"
#include "blockdev.h"

#define DMPREFIX "dm-"

struct sysfs_device {
	char *path;
	char *subsystem;
	HotplugEventType type;
};

static GHashTable *sysfs_to_udev_map;
static GSList *device_list;
static char dev_root[HAL_PATH_MAX];
static gchar *udevinfo_stdout = NULL;
static unsigned long long coldplug_seqnum = 0;
typedef struct _UdevInfo UdevInfo;

struct _UdevInfo
{
	const char *sysfs_path;
	const char *device_file;
	const char *vendor;
	const char *model;
	const char *revision;
	const char *serial;
	const char *fsusage;
	const char *fstype;
	const char *fsversion;
	const char *fsuuid;
	const char *fslabel;
};

static void udev_info_free (gpointer data)
{
	g_slice_free(UdevInfo, data);
}


static HotplugEvent*
udev_info_to_hotplug_event (const UdevInfo *info)
{
	HotplugEvent *hotplug_event;
	gchar *str;

	hotplug_event = g_slice_new0 (HotplugEvent);
	g_strlcpy (hotplug_event->sysfs.sysfs_path, "/sys", sizeof(hotplug_event->sysfs.sysfs_path));
	g_strlcat (hotplug_event->sysfs.sysfs_path, info->sysfs_path, sizeof(hotplug_event->sysfs.sysfs_path));

	HAL_INFO(("creating HotplugEvent for %s", hotplug_event->sysfs.sysfs_path));

	if (info->device_file) {
		g_snprintf (hotplug_event->sysfs.device_file, sizeof(hotplug_event->sysfs.device_file),
			"%s/%s", dev_root, info->device_file);
		HAL_INFO(("with device file %s", hotplug_event->sysfs.device_file));
		if (strstr(hotplug_event->sysfs.device_file, "/" DMPREFIX) != NULL) {
			HAL_INFO (("Found a dm-device (%s) , mark it", hotplug_event->sysfs.device_file));
			hotplug_event->sysfs.is_dm_device = TRUE;
		}
	}
	if ((str = hal_util_strdup_valid_utf8(info->vendor)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.vendor, str, sizeof(hotplug_event->sysfs.vendor));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->model)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.model, str, sizeof(hotplug_event->sysfs.model));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->revision)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.revision, str, sizeof(hotplug_event->sysfs.revision));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->serial)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.serial, str, sizeof(hotplug_event->sysfs.serial));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->fsusage)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.fsusage, str, sizeof(hotplug_event->sysfs.fsusage));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->fstype)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.fstype, str, sizeof(hotplug_event->sysfs.fstype));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->fsversion)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.fsversion, str, sizeof(hotplug_event->sysfs.fsversion));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->fsuuid)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.fsuuid, str, sizeof(hotplug_event->sysfs.fsuuid));
		g_free (str);
	}

	if ((str = hal_util_strdup_valid_utf8(info->fslabel)) != NULL) {
		g_strlcpy (hotplug_event->sysfs.fslabel, str, sizeof(hotplug_event->sysfs.fslabel));
		g_free (str);
	}

	return hotplug_event;
}


static gboolean
hal_util_init_sysfs_to_udev_map (void)
{
	char *udevdb_export_argv[] = { "/sbin/udevadm", "info", "-e", NULL };
	char *udevroot_argv[] = { "/sbin/udevadm", "info", "-r", NULL };
	int udevinfo_exitcode;
	UdevInfo *info = NULL;
	char *p;
	int len;

	sysfs_to_udev_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, udev_info_free);

	/* get udevroot */
	if (g_spawn_sync ("/", udevroot_argv, NULL, G_SPAWN_LEAVE_DESCRIPTORS_OPEN, NULL, NULL,
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
	if (g_spawn_sync ("/", udevdb_export_argv, NULL, G_SPAWN_LEAVE_DESCRIPTORS_OPEN, NULL, NULL,
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

		/* get line, terminate and move to next line */
		line = p;
		end = strchr(line, '\n');
		if (end == NULL)
			break;
		end[0] = '\0';
		p = &end[1];

		/* insert device */
		if (line[0] == '\0') {
			if (info != NULL) {
				g_hash_table_insert (sysfs_to_udev_map, g_strdup_printf ("/sys%s", info->sysfs_path), info);
				HAL_INFO (("found (udevdb export) '/sys%s' -> '%s/%s'",
					   info->sysfs_path, dev_root, info->device_file));
				info = NULL;
			}
			continue;
		}

		/* new device */
		if (strncmp(line, "P: ", 3) == 0) {
			info = g_slice_new0 (UdevInfo);
			info->sysfs_path = &line[3];
			continue;
		}

		/* only valid if we have an actual device */
		if (info == NULL)
			continue;

		if (strncmp(line, "N: ", 3) == 0) {
			info->device_file = &line[3];
		} else if (strncmp(line, "E: ID_VENDOR=", 13) == 0) {
			info->vendor = &line[13];
		} else if (strncmp(line, "E: ID_MODEL=", 12) == 0) {
			info->model = &line[12];
		} else if (strncmp(line, "E: ID_REVISION=", 15) == 0) {
			info->revision = &line[15];
		} else if (strncmp(line, "E: ID_SERIAL=", 13) == 0) {
			info->serial = &line[13];
		} else if (strncmp(line, "E: ID_FS_USAGE=", 15) == 0) {
			info->fsusage = &line[15];
		} else if (strncmp(line, "E: ID_FS_TYPE=", 14) == 0) {
			info->fstype = &line[14];
		} else if (strncmp(line, "E: ID_FS_VERSION=", 17) == 0) {
			info->fsversion = &line[17];
		} else if (strncmp(line, "E: ID_FS_UUID=", 14) == 0) {
			info->fsuuid = &line[14];
		} else if (strncmp(line, "E: ID_FS_LABEL_ENC=", 19) == 0) {
			len = strlen (&line[15]);
			info->fslabel = g_malloc0 (len + 1);
			hal_util_decode_escape (&line[19], (char *)info->fslabel, len + 1);
		}
	}

	return TRUE;

error:
	g_free(udevinfo_stdout);
	g_hash_table_destroy (sysfs_to_udev_map);
	sysfs_to_udev_map = NULL;
	return FALSE;
}

static HotplugEvent
*coldplug_get_hotplug_event(const gchar *sysfs_path, const gchar *subsystem, HotplugEventType type)
{
	HotplugEvent *hotplug_event;
	const char *pos;
	gchar path[HAL_PATH_MAX];
	struct stat statbuf;
	UdevInfo *info;

	/* lookup if udev has something stored in its database */
	info = (UdevInfo*) g_hash_table_lookup (sysfs_to_udev_map, sysfs_path);
	if (info) {
		hotplug_event = udev_info_to_hotplug_event (info);
		HAL_INFO (("new event (dev node from udev) '%s' '%s'", hotplug_event->sysfs.sysfs_path, hotplug_event->sysfs.device_file));
	} else {
		hotplug_event = g_slice_new0 (HotplugEvent);

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
		if (strstr(hotplug_event->sysfs.device_file, "/" DMPREFIX) != NULL) {
			HAL_INFO (("Found a dm-device (%s) , mark it", hotplug_event->sysfs.device_file));
			hotplug_event->sysfs.is_dm_device = TRUE;
		}
	}

no_node:
	if (hotplug_event->sysfs.device_file[0] == '\0')
		HAL_INFO (("new event (no dev node) '%s'", sysfs_path));
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = type;
	hotplug_event->sysfs.net_ifindex = -1;
	/*emulate sequence numbers for coldplug events*/
	hotplug_event->sysfs.seqnum = coldplug_seqnum++;
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

	sysfs_dev = g_slice_new0 (struct sysfs_device);
	if (sysfs_dev == NULL)
		goto error;

	/* resolve possible link to real target */
	if (lstat(path, &statbuf) < 0)
		goto error;
	if (S_ISLNK(statbuf.st_mode)) {
		gchar *target;

		if ((target = hal_util_readlink (path)) != NULL) {
			gchar *normalized_target;

			g_strlcpy(filename, path, sizeof(filename));
			hal_util_path_ascend (filename);
			normalized_target = hal_util_get_normalized_path (filename, target);
			sysfs_dev->path = normalized_target;
			goto found;
		}
		goto error;
	}

	sysfs_dev->path = g_strdup (path);
found:
	sysfs_dev->subsystem = g_strdup (subsystem);
	device_list = g_slist_append (device_list, sysfs_dev);
	return 0;

error:
	g_slice_free (struct sysfs_device, sysfs_dev);
	return -1;
}

static void
scan_single_bus (const char *bus_name)
{
        char dirname[HAL_PATH_MAX];
        DIR *dir2;
        struct dirent *dent2;
        
        g_strlcpy(dirname, "/sys/bus/", sizeof(dirname));
        g_strlcat(dirname, bus_name, sizeof(dirname));
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
                        device_list_insert(dirname2, bus_name, HOTPLUG_EVENT_SYSFS_DEVICE);
                }
                closedir(dir2);
        }
}

static void scan_subsystem(const char *subsys)
{
	char base[HAL_PATH_MAX];
	DIR *dir;
	struct dirent *dent;

	g_strlcpy(base, "/sys/", sizeof(base));
	g_strlcat(base, subsys, sizeof(base));

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
					device_list_insert(dirname2, dent->d_name, HOTPLUG_EVENT_SYSFS_DEVICE);
				}
				closedir(dir2);
			}
		}
		closedir(dir);
	}
}

static void scan_block(void)
{
	DIR *dir;
	struct dirent *dent;

	dir = opendir("/sys/block");
	if (dir != NULL) {
		for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
			char dirname[HAL_PATH_MAX];
			DIR *dir2;
			struct dirent *dent2;

			if (dent->d_name[0] == '.')
				continue;

                        /* md devices are handled via looking at /proc/mdstat */
                        if (g_str_has_prefix (dent->d_name, "md")) {
                                HAL_INFO (("skipping md event for %s", dent->d_name));
                                continue;
                        }

			g_strlcpy(dirname, "/sys/block/", sizeof(dirname));
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
	DIR *dir;
	struct dirent *dent;

	dir = opendir("/sys/class");
	if (dir != NULL) {
		for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
			char dirname[HAL_PATH_MAX];
			DIR *dir2;
			struct dirent *dent2;

			if (dent->d_name[0] == '.')
				continue;

			g_strlcpy(dirname, "/sys/class/", sizeof(dirname));
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
					device_list_insert(dirname2, dent->d_name, HOTPLUG_EVENT_SYSFS_DEVICE);
				}
				closedir(dir2);
			}
		}
		closedir(dir);
	}
}

static void process_coldplug_events(void)
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
		hotplug_event_process_queue();

		g_free (sysfs_dev->path);
		g_free (sysfs_dev->subsystem);
		g_slice_free (struct sysfs_device, sysfs_dev);
	}

	g_slist_free (device_list);
	device_list = NULL;
}

static int _device_order (const void *d1, const void *d2)
{
	const struct sysfs_device *dev1 = d1;
	const struct sysfs_device *dev2 = d2;

	/* device mapper needs to be the last events, to have the other block devs already around */
	if (strstr (dev2->path, "/" DMPREFIX))
		return -1;
	if (strstr (dev1->path, "/" DMPREFIX))
		return 1;

	return strcmp(dev1->path, dev2->path);
}

gboolean
coldplug_synthesize_events (void)
{
	struct stat statbuf;

	if (hal_util_init_sysfs_to_udev_map () == FALSE) {
		HAL_ERROR (("Unable to get sysfs to dev map"));
		goto error;
	}

	/* if we have /sys/subsystem, forget all the old stuff */
	if (stat("/sys/subsystem", &statbuf) == 0) {
		scan_subsystem ("subsystem");
		device_list = g_slist_sort (device_list, _device_order);
		process_coldplug_events ();
	} else {
		scan_subsystem ("bus");
		device_list = g_slist_sort (device_list, _device_order);
		process_coldplug_events ();

		scan_class ();
                scan_single_bus ("bluetooth");
		device_list = g_slist_sort (device_list, _device_order);
		process_coldplug_events ();

		/* scan /sys/block, if it isn't already a class */
		if (stat("/sys/class/block", &statbuf) != 0) {
			scan_block ();
			device_list = g_slist_sort (device_list, _device_order);
			process_coldplug_events ();
		}

                /* add events from reading /proc/mdstat */
                blockdev_process_mdstat ();
	}

	g_hash_table_destroy (sysfs_to_udev_map);
	g_free (udevinfo_stdout);
	return TRUE;

error:
	return FALSE;
}
