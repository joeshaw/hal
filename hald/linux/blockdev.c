/***************************************************************************
 * CVSID: $Id$
 *
 * blockdev.c : Handling of block devices
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005,2006 Kay Sievers, <kay.sievers@vrfy.org>
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

#include <ctype.h>
#include <limits.h>
#include <linux/kdev_t.h>
#include <mntent.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../hald.h"
#include "../hald_dbus.h"
#include "../hald_runner.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"

#include "coldplug.h"
#include "hotplug.h"
#include "hotplug_helper.h"
#include "osspec_linux.h"

#include "blockdev.h"

static gchar *
strdup_valid_utf8 (const char *str)
{
        char *endchar;
        char *newstr;
        unsigned int fixes;

        if (str == NULL)
                return NULL;

        newstr = g_strdup (str);

        fixes = 0;
        while (!g_utf8_validate (newstr, -1, (const char **) &endchar)) {
                *endchar = '_';
                ++fixes;
        }

        /* If we had to fix more than 20% of the characters, give up */
        if (fixes > 0 && g_utf8_strlen (newstr, -1) / fixes < 5) {
            g_free (newstr);
            newstr = g_strdup("");
        }

        return newstr;
}


/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
blockdev_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_property_get_bool (d, "block.is_volume")) {
		const char *label;
		const char *uuid;
		char *volumelabel;

		label = hal_device_property_get_string (d, "volume.label");
	        /* replace '/'  to avoid trouble if the string get part of the UDI see fd.o #11401 */
		volumelabel = g_strdup(label);
	        volumelabel = g_strdelimit (volumelabel, "/", '_');

		uuid = hal_device_property_get_string (d, "volume.uuid");

		if (uuid != NULL && strlen (uuid) > 0) {
			hald_compute_udi (udi, sizeof (udi),
					  "/org/freedesktop/Hal/devices/volume_uuid_%s", uuid);
		} else if (volumelabel != NULL && strlen (volumelabel) > 0) {
			hald_compute_udi (udi, sizeof (udi),
					  "/org/freedesktop/Hal/devices/volume_label_%s", volumelabel);
		} else if (hal_device_property_get_bool(d, "volume.is_disc") &&
			   hal_device_property_get_bool(d, "volume.disc.is_blank")) {
			/* this should be a empty CD/DVD */
			hald_compute_udi (udi, sizeof (udi),
                                          "/org/freedesktop/Hal/devices/volume_empty_%s",
					  hal_device_property_get_string (d, "volume.disc.type"));
		} else {
			/* fallback to partition number, size */
			hald_compute_udi (udi, sizeof (udi),
					  "/org/freedesktop/Hal/devices/volume_part%d_size_%lld",
					  hal_device_property_get_int (d, "volume.partition.number"),
					  hal_device_property_get_uint64 (d, "volume.size"));
		}
		g_free(volumelabel);
	} else {
		const char *model;
		const char *serial;
		const char *bus;
		const char *type;

		model = hal_device_property_get_string (d, "storage.model");
		serial = hal_device_property_get_string (d, "storage.serial");
		bus = hal_device_property_get_string (d, "storage.bus");
		type = hal_device_property_get_string (d, "storage.drive_type");

		if (serial != NULL) {
			hald_compute_udi (udi, sizeof (udi),
					  "/org/freedesktop/Hal/devices/storage_serial_%s",
					  serial);
		} else if ((model != NULL) && (strlen(model) != 0) ) {
			hald_compute_udi (udi, sizeof (udi),
					  "/org/freedesktop/Hal/devices/storage_model_%s",
					  model);
		} else if ((bus != NULL) && (type != NULL)){
			hald_compute_udi (udi, sizeof (udi),
					  "%s_storage_%s_%s",
					  hal_device_property_get_string (d, "storage.originating_device"),
					  bus, type);
		} else {
			hald_compute_udi (udi, sizeof (udi),
					  "%s_storage",
					  hal_device_property_get_string (d, "storage.originating_device"));
		}
	}

	hal_device_set_udi (d, udi);

	return TRUE;
}


static void 
blockdev_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
blockdev_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Remove callouts completed udi=%s", hal_device_get_udi (d)));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}
	g_object_unref (d);

	hotplug_event_end (end_token);
}

static void
cleanup_mountpoint_cb (HalDevice *d, guint32 exit_type, 
		       gint return_code, gchar **error,
		       gpointer data1, gpointer data2)
{
	char *mount_point = (char *) data1;
	HAL_INFO (("In cleanup_mountpoint_cb for '%s'", mount_point));
	g_free (mount_point);
}

void
blockdev_refresh_mount_state (HalDevice *d)
{
	FILE *f;
	struct mntent mnt;
	char buf[1024];
	GSList *volumes = NULL;
	GSList *volume;

	/* open /proc/mounts */
	g_snprintf (buf, sizeof (buf), "%s/mounts", "/proc");
	if ((f = setmntent (buf, "r")) == NULL) {
		HAL_ERROR (("Could not open /proc/mounts"));
		return;
	}

	if (d)
		volumes = g_slist_append (NULL, d);
	else
		volumes = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), "info.category", "volume");

	if (!volumes)
		goto exit;

	/* loop over /proc/mounts */
	while (getmntent_r (f, &mnt, buf, sizeof(buf)) != NULL) {
		struct stat statbuf;
		dev_t devt;

		/* HAL_INFO ((" * /proc/mounts contain dev %s - type %s", mnt.mnt_fsname, mnt.mnt_type)); */

		/* We don't handle nfs mounts in HAL and stat() on mountpoints,
		 * and we would block on 'stale nfs handle'.
		 */
		if (strcmp(mnt.mnt_type, "nfs") == 0)
			continue;

		/* skip plain names, we look for device nodes */
		if (mnt.mnt_fsname[0] != '/')
			continue;

		/*
		 * We can't just stat() the mountpoint, because it breaks all sorts
		 * non-disk filesystems. So assume, that the names in /proc/mounts
		 * are existing device-files used to mount the filesystem.
		 */
		devt = makedev(0, 0);
		if (stat (mnt.mnt_fsname, &statbuf) == 0) {
			/* not a device node */
			if (major (statbuf.st_rdev) == 0)
				continue;

			/* found major/minor */
			devt = statbuf.st_rdev;
		} else {
			/* The root filesystem may be mounted by a device name that doesn't
			 * exist in the real root, like /dev/root, which the kernel uses
			 * internally, when no initramfs image is used. For "/", it is safe
			 * to get the major/minor by stat()'ing the mount-point.
			 */
			if (strcmp (mnt.mnt_dir, "/") == 0 && stat ("/", &statbuf) == 0)
				devt = statbuf.st_dev;

			/* DING DING DING... the device-node may not exist, or is
			 * already deleted, but the device may be still mounted.
			 *
			 * We will fall back to looking up the device-name, instead
			 * of using major/minor.
		 	 */
		}

		/* HAL_INFO (("* found mounts dev %s (%i:%i)", mnt.mnt_fsname,
			   major (devt), minor (devt))); */

		for (volume = volumes; volume != NULL; volume = g_slist_next (volume)) {
			HalDevice *dev;
			gboolean is_match;

			is_match = FALSE;
			dev = HAL_DEVICE (volume->data);

			/* lookup dev_t or devname of known hal devices */
			if (major (devt) == 0) {
				const char *device_name;

				device_name = hal_device_property_get_string (dev, "block.device");
				if (device_name == NULL)
					continue;

				if (strcmp (device_name, mnt.mnt_fsname) == 0)
					is_match = TRUE;
			} else {
				unsigned int majornum;
				unsigned int minornum;

				majornum = hal_device_property_get_int (dev, "block.major");
				if (majornum == 0)
					continue;
				minornum = hal_device_property_get_int (dev, "block.minor");
				/* HAL_INFO (("  match %s (%i:%i)", hal_device_get_udi (dev), majornum, minornum)); */

				if (majornum == major (devt) && minornum == minor (devt))
					is_match = TRUE;
			}

			if (is_match) {
				/* found entry for this device in /proc/mounts */
				device_property_atomic_update_begin ();
				hal_device_property_set_bool (dev, "volume.is_mounted", TRUE);
				hal_device_property_set_bool (dev, "volume.is_mounted_read_only",
							      hasmntopt (&mnt, MNTOPT_RO) ? TRUE : FALSE);
				hal_device_property_set_string (dev, "volume.mount_point", mnt.mnt_dir);
				device_property_atomic_update_end ();
				/* HAL_INFO (("  set %s to be mounted at %s (%s)", hal_device_get_udi (dev),
					   mnt.mnt_dir, hasmntopt (&mnt, MNTOPT_RO) ? "ro" : "rw")); */
				volumes = g_slist_delete_link (volumes, volume);
				break;
			}
		}
	}

	/* all remaining volumes are not mounted */
	for (volume = volumes; volume != NULL; volume = g_slist_next (volume)) {
		HalDevice *dev;

		dev = HAL_DEVICE (volume->data);
		/* do nothing if we have a Unmount() method running on the object. This is
		 * is because on Linux /proc/mounts is changed immediately while umount(8)
		 * doesn't return until the block cache is flushed. Note that when Unmount()
		 * terminates we'll be checking /proc/mounts again so this event is not
		 * lost... it is merely delayed...
		 */
		if (device_is_executing_method (dev, "org.freedesktop.Hal.Device.Volume", "Unmount")) {
			HAL_INFO (("/proc/mounts tells that %s is unmounted - waiting for Unmount() to complete to change mount state", hal_device_get_udi (dev)));
		} else {
			char *mount_point;

			mount_point = g_strdup (hal_device_property_get_string (dev, "volume.mount_point"));
			device_property_atomic_update_begin ();
			hal_device_property_set_bool (dev, "volume.is_mounted", FALSE);
			hal_device_property_set_bool (dev, "volume.is_mounted_read_only", FALSE);
			hal_device_property_set_string (dev, "volume.mount_point", "");
			device_property_atomic_update_end ();
			/*HAL_INFO (("set %s to unmounted", hal_device_get_udi (dev)));*/
			
			if (mount_point != NULL && strlen (mount_point) > 0 && 
			    hal_util_is_mounted_by_hald (mount_point)) {
				char *cleanup_stdin;
				char *extra_env[2];
				
				HAL_INFO (("Cleaning up directory '%s' since it was created by HAL's Mount()", mount_point));
				
				extra_env[0] = g_strdup_printf ("HALD_CLEANUP=%s", mount_point);
				extra_env[1] = NULL;
				cleanup_stdin = "\n";
				
				hald_runner_run_method (dev, 
							"hal-storage-cleanup-mountpoint", 
							extra_env, 
							cleanup_stdin, TRUE,
							0,
							cleanup_mountpoint_cb,
							g_strdup (mount_point), NULL);
			}

			g_free (mount_point);
		}

	}
	g_slist_free (volumes);
exit:
	endmntent (f);
}

static void
generate_fakevolume_hotplug_event_add_for_storage_device (HalDevice *d)
{
	const char *sysfs_path;
	const char *device_file;
	HotplugEvent *hotplug_event;
	char fake_sysfs_path[HAL_PATH_MAX];

	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");
	device_file = hal_device_property_get_string (d, "block.device");

	snprintf (fake_sysfs_path, sizeof(fake_sysfs_path), "%s/fakevolume", sysfs_path);

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, fake_sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	if (device_file != NULL)
		g_strlcpy (hotplug_event->sysfs.device_file, device_file, sizeof (hotplug_event->sysfs.device_file));
	else
		hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	hotplug_event_enqueue (hotplug_event);
}

static void 
add_blockdev_probing_helper_done (HalDevice *d, guint32 exit_type, 
                                  gint return_code, char **error,
                                  gpointer data1, gpointer data2) 
{
	void *end_token = (void *) data1;
	gboolean is_volume;

	/* helper_data may be null if probing is skipped */

	HAL_INFO (("entering; exit_type=%d, return_code=%d", exit_type, return_code));

	if (d == NULL) {
		HAL_INFO (("Device object already removed"));
		hotplug_event_end (end_token);
		goto out;
	}

	is_volume = hal_device_property_get_bool (d, "block.is_volume");

	/* Discard device if probing reports failure 
	 * 
	 * (return code 2 means fs found on main block device (for non-volumes)) 
	 */
	if (exit_type != HALD_RUN_SUCCESS
	    || !(return_code == 0 || (!is_volume && return_code == 2))) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		hotplug_event_end (end_token);
		goto out;
	}

	if (!blockdev_compute_udi (d)) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		hotplug_event_end (end_token);
		goto out;
	}

	/* set block.storage_device for storage devices since only now we know the UDI */
	if (!is_volume) {
		hal_device_copy_property (d, "info.udi", d, "block.storage_device");
	} else {
		/* check for mount point */
		blockdev_refresh_mount_state (d);
	}

	/* Merge properties from .fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

	/* TODO: Merge persistent properties */

	/* Run callouts */
	hal_util_callout_device_add (d, blockdev_callouts_add_done, end_token, NULL);

	/* Yay, got a file system on the main block device...
	 *
	 * Generate a fake hotplug event to get this added
	 */
	if (!is_volume && return_code == 2) {
		generate_fakevolume_hotplug_event_add_for_storage_device (d);
	}


out:
	return;
}

static void 
blockdev_callouts_preprobing_storage_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	if (hal_device_property_get_bool (d, "info.ignore")) {
		/* Leave the device here with info.ignore==TRUE so we won't pick up children 
		 * Also remove category and all capabilities
		 */
		hal_device_property_remove (d, "info.category");
		hal_device_property_remove (d, "info.capabilities");
		hal_device_property_set_string (d, "info.udi", "/org/freedesktop/Hal/devices/ignored-device");
		hal_device_property_set_string (d, "info.product", "Ignored Device");

		HAL_INFO (("Preprobing merged info.ignore==TRUE"));

		/* Move from temporary to global device store */
		hal_device_store_remove (hald_get_tdl (), d);
		hal_device_store_add (hald_get_gdl (), d);
		
		hotplug_event_end (end_token);
		goto out;
	}

	if (!hal_device_property_get_bool (d, "storage.media_check_enabled") &&
	    hal_device_property_get_bool (d, "storage.no_partitions_hint")) {

		/* special probe for PC floppy drives */
		if (strcmp (hal_device_property_get_string (d, "storage.bus"), "platform") == 0 &&
		    strcmp (hal_device_property_get_string (d, "storage.drive_type"), "floppy") == 0) {
			HAL_INFO (("Probing PC floppy %s to see if it is present", 
				   hal_device_property_get_string (d, "block.device")));

			hald_runner_run(d, 
			                    "hald-probe-pc-floppy", NULL,
			                    HAL_HELPER_TIMEOUT,
			                    add_blockdev_probing_helper_done,
			                    (gpointer) end_token, NULL);
			goto out;
		} else {
			char *synerror[1] = {NULL};

			HAL_INFO (("Not probing storage device %s", 
				   hal_device_property_get_string (d, "block.device")));

			add_blockdev_probing_helper_done (d, FALSE, 0, synerror, (gpointer) end_token, NULL);
			goto out;
		}
	}

	/* run prober for 
	 *
	 *  - cdrom drive properties
	 *  - non-partitioned filesystem on main block device
	 */

	HAL_INFO (("Probing storage device %s", hal_device_property_get_string (d, "block.device")));

	/* probe the device */
	hald_runner_run(d,
			"hald-probe-storage", NULL,
			HAL_HELPER_TIMEOUT,
			add_blockdev_probing_helper_done,
			(gpointer) end_token, NULL);

out:
	return;
}

static void
blockdev_callouts_preprobing_volume_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	if (hal_device_property_get_bool (d, "info.ignore")) {
		/* Leave the device here with info.ignore==TRUE so we won't pick up children
		 * Also remove category and all capabilities
		 */
		hal_device_property_remove (d, "info.category");
		hal_device_property_remove (d, "info.capabilities");
		hal_device_property_set_string (d, "info.udi", "/org/freedesktop/Hal/devices/ignored-device");
		hal_device_property_set_string (d, "info.product", "Ignored Device");

		HAL_INFO (("Preprobing merged info.ignore==TRUE"));

		/* Move from temporary to global device store */
		hal_device_store_remove (hald_get_tdl (), d);
		hal_device_store_add (hald_get_gdl (), d);

		hotplug_event_end (end_token);
		goto out;
	}

	/* probe the device */
	hald_runner_run (d,
			 "hald-probe-volume", NULL, 
			 HAL_HELPER_TIMEOUT,
			 add_blockdev_probing_helper_done,
			 (gpointer) end_token, NULL);
out:
	return;
}

/* borrowed from gtk/gtkfilesystemunix.c in GTK+ on 02/23/2006 
 * (which is LGPL, so can't go into hald/utils.[ch] until it's relicensed)
 */
static void
canonicalize_filename (gchar *filename)
{
	gchar *p, *q;
	gboolean last_was_slash = FALSE;
	
	p = filename;
	q = filename;
	
	while (*p)
	{
		if (*p == G_DIR_SEPARATOR)
		{
			if (!last_was_slash)
				*q++ = G_DIR_SEPARATOR;
			
			last_was_slash = TRUE;
		}
		else
		{
			if (last_was_slash && *p == '.')
			{
				if (*(p + 1) == G_DIR_SEPARATOR ||
				    *(p + 1) == '\0')
				{
					if (*(p + 1) == '\0')
						break;
					
					p += 1;
				}
				else if (*(p + 1) == '.' &&
					 (*(p + 2) == G_DIR_SEPARATOR ||
					  *(p + 2) == '\0'))
				{
					if (q > filename + 1)
					{
						q--;
						while (q > filename + 1 &&
						       *(q - 1) != G_DIR_SEPARATOR)
							q--;
					}
					
					if (*(p + 2) == '\0')
						break;
					
					p += 2;
				}
				else
				{
					*q++ = *p;
					last_was_slash = FALSE;
				}
			}
			else
			{
				*q++ = *p;
				last_was_slash = FALSE;
			}
		}
		
		p++;
	}
	
	if (q > filename + 1 && *(q - 1) == G_DIR_SEPARATOR)
		q--;
	
	*q = '\0';
}

static char *
resolve_symlink (const char *file)
{
	char *dir;
	gchar link[HAL_PATH_MAX];
	char *f;
	char *f1;

	f = g_strdup (file);
	memset(link, 0, HAL_PATH_MAX);
	while (g_file_test (f, G_FILE_TEST_IS_SYMLINK)) {
		if(readlink(f, link, HAL_PATH_MAX - 1) < 0) {
			g_warning ("Cannot resolve symlink %s: %s", f, strerror(errno));
			g_free (f);
			f = NULL;
			goto out;
		}
		
		dir = g_path_get_dirname (f);
		f1 = g_strdup_printf ("%s/%s", dir, link);
		g_free (dir);
		g_free (f);
		f = f1;
	}

out:
	if (f != NULL)
		canonicalize_filename (f);
	return f;
}

static gboolean
refresh_md_state (HalDevice *d);

static gboolean
md_check_sync_timeout (gpointer user_data)
{
        HalDevice *d;
        char *sysfs_path = (char *) user_data;

        HAL_INFO (("In md_check_sync_timeout for sysfs path %s", sysfs_path));

        d = hal_device_store_match_key_value_string (hald_get_gdl (), 
                                                     "storage.linux_raid.sysfs_path", 
                                                     sysfs_path);
        if (d == NULL)
                d = hal_device_store_match_key_value_string (hald_get_tdl (), 
                                                             "storage.linux_raid.sysfs_path", 
                                                             sysfs_path);
        if (d == NULL) {
                HAL_WARNING (("Cannot find md device with sysfs path '%s'", sysfs_path));
                goto out;
        }

        refresh_md_state (d);
        
out:
        g_free (sysfs_path);
        return FALSE;
}

static gboolean
refresh_md_state (HalDevice *d)
{
        int n;
        char *sync_action;
        int num_components;
        gboolean ret;
        const char *sysfs_path;
        const char *level;

        ret = FALSE;

        sysfs_path = hal_device_property_get_string (d, "storage.linux_raid.sysfs_path");
        if (sysfs_path == NULL) {
                HAL_WARNING (("Cannot get sysfs_path for udi %s", hal_device_get_udi (d)));
                goto error;
        }

        HAL_INFO (("In refresh_md_state() for '%s'", sysfs_path));
        level = hal_device_property_get_string (d, "storage.linux_raid.level");
        HAL_INFO ((" MD Level is '%s'", level));
		
        /* MD linear device are not syncable */
        if (strcmp (level, "linear") != 0) {
	        sync_action = hal_util_get_string_from_file (sysfs_path, "md/sync_action");
	        if (sync_action == NULL) {
	                HAL_WARNING (("Cannot get sync_action for %s", sysfs_path));
	                goto error;
	        }
	        if (strcmp (sync_action, "idle") == 0) {
	                hal_device_property_set_bool (d, "storage.linux_raid.is_syncing", FALSE);
			hal_device_property_remove (d, "storage.linux_raid.sync.action");
			hal_device_property_remove (d, "storage.linux_raid.sync.speed");
			hal_device_property_remove (d, "storage.linux_raid.sync.progress");
	        } else {
	                int speed;
	                char *str_completed;
                        
	                hal_device_property_set_bool (d, "storage.linux_raid.is_syncing", TRUE);
                        
	                hal_device_property_set_string (d, "storage.linux_raid.sync.action", sync_action);
                        
			if (!hal_util_get_int_from_file (sysfs_path, "md/sync_speed", &speed, 10)) {
	                        HAL_WARNING (("Cannot get sync_speed for %s", sysfs_path));
	                } else {
	                        hal_device_property_set_uint64 (d, "storage.linux_raid.sync.speed", speed);
	                }
                        
	                if ((str_completed = hal_util_get_string_from_file (sysfs_path, "md/sync_completed")) == NULL) {
	                        HAL_WARNING (("Cannot get sync_completed for %s", sysfs_path));
	                } else {
	                        long long int sync_pos, sync_total;
                                
	                        if (sscanf (str_completed, "%lld / %lld", &sync_pos, &sync_total) != 2) {
	                                HAL_WARNING (("Malformed sync_completed '%s'", str_completed));
	                        } else {
	                                double sync_progress;
	                                sync_progress = ((double) sync_pos) / ((double) sync_total);
	                                hal_device_property_set_double (d, "storage.linux_raid.sync.progress", sync_progress);
	                        }
	                }
                        
	                /* check again in two seconds */
#ifdef HAVE_GLIB_2_14
	                g_timeout_add_seconds (2, md_check_sync_timeout, g_strdup (sysfs_path));
#else
	                g_timeout_add (2000, md_check_sync_timeout, g_strdup (sysfs_path));
#endif
	        }
        } else
                hal_device_property_set_bool (d, "storage.linux_raid.is_syncing", FALSE);
        
        if (!hal_util_get_int_from_file (sysfs_path, "md/raid_disks", &num_components, 0)) {
                HAL_WARNING (("Cannot get number of RAID components"));
                goto error;
        }
        hal_device_property_set_int (d, "storage.linux_raid.num_components", num_components);
        
        /* add all components */
        for (n = 0; n < num_components; n++) {
                char *s;
                char *link;
                char *target;
                HalDevice *slave_volume;
                const char *slave_volume_stordev_udi;
                HalDevice *slave_volume_stordev;

                s = g_strdup_printf ("%s/md/rd%d", sysfs_path, n);
                if (!g_file_test (s, G_FILE_TEST_IS_SYMLINK)) {
                        g_free (s);
                        break;
                }
                g_free (s);
                
                link = g_strdup_printf ("%s/md/rd%d/block", sysfs_path, n);
                target = resolve_symlink (link);
                if (target == NULL) {
                        HAL_WARNING (("Cannot resolve %s", link));
                        g_free (link);
                        goto error;
                }
                HAL_INFO (("link->target: '%s' -> '%s'", link, target));
                
                slave_volume = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", target);
                if (slave_volume == NULL) {
                        HAL_WARNING (("No volume for sysfs path %s", target));
                        g_free (target);
                        g_free (link);
                        goto error;
                }
                
                
                slave_volume_stordev_udi = hal_device_property_get_string (slave_volume, "block.storage_device");
                if (slave_volume_stordev_udi == NULL) {
                        HAL_WARNING (("No storage device for slave"));
                        g_free (target);
                        g_free (link);
                        goto error;
                }
                slave_volume_stordev = hal_device_store_find (hald_get_gdl (), slave_volume_stordev_udi);
                if (slave_volume_stordev == NULL) {
                        HAL_WARNING (("No storage device for slave"));
                        g_free (target);
                        g_free (link);
                        goto error;
                }
                
                
                hal_device_property_strlist_add (d, "storage.linux_raid.components", hal_device_get_udi (slave_volume));
                
                /* derive 
                 *
                 * - hotpluggability (is that a word?) 
                 * - array uuid
                 *
                 * Hmm.. every raid member (PV) get the array UUID. That's
                 * probably.. wrong. TODO: check with Kay.
                 *
                 * from component 0.
                 */
                if (n == 0) {
                        const char *uuid;

                        hal_device_property_set_bool (
                                d, "storage.hotpluggable",
                                hal_device_property_get_bool (slave_volume_stordev, "storage.hotpluggable"));

                        
                        uuid = hal_device_property_get_string (
                                slave_volume, "volume.uuid");
                        if (uuid != NULL) {
                                hal_device_property_set_string (
                                        d, "storage.serial", uuid);
                        }
                }
                
                g_free (target);
                g_free (link);
                
        } /* for all components */

        hal_device_property_set_int (d, "storage.linux_raid.num_components_active", n);
        
        /* TODO: add more state here */

        ret = TRUE;

error:
        return ret;
}


void
hotplug_event_begin_add_blockdev (const gchar *sysfs_path, const gchar *device_file, gboolean is_partition,
				  HalDevice *parent, void *end_token)
{
	HotplugEvent *hotplug_event = (HotplugEvent *) end_token;
	gchar *major_minor;
	HalDevice *d;
	unsigned int major, minor;
	gboolean is_fakevolume;
	char *sysfs_path_real = NULL;
	int floppy_num;
	gboolean is_device_mapper;
        gboolean is_md_device;
	gboolean is_cciss_device;
        int md_number;

	is_device_mapper = FALSE;
        is_fakevolume = FALSE;
        is_md_device = FALSE;
	is_cciss_device = FALSE;

	HAL_INFO (("block_add: sysfs_path=%s dev=%s is_part=%d, parent=0x%08x", 
		   sysfs_path, device_file, is_partition, parent));

	if (parent != NULL && hal_device_property_get_bool (parent, "info.ignore")) {
		HAL_INFO (("Ignoring block_add since parent has info.ignore==TRUE"));
		goto out;
	}

	if (strcmp (hal_util_get_last_element (sysfs_path), "fakevolume") == 0) {
		HAL_INFO (("Handling %s as fakevolume - sysfs_path_real=%s", device_file, sysfs_path_real));
		is_fakevolume = TRUE;
		sysfs_path_real = hal_util_get_parent_path (sysfs_path);
        } else if (sscanf (hal_util_get_last_element (sysfs_path), "md%d", &md_number) == 1) {
		HAL_INFO (("Handling %s as MD device", device_file));
                is_md_device = TRUE;
		sysfs_path_real = g_strdup (sysfs_path);
                /* set parent to root computer device object */
                parent = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
                if (parent == NULL)
                        parent = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	} else {
		sysfs_path_real = g_strdup (sysfs_path);
	}
	
	/* See if we already have device (which we may have as we're ignoring rem/add
	 * for certain classes of devices - see hotplug_event_begin_remove_blockdev)
	 */
	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);
	if (d != NULL) {
		HAL_INFO (("Ignoring hotplug event - device is already added"));
		goto out;
	}

	d = hal_device_new ();

	/* OK, no parent... */
	if (parent == NULL && !is_partition && !is_fakevolume && !hotplug_event->reposted) {
		char path[HAL_PATH_MAX];
		g_snprintf (path, HAL_PATH_MAX, "%s/slaves", sysfs_path);

		if (device_file && (strstr(device_file, "/dev/cciss/") != NULL)) {
			/* ... it's a cciss (HP Smart Array CCISS) device */
			if (g_file_test (path, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
				HAL_DEBUG(("block_add: found cciss a base device"));

				is_cciss_device = TRUE;
                		parent = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
		                if (parent == NULL)
                		        parent = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
			} 
		} else {
			/* ... it might a device-mapper device => check slaves/ subdir in sysfs */
			DIR * dir;
			struct dirent *dp;

			HAL_INFO (("Looking in %s for Device Mapper", path));

			if ((dir = opendir (path)) == NULL) {
				HAL_WARNING (("Unable to open %s: %s", path, strerror(errno)));
			} else {
				while (((dp = readdir (dir)) != NULL) && (parent == NULL)) {
					char *link;
					char *target;

					link = g_strdup_printf ("%s/%s", path, dp->d_name);
					target = resolve_symlink (link);
					HAL_INFO ((" %s -> %s", link, target));
					g_free (link);

					if (target != NULL) {
						HalDevice *slave_volume;

						slave_volume = hal_device_store_match_key_value_string (hald_get_gdl (),
													"linux.sysfs_path", 
													target);
						if (slave_volume != NULL) {
							const char *slave_volume_stordev_udi;
							const char *slave_volume_fstype;

							slave_volume_stordev_udi = hal_device_property_get_string (slave_volume, "block.storage_device");
							slave_volume_fstype = hal_device_property_get_string (slave_volume, "volume.fstype");

							/* Yup, we only support crypto_LUKS right now.
							 *
							 * In the future we can support other device-mapper mappings
							 * such as LVM etc.
							 */ 
							if (slave_volume_stordev_udi != NULL &&
							    slave_volume_fstype != NULL &&
							    (strcmp (slave_volume_fstype, "crypto_LUKS") == 0)) {
								HAL_INFO ((" slave_volume_stordev_udi='%s'!", slave_volume_stordev_udi));
								parent = hal_device_store_find (hald_get_gdl (), slave_volume_stordev_udi);
								if (parent != NULL) {
									HAL_INFO ((" parent='%s'!", hal_device_get_udi (parent)));
									hal_device_property_set_string (d, "volume.crypto_luks.clear.backing_volume", 
													hal_device_get_udi (slave_volume));
									is_device_mapper = TRUE;
								}
							}
						} else {
							HAL_INFO(("Couldn't find slave volume in devices"));
						}
					}
					g_free (target);
				}
				closedir(dir);
				HAL_INFO (("Done looking in %s", path));
			}
		}

	}

	if (parent == NULL) {
		HAL_INFO (("Ignoring hotplug event - no parent"));
		goto error;
	}

	if (!is_fakevolume && hal_device_property_get_bool (parent, "storage.no_partitions_hint")) {
		HAL_INFO (("Ignoring blockdev since not a fakevolume and parent has "
			   "storage.no_partitions_hint==TRUE"));
		goto error;
	}

	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent));
	hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_SYSFS_BLOCK);

	hal_device_property_set_string (d, "block.device", device_file);
	if ((major_minor = hal_util_get_string_from_file (sysfs_path_real, "dev")) == NULL || 
	    sscanf (major_minor, "%d:%d", &major, &minor) != 2) {
		HAL_INFO (("Ignoring hotplug event - cannot read major:minor"));
		goto error;
	}

	hal_device_property_set_int (d, "block.major", major);
	hal_device_property_set_int (d, "block.minor", minor);
	hal_device_property_set_bool (d, "block.is_volume", is_partition || is_device_mapper || is_fakevolume);

	if (hal_device_has_property(parent, "info.subsystem") &&
		(strcmp(hal_device_property_get_string(parent, "info.subsystem"), "platform") == 0) &&
		(sscanf(hal_device_property_get_string(parent, "platform.id"), "floppy.%d", &floppy_num) == 1)) {
		/* for now, just cheat here for floppy drives */

		HAL_INFO (("doing floppy drive hack for floppy %d", floppy_num));

		hal_device_property_set_string (d, "storage.bus", "platform");
		hal_device_property_set_bool (d, "storage.no_partitions_hint", TRUE);
		hal_device_property_set_bool (d, "storage.media_check_enabled", FALSE);
		hal_device_property_set_bool (d, "storage.automount_enabled_hint", TRUE);
		hal_device_property_set_string (d, "storage.model", "");
		hal_device_property_set_string (d, "storage.vendor", "PC Floppy Drive");
		hal_device_property_set_string (d, "info.vendor", "");
		hal_device_property_set_string (d, "info.product", "PC Floppy Drive");
		hal_device_property_set_string (d, "storage.drive_type", "floppy");
		hal_device_property_set_string (d, "storage.originating_device", hal_device_get_udi (parent));
		hal_device_property_set_bool (d, "storage.removable", TRUE);
		hal_device_property_set_bool (d, "storage.removable.media_available", FALSE);
		hal_device_property_set_bool (d, "storage.hotpluggable", FALSE);
		hal_device_property_set_bool (d, "storage.requires_eject", FALSE);
		hal_device_property_set_uint64 (d, "storage.size", 0);

		hal_device_property_set_string (d, "info.category", "storage");
		hal_device_add_capability (d, "storage");
		hal_device_add_capability (d, "block");

		/* add to TDL so preprobing callouts and prober can access it */
		hal_device_store_add (hald_get_tdl (), d);

		/* Process preprobe fdi files */
		di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);

		/* Run preprobe callouts */
		hal_util_callout_device_preprobe (d, blockdev_callouts_preprobing_storage_done, end_token, NULL);
		goto out2;
	}

	if (!is_partition && !is_device_mapper && !is_fakevolume) {
		const char *udi_it;
		const char *physdev_udi;
		HalDevice *scsidev;
		HalDevice *physdev;
		gboolean is_hotpluggable;
		gboolean is_removable;
		gboolean requires_eject;
		gboolean no_partitions_hint;
		const gchar *bus;
		const gchar *parent_bus;

		/********************************
		 * storage devices
		 *******************************/

		scsidev = NULL;
		physdev = NULL;
		physdev_udi = NULL;

		is_removable = FALSE;
		is_hotpluggable = FALSE;
		requires_eject = FALSE;
		no_partitions_hint = FALSE;

		/* defaults */
		hal_device_property_set_string (d, "storage.bus", "unknown");
		hal_device_property_set_bool (d, "storage.no_partitions_hint", TRUE);
		hal_device_property_set_bool (d, "storage.media_check_enabled", TRUE);
		hal_device_property_set_bool (d, "storage.automount_enabled_hint", TRUE);
		hal_device_property_set_string (d, "storage.drive_type", "disk");

		/* persistent properties from udev (may be empty) */
		hal_device_property_set_string (d, "storage.model", hotplug_event->sysfs.model);
		hal_device_property_set_string (d, "storage.vendor", hotplug_event->sysfs.vendor);
		if (hotplug_event->sysfs.serial[0] != '\0')
			hal_device_property_set_string (d, "storage.serial", hotplug_event->sysfs.serial);
		if (hotplug_event->sysfs.revision[0] != '\0')
			hal_device_property_set_string (d, "storage.firmware_version", hotplug_event->sysfs.revision);

		/* walk up the device chain to find the physical device, 
		 * start with our parent. On the way, optionally pick up
		 * the scsi if it exists */
		udi_it = hal_device_get_udi (parent);
		while (udi_it != NULL) {
			HalDevice *d_it;

			/*************************
			 *
			 * STORAGE
			 *
			 ************************/

			/* Find device */
			d_it = hal_device_store_find (hald_get_gdl (), udi_it);
			if (d_it == NULL) { 
				d_it = hal_device_store_find (hald_get_tdl (), udi_it);
				if (d_it == NULL) {
					HAL_WARNING (("Could not get device '%s' from gdl or tdl.", udi_it));
					goto error;
				}
			}

                        if (strcmp (udi_it, "/org/freedesktop/Hal/devices/computer") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
                                        if (is_md_device) {
                                                char *level;
                                                char *model_name;

                                                hal_device_property_set_string (d, "storage.bus", "linux_raid");

                                                level = hal_util_get_string_from_file (sysfs_path_real, "md/level");
                                                if (level == NULL)
                                                        goto error;
                                                hal_device_property_set_string (d, "storage.linux_raid.level", level);

                                                hal_device_property_set_string (d, "storage.linux_raid.sysfs_path", sysfs_path_real);

                                                hal_device_property_set_string (d, "storage.vendor", "Linux");
                                                if (strcmp (level, "linear") == 0) {
                                                        model_name = g_strdup ("Software RAID (Linear)");
                                                } else if (strcmp (level, "raid0") == 0) {
                                                        model_name = g_strdup ("Software RAID-0 (Stripe)");
                                                } else if (strcmp (level, "raid1") == 0) {
                                                        model_name = g_strdup ("Software RAID-1 (Mirror)");
                                                } else if (strcmp (level, "raid5") == 0) {
                                                        model_name = g_strdup ("Software RAID-5");
                                                } else {
                                                        model_name = g_strdup_printf ("Software RAID (%s)", level);
                                                }
                                                hal_device_property_set_string (d, "storage.model", model_name);
                                                g_free (model_name);

                                                hal_util_set_string_from_file (
                                                        d, "storage.firmware_version", 
                                                        sysfs_path_real, "md/metadata_version");
                                                
                                                hal_device_add_capability (d, "storage.linux_raid");

                                                if (!refresh_md_state (d))
                                                        goto error;

                                                is_hotpluggable = hal_device_property_get_bool (
                                                        d, "storage.hotpluggable");

					} else if (is_cciss_device) {
						HAL_DEBUG (("block_add: parent=/org/freedesktop/Hal/devices/computer, is_cciss_device=true"));
						hal_device_property_set_string (d, "storage.bus", "cciss");
					}
                                        break;
                        }

			/* Check info.subsystem */
			if ((bus = hal_device_property_get_string (d_it, "info.subsystem")) != NULL) {
				HAL_DEBUG(("block_add: info.subsystem='%s'", bus));

				if (strcmp (bus, "scsi") == 0) {
					scsidev = d_it;
					physdev = d_it;
					physdev_udi = udi_it;
					hal_device_property_set_string (d, "storage.bus", "scsi");
					hal_device_copy_property (scsidev, "scsi.lun", d, "storage.lun");
					/* want to continue here, because it may be USB or IEEE1394 */
				}

				if (strcmp (bus, "usb") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string (d, "storage.bus", "usb");
					break;
				} else if (strcmp (bus, "ieee1394") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string (d, "storage.bus", "ieee1394");
					break;
				} else if (strcmp (bus, "ide") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					hal_device_property_set_string (d, "storage.bus", "ide");
					/* want to continue here, because it may be pcmcia */
				} else if (strcmp (bus, "pcmcia") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string (d, "storage.bus", "pcmcia");
					break;
				} else if (strcmp (bus, "mmc") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string (d, "storage.bus", "mmc");
					break;
				} else if (strcmp (bus, "memstick") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string (d, "storage.bus", "memstick");
					break;
				} else if (strcmp (bus, "ccw") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string (d, "storage.bus", "ccw");
				} else if (strcmp (bus, "vio") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					hal_device_property_set_string (d, "storage.bus", "vio");
				} else if (strcmp (bus, "pci") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					hal_device_property_set_string (d, "storage.bus", "pci");
				}
			}

			/* Go to parent */
			udi_it = hal_device_property_get_string (d_it, "info.parent");
		}

		/* needs physical device */
		if (physdev_udi == NULL) {
			HAL_WARNING (("No physical device?"));
			goto error;
		}

		hal_device_property_set_string (d, "storage.originating_device", physdev_udi);

		if (!hal_util_get_int_from_file (sysfs_path_real, "removable", (gint *) &is_removable, 10)) {
			HAL_WARNING (("Cannot get 'removable' file"));
                        is_removable = FALSE;
		}

		hal_device_property_set_bool (d, "storage.removable.media_available", FALSE);
		hal_device_property_set_bool (d, "storage.removable", is_removable);
		/* set storage.size only if we have fixed media */
		if (!is_removable) {
			guint64 num_blocks;
			if (hal_util_get_uint64_from_file (sysfs_path_real, "size", &num_blocks, 0)) {
				/* TODO: sane to assume this is always 512 for non-removable? 
				 * I think genhd.c guarantees this... */
				hal_device_property_set_uint64 (d, "storage.size", num_blocks * 512);
			}
		} else {
			hal_device_property_set_uint64 (d, "storage.size", 0);
		}

                if (is_removable) {
                        int sysfs_capability;
                        gboolean support_an;

                        support_an = 
                                hal_util_get_int_from_file (sysfs_path, "capability", &sysfs_capability, 16) &&
                                (sysfs_capability&4) != 0;
                        
                        hal_device_property_set_bool (d, "storage.removable.support_async_notification", support_an);
                }

		/* by default, do checks for media if, and only if, the removable file is set to 1
		 *
		 * Problematic buses, like IDE, may override this.
		 */
		hal_device_property_set_bool (d, "storage.media_check_enabled", is_removable);

		parent_bus = hal_device_property_get_string (parent, "info.subsystem");
		if (parent_bus == NULL) {
			HAL_INFO (("parent_bus is NULL - wrong parent?"));
			goto error;
		}
		HAL_INFO (("parent_bus is %s", parent_bus));

		/* per-bus specific properties */
		if (strcmp (parent_bus, "ide") == 0) {
			char buf[256];
			gchar *media;
			gchar *model;
			struct stat st;

			/* Be conservative and don't poll IDE drives at all (except CD-ROM's, see below) */
			hal_device_property_set_bool (d, "storage.media_check_enabled", FALSE);

			/* according to kernel source, media can assume the following values:
			 *
			 * "disk", "cdrom", "tape", "floppy", "UNKNOWN"
			 */
			snprintf (buf, sizeof (buf), "/proc/ide/%s", hal_util_get_last_element (sysfs_path_real));
			if (stat(buf, &st)) {
				/*
				 * /proc/ide does not exist; try with sysfs
				 */
				snprintf (buf, sizeof (buf), "%s/%s", sysfs_path_real, "device");
			}
			if ((media = hal_util_get_string_from_file (buf, "media")) != NULL) {
				if (strcmp (media, "disk") == 0 ||
				    strcmp (media, "cdrom") == 0 ||
				    strcmp (media, "floppy") == 0) {
					hal_device_property_set_string (d, "storage.drive_type", media);
				} else {
					HAL_WARNING (("Cannot determine IDE drive type from file %s/media", buf));
					goto error;
				}

				if (strcmp (media, "cdrom") == 0) {
					/* only optical drives are the only IDE devices that can safely be polled */
					hal_device_property_set_bool (d, "storage.media_check_enabled", TRUE);
				}
			}

			if ((model = hal_util_get_string_from_file (buf, "model")) != NULL) {
				hal_device_property_set_string (d, "storage.model", model);
				hal_device_property_set_string (d, "info.product", model);
			}

		} else if (strcmp (parent_bus, "scsi") == 0) {
			if (strcmp (hal_device_property_get_string (parent, "scsi.type"), "unknown") == 0) {
				HAL_WARNING (("scsi.type is unknown"));
				goto error;
			}
			hal_device_copy_property (parent, "scsi.type", d, "storage.drive_type");
			hal_device_copy_property (parent, "scsi.vendor", d, "storage.vendor");
			hal_device_copy_property (parent, "scsi.model", d, "storage.model");

			hal_device_copy_property (d, "storage.vendor", d, "info.vendor");
			hal_device_copy_property (d, "storage.model", d, "info.product");

			/* Check for USB floppy drive by looking at USB Mass Storage interface class
			 * instead of Protocol: Uniform Floppy Interface (UFI) in /proc as we did before.
			 *
			 * (should fix RH bug 133834)
			 */
			if (physdev != NULL) {
				if (hal_device_property_get_int (physdev, "usb.interface.class") == 8 &&
				    hal_device_property_get_int (physdev, "usb.interface.subclass") == 4 ) {
					
					hal_device_property_set_string (d, "storage.drive_type", "floppy");
					
					/* My experiments with my USB LaCie Floppy disk
					 * drive is that polling indeed work (Yay!), so
					 * we don't set storage.media_check_enabled to 
					 * FALSE - for devices where this doesn't work,
					 * we can override it with .fdi files
					 */
				}
			}

		} else if (strcmp (parent_bus, "mmc") == 0) {
			hal_device_property_set_string (d, "storage.drive_type", "sd_mmc");
		} else if (strcmp (parent_bus, "memstick") == 0) {
			hal_device_property_set_string (d, "storage.drive_type", "memstick");
		} else if (strcmp (parent_bus, "vio") == 0) {
			char buf[256];
			const gchar *prop;

			snprintf(buf, sizeof(buf), "%s/device", sysfs_path_real);
			prop = hal_util_get_string_from_file (buf, "name");
			if (prop) {
				if (strcmp (prop, "viocd") == 0) {
					hal_device_property_set_string (d, "info.product", "Vio Virtual CD");
					hal_device_property_set_string (d, "storage.drive_type", "cdrom");
				} else if (strcmp (prop, "viodasd") == 0) {
					hal_device_property_set_string (d, "info.product", "Vio Virtual Disk");
					hal_device_property_set_string (d, "storage.drive_type", "disk");
				} else if (strcmp (prop, "viotape") == 0) {
					hal_device_property_set_string (d, "info.product", "Vio Virtual Tape");
					hal_device_property_set_string (d, "storage.drive_type", "tape");
				}
			}
		}
		else if (strcmp (parent_bus, "xen") == 0) {
			char buf[256];
			gchar *media;

			snprintf (buf, sizeof (buf), "%s/%s", sysfs_path_real, "device");
			if ((media = hal_util_get_string_from_file (buf, "media")) != NULL) {
				if (strcmp (media, "cdrom") == 0) {
					hal_device_property_set_string (d, "info.product", "Xen Virtual CD");
					hal_device_property_set_string (d, "storage.drive_type", media);
					hal_device_property_set_bool (d, "storage.media_check_enabled", FALSE);
				} else if (strcmp (media, "floppy") == 0) {
					hal_device_property_set_string (d, "info.product", "Xen Virtual Floppy");
					hal_device_property_set_string (d, "storage.drive_type", media);
					hal_device_property_set_bool (d, "storage.media_check_enabled", FALSE);
				}
				else {
					hal_device_property_set_string (d, "info.product", "Xen Virtual Disk");
					hal_device_property_set_string (d, "storage.drive_type", "disk");
				}
			}
		}

		hal_device_property_set_string (d, "info.category", "storage");
		hal_device_add_capability (d, "storage");
		hal_device_add_capability (d, "block");

		if (strcmp (hal_device_property_get_string (d, "storage.drive_type"), "cdrom") == 0) {
			hal_device_add_capability (d, "storage.cdrom");
			no_partitions_hint = TRUE;
			requires_eject = TRUE;
		}

		if (strcmp (hal_device_property_get_string (d, "storage.drive_type"), "floppy") == 0) {
			no_partitions_hint = TRUE;
		}

		hal_device_property_set_bool (d, "storage.hotpluggable", is_hotpluggable);
		hal_device_property_set_bool (d, "storage.requires_eject", requires_eject);
		hal_device_property_set_bool (d, "storage.no_partitions_hint", no_partitions_hint);

		/* add to TDL so preprobing callouts and prober can access it */
		hal_device_store_add (hald_get_tdl (), d);

		/* Process preprobe fdi files */
		di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);

		/* Run preprobe callouts */
		hal_util_callout_device_preprobe (d, blockdev_callouts_preprobing_storage_done, end_token, NULL);

	} else {
		guint sysfs_path_len;
		gboolean is_physical_partition;
		char *volume_label;
		char buf[64];

		/*************************
		 *
		 * VOLUMES
		 *
		 ************************/
		hal_device_property_set_string (d, "block.storage_device", hal_device_get_udi (parent));

		/* defaults */
		hal_device_property_set_string (d, "volume.fstype", "");
		hal_device_property_set_string (d, "volume.fsusage", "");
		hal_device_property_set_string (d, "volume.fsversion", "");
		hal_device_property_set_string (d, "volume.uuid", "");
		hal_device_property_set_string (d, "volume.label", "");
		hal_device_property_set_string (d, "volume.mount_point", "");

		/* persistent properties from udev (may be empty) */
                hal_device_property_set_string (d, "volume.fsusage", hotplug_event->sysfs.fsusage);
                hal_device_property_set_string (d, "volume.fsversion", hotplug_event->sysfs.fsversion);
                hal_device_property_set_string (d, "volume.uuid", hotplug_event->sysfs.fsuuid);
                hal_device_property_set_string (d, "volume.fstype", hotplug_event->sysfs.fstype);
		if (hotplug_event->sysfs.fstype[0] != '\0') {
			snprintf (buf, sizeof (buf), "Volume (%s)", hotplug_event->sysfs.fstype);
		} else {
			snprintf (buf, sizeof (buf), "Volume");
		}
		hal_device_property_set_string (d, "info.product", buf);

		volume_label = strdup_valid_utf8 (hotplug_event->sysfs.fslabel);
		if (volume_label) {
	                hal_device_property_set_string (d, "volume.label", volume_label);
			if (volume_label[0] != '\0') {
	                	hal_device_property_set_string (d, "info.product", volume_label);
			}
			g_free(volume_label);
		}

		hal_device_property_set_bool (d, "volume.is_mounted", FALSE);
		hal_device_property_set_bool (d, "volume.is_mounted_read_only", FALSE);
		hal_device_property_set_bool (d, "volume.linux.is_device_mapper", is_device_mapper);
		/* Don't assume that the parent has storage capability, eg
		 * if we are an MD partition then this is the case as we were
		 * re-parented to the root computer device object earlier.
		 */
		if (hal_device_has_property(parent, "storage.drive_type")) {
			hal_device_property_set_bool (d, "volume.is_disc", strcmp (hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0);
		} else {
			hal_device_property_set_bool (d, "volume.is_disc", FALSE);
		}

		is_physical_partition = TRUE;
		if (is_fakevolume || is_device_mapper)
			is_physical_partition = FALSE;

		hal_device_property_set_bool (d, "volume.is_partition", is_physical_partition);

		hal_device_property_set_string (d, "info.category", "volume");
		if (hal_device_has_property(parent, "storage.drive_type")) {
			if (strcmp(hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0) {
				hal_device_add_capability (d, "volume.disc");
			}
		}
		hal_device_add_capability (d, "volume");
		hal_device_add_capability (d, "block");

		/* determine partition number */
		sysfs_path_len = strlen (sysfs_path);
		if (is_physical_partition) {
			if (sysfs_path_len > 0 && isdigit (sysfs_path[sysfs_path_len - 1])) {
				guint i;
				for (i = sysfs_path_len - 1; isdigit (sysfs_path[i]); --i)
					;
				if (isdigit (sysfs_path[i+1])) {
					guint partition_number;
					partition_number = atoi (&sysfs_path[i+1]);
					hal_device_property_set_int (d, "volume.partition.number", partition_number);
				} else {
					HAL_WARNING (("Cannot determine partition number?"));
					goto error;
				}
			} else {
				HAL_WARNING (("Cannot determine partition number"));
				goto error;
			}
		}

		/* first estimate - prober may override this...
		 *
		 * (block size requires opening the device file)
		 */
		hal_device_property_set_int (d, "volume.block_size", 512);
		if (!hal_util_set_uint64_from_file (d, "volume.num_blocks", sysfs_path_real, "size", 0)) {
			HAL_INFO (("Ignoring hotplug event - cannot read 'size'"));
			goto error;
		}
		hal_device_property_set_uint64 (
			d, "volume.size",
			((dbus_uint64_t)(512)) * ((dbus_uint64_t)(hal_device_property_get_uint64 (d, "volume.num_blocks"))));
		/* TODO: move to prober? */
		if (is_physical_partition) {
			guint64 start_block;
			guint64 parent_size;
			if (hal_util_get_uint64_from_file (sysfs_path, "start", &start_block, 0)) {
				hal_device_property_set_uint64 (d, "volume.partition.start", start_block * 512);
			}
			if (hal_util_get_uint64_from_file (sysfs_path, "../size", &parent_size, 0)) {
				hal_device_property_set_uint64 (d, "volume.partition.media_size", parent_size * 512);
			}
		}


		/* add to TDL so preprobing callouts and prober can access it */
		hal_device_store_add (hald_get_tdl (), d);

		/* Process preprobe fdi files */
		di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);

		/* Run preprobe callouts */
		hal_util_callout_device_preprobe (d, blockdev_callouts_preprobing_volume_done, end_token, NULL);
	}
out2:
	g_free (sysfs_path_real);
	return;

error:
	HAL_WARNING (("Not adding device object"));
	if (d != NULL)
		g_object_unref (d);
out:
        hotplug_event_end (end_token);
	g_free (sysfs_path_real);
        return;
}


static void
force_unmount_cb (HalDevice *d, guint32 exit_type, 
		  gint return_code, gchar **error,
		  gpointer data1, gpointer data2)
{
	void *end_token = (void *) data1;

	HAL_INFO (("force_unmount_cb for udi='%s', exit_type=%d, return_code=%d", hal_device_get_udi (d), exit_type, return_code));

	if (exit_type == HALD_RUN_SUCCESS && error != NULL && 
	    error[0] != NULL && error[1] != NULL) {
		char *exp_name = NULL;
		char *exp_detail = NULL;

		exp_name = error[0];
		if (error[0] != NULL) {
			exp_detail = error[1];
		}
		HAL_INFO (("failed with '%s' '%s'", exp_name, exp_detail));
	}

	hal_util_callout_device_remove (d, blockdev_callouts_remove_done, end_token, NULL);

}

static gboolean
force_unmount (HalDevice *d, void *end_token)
{
	const char *device_file;
	const char *mount_point;

	device_file = hal_device_property_get_string (d, "block.device");
	mount_point = hal_device_property_get_string (d, "volume.mount_point");

	/* look up in /media/.hal-mtab to see if we mounted this one */
	if (mount_point != NULL && strlen (mount_point) > 0 && hal_util_is_mounted_by_hald (mount_point)) {
		char *unmount_stdin;
		char *extra_env[2];

		extra_env[0] = "HAL_METHOD_INVOKED_BY_UID=0";
		extra_env[1] = NULL;
		
		HAL_INFO (("force_unmount for udi='%s'", hal_device_get_udi (d)));
		syslog (LOG_NOTICE, "forcibly attempting to lazy unmount %s as enclosing drive was disconnected", device_file);
		
		unmount_stdin = "lazy\n";
		
		/* so, yea, calling the Unmount methods handler.. is cheating a bit :-) */
		hald_runner_run_method (d, 
					"hal-storage-unmount", 
					extra_env, 
					unmount_stdin, TRUE,
					0,
					force_unmount_cb,
					end_token, NULL);
		return TRUE;
	} else {
		return FALSE;
	}
}

void
hotplug_event_begin_remove_blockdev (const gchar *sysfs_path, void *end_token)
{
	HalDevice *d;

	HAL_INFO (("block_rem: sysfs_path=%s", sysfs_path));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Device is not in the HAL database"));
		hotplug_event_end (end_token);
	} else {
		HalDevice *fakevolume;
		char fake_sysfs_path[HAL_PATH_MAX];

		/* if we're a storage device synthesize hotplug rem event 
		 * for the one potential fakevolume we've got 
		 */
		snprintf (fake_sysfs_path, sizeof(fake_sysfs_path), "%s/fakevolume", sysfs_path);
		fakevolume = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", fake_sysfs_path);
		if (fakevolume != NULL) {
			HotplugEvent *hotplug_event;
			HAL_INFO (("Storage device with a fakevolume is going away; "
				   "synthesizing hotplug rem for fakevolume %s", hal_device_get_udi (fakevolume)));
			hotplug_event = blockdev_generate_remove_hotplug_event (fakevolume);
			if (hotplug_event != NULL) {
				/* push synthesized event at front of queue and repost this event... this is such that
				 * the fakevolume event is processed before this one... because if we didn't make the
				 * events to be processed in this order, the "lazy unmount" of the fakevolume would
				 * fail...
				 */
				hotplug_event_enqueue_at_front ((HotplugEvent *) end_token);
				hotplug_event_enqueue_at_front (hotplug_event);
				hotplug_event_reposted (end_token);
				goto out;
			}

		}
		
		/* if we're mounted, then do a lazy unmount so the system can gracefully recover */
		if (hal_device_property_get_bool (d, "volume.is_mounted")) {
			if (!force_unmount (d, end_token)) {
				/* this wasn't mounted by us... carry on */
				HAL_INFO (("device at sysfs_path %s is mounted, but not by HAL", sysfs_path));
				hal_util_callout_device_remove (d, blockdev_callouts_remove_done, end_token, NULL);
			}
		} else {
			HAL_INFO (("device at sysfs_path %s is not mounted", sysfs_path));
			hal_util_callout_device_remove (d, blockdev_callouts_remove_done, end_token, NULL);
		}
	}
out:
	;
}

void 
hotplug_event_refresh_blockdev (gchar *sysfs_path, HalDevice *d, void *end_token)
{
	HAL_INFO (("block_change: sysfs_path=%s", sysfs_path));

        if (hal_device_property_get_bool (d, "storage.removable.support_async_notification")) {
                blockdev_rescan_device (d);
        }

	/* done with change event */
	hotplug_event_end (end_token);
}


static void 
block_rescan_storage_done (HalDevice *d, guint32 exit_type, 
                           gint return_code, gchar **error,
                           gpointer data1, gpointer data2)
{
	const char *sysfs_path;
	HalDevice *fakevolume;
	char fake_sysfs_path[HAL_PATH_MAX];

	HAL_INFO (("hald-probe-storage --only-check-for-media returned %d (exit_type=%d)", return_code, exit_type));

	if (d == NULL) {
		HAL_INFO (("Device object already removed"));
		goto out;
	}

	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	/* see if we already got a fake volume */
	snprintf (fake_sysfs_path, sizeof(fake_sysfs_path), "%s/fakevolume", sysfs_path);
	fakevolume = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", fake_sysfs_path);

	if (return_code == 2) {
		/* we've got something on the main block device - add fakevolume if we haven't got one already */
		if (fakevolume == NULL) {
			HAL_INFO (("Media insertion detected with file system on main block device; synthesizing hotplug add"));
			generate_fakevolume_hotplug_event_add_for_storage_device (d);
		}
	} else {
		/* no fs on the main block device - remove fakevolume if we have one */
		if (fakevolume != NULL) {
			/* generate hotplug event to remove the fakevolume */
			HotplugEvent *hotplug_event;
			HAL_INFO (("Media removal detected; synthesizing hotplug rem for fakevolume %s", 
				   hal_device_get_udi (fakevolume)));
			hotplug_event = blockdev_generate_remove_hotplug_event (fakevolume);
			if (hotplug_event != NULL) {
				hotplug_event_enqueue (hotplug_event);
			}
		}
	}

out:
	;
}

gboolean
blockdev_rescan_device (HalDevice *d)
{
	gboolean ret;

	ret = FALSE;

	HAL_INFO (("blockdev_rescan_device: udi=%s", hal_device_get_udi (d)));

	/* This only makes sense on storage devices */
	if (hal_device_property_get_bool (d, "block.is_volume")) {
		HAL_INFO (("No action on volumes", hal_device_get_udi (d)));
		goto out;
	}

	/* now see if we got a file system on the main block device */
	hald_runner_run (d,
	                 "hald-probe-storage --only-check-for-media", NULL, 
	                 HAL_HELPER_TIMEOUT,
	                 block_rescan_storage_done,
	                 NULL, NULL);
	ret = TRUE;

out:
	return ret;
}


HotplugEvent *
blockdev_generate_add_hotplug_event (HalDevice *d)
{
	const char *sysfs_path;
	const char *device_file;
	const char *model;
	const char *vendor;
	const char *serial;
	const char *revision;
	HotplugEvent *hotplug_event;
	const char *nul;

	nul = "\0";

	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	device_file = hal_device_property_get_string (d, "block.device");
	model       = hal_device_property_get_string (d, "storage.model");
	vendor      = hal_device_property_get_string (d, "storage.vendor");
	serial      = hal_device_property_get_string (d, "storage.serial");
	revision    = hal_device_property_get_string (d, "storage.firmware_revision");

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));

	g_strlcpy (hotplug_event->sysfs.device_file, device_file != NULL ? device_file : nul, HAL_NAME_MAX);
	g_strlcpy (hotplug_event->sysfs.vendor,           vendor != NULL ?      vendor : nul, HAL_NAME_MAX);
	g_strlcpy (hotplug_event->sysfs.model,             model != NULL ?       model : nul, HAL_NAME_MAX);
	g_strlcpy (hotplug_event->sysfs.serial,           serial != NULL ?      serial : nul, HAL_NAME_MAX);
	g_strlcpy (hotplug_event->sysfs.revision,       revision != NULL ?    revision : nul, HAL_NAME_MAX);

	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}

HotplugEvent *
blockdev_generate_remove_hotplug_event (HalDevice *d)
{
	const char *sysfs_path;
	HotplugEvent *hotplug_event;

	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}

static GSList *md_devs = NULL;

static char *
udev_get_device_file_for_sysfs_path (const char *sysfs_path)
{
        char *ret;
        char *u_stdout;
        int u_exit_status;
        const char *argv[] = {"/sbin/udevadm", "info", "--root", "--query", "name", "--path", NULL, NULL};
        GError *g_error;

        ret = NULL;
        argv[6] = sysfs_path;

        g_error = NULL;

        if (!g_spawn_sync("/", 
                          (char **) argv, 
                          NULL,           			/* envp */
                          G_SPAWN_LEAVE_DESCRIPTORS_OPEN, 	/* flags */
                          NULL,           			/* child_setup */
                          NULL,           			/* user_data */
                          &u_stdout,
                          NULL,           			/* stderr */
                          &u_exit_status,
                          &g_error)) {
                HAL_ERROR (("Error spawning udevinfo: %s", g_error->message));
                g_error_free (g_error);
                goto out;
        }

        if (u_exit_status != 0) {
                HAL_ERROR (("udevinfo returned exit code %d", u_exit_status));
                g_free (u_stdout);
                goto out;
        }

        ret = u_stdout;
        g_strchomp (ret);
        HAL_INFO (("Got '%s'", ret));

out:
        return ret;
}


void 
blockdev_process_mdstat (void)
{
	HotplugEvent *hotplug_event;
        GIOChannel *channel;
        GSList *read_md_devs;
        GSList *i;
        GSList *j;
        GSList *k;

        channel = get_mdstat_channel ();
        if (channel == NULL)
                goto error;

        if (g_io_channel_seek (channel, 0, G_SEEK_SET) != G_IO_ERROR_NONE) {
                HAL_ERROR (("Cannot seek in /proc/mdstat"));
                goto error;
        }

        read_md_devs = NULL;
        while (TRUE) {
                int num;
                char *line;

                if (g_io_channel_read_line (channel, &line, NULL, NULL, NULL) != G_IO_STATUS_NORMAL)
                        break;

                if (sscanf (line, "md%d : ", &num) == 1) {
                        char *sysfs_path;
                        sysfs_path = g_strdup_printf ("/sys/block/md%d", num);
                        read_md_devs = g_slist_prepend (read_md_devs, sysfs_path);
                }

                g_free (line);
        }

        /* now compute the delta */

        /* add devices */
        for (i = read_md_devs; i != NULL; i = i->next) {
                gboolean should_add = TRUE;

                for (j = md_devs; j != NULL && should_add; j = j->next) {
                        if (strcmp (i->data, j->data) == 0) {
                                should_add = FALSE;
                        }
                }

                if (should_add) {
                        char *sysfs_path = i->data;
                        char *device_file;
                        int num_tries;

                        num_tries = 0;
                retry_add:
                        device_file = udev_get_device_file_for_sysfs_path (sysfs_path);
                        if (device_file == NULL) {
                                if (num_tries <= 6) {
                                        int num_ms;
                                        num_ms = 10 * (1<<num_tries);
                                        HAL_INFO (("spinning %d ms waiting for device file for sysfs path %s", 
                                                   num_ms, sysfs_path));
                                        usleep (1000 * num_ms);
                                        num_tries++;
                                        goto retry_add;
                                } else {
                                        HAL_ERROR (("Cannot get device file for sysfs path %s", sysfs_path));
                                }
                        } else {
                                HAL_INFO (("Adding md device at '%s' ('%s')", sysfs_path, device_file));

                                hotplug_event = g_slice_new0 (HotplugEvent);
                                hotplug_event->action = HOTPLUG_ACTION_ADD;
                                hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;
                                g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
                                g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
                                g_strlcpy (hotplug_event->sysfs.device_file, device_file, sizeof (hotplug_event->sysfs.device_file));
                                hotplug_event->sysfs.net_ifindex = -1;
                                hotplug_event_enqueue (hotplug_event);
                                
                                md_devs = g_slist_prepend (md_devs, g_strdup (sysfs_path));

                                g_free (device_file);
                        }

                }
        }

        /* remove devices */
        for (i = md_devs; i != NULL; i = k) {
                gboolean should_remove = TRUE;

                k = i->next;

                for (j = read_md_devs; j != NULL && should_remove; j = j->next) {
                        if (strcmp (i->data, j->data) == 0) {
                                should_remove = FALSE;
                        }
                }

                if (should_remove) {
                        char *sysfs_path = i->data;
                        char *device_file;
                        int num_tries;
                        
                        num_tries = 0;
                retry_rem:
                        device_file = udev_get_device_file_for_sysfs_path (sysfs_path);
                        if (device_file == NULL) {
                                if (num_tries <= 6) {
                                        int num_ms;
                                        num_ms = 10 * (1<<num_tries);
                                        HAL_INFO (("spinning %d ms waiting for device file for sysfs path %s", 
                                                   num_ms, sysfs_path));
                                        usleep (1000 * num_ms);
                                        num_tries++;
                                        goto retry_rem;
                                } else {
                                        HAL_ERROR (("Cannot get device file for sysfs path %s", sysfs_path));
                                }
                        } else {
                                
                                HAL_INFO (("Removing md device at '%s' ('%s')", sysfs_path, device_file));

                                hotplug_event = g_slice_new0 (HotplugEvent);
                                hotplug_event->action = HOTPLUG_ACTION_REMOVE;
                                hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;
                                g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
                                g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
                                g_strlcpy (hotplug_event->sysfs.device_file, device_file, sizeof (hotplug_event->sysfs.device_file));
                                hotplug_event->sysfs.net_ifindex = -1;
                                hotplug_event_enqueue (hotplug_event);
                                
                                md_devs = g_slist_remove_link (md_devs, i);
                                g_free (i->data);
                                g_slist_free (i);

                                g_free (device_file);
                        }

                }
        }

        g_slist_foreach (read_md_devs, (GFunc) g_free, NULL);
        g_slist_free (read_md_devs);

        /* finally, refresh all md devices */
        for (i = md_devs; i != NULL; i = i->next) {
                char *sysfs_path = i->data;
                HalDevice *d;

                d = hal_device_store_match_key_value_string (hald_get_gdl (), 
                                                             "storage.linux_raid.sysfs_path", 
                                                             sysfs_path);
                if (d == NULL)
                        d = hal_device_store_match_key_value_string (hald_get_tdl (), 
                                                                     "storage.linux_raid.sysfs_path", 
                                                                     sysfs_path);
                if (d != NULL)
                        refresh_md_state (d);
        }
        

        hotplug_event_process_queue ();

error:
        ;
}
