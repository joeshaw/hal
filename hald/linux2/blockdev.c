/***************************************************************************
 * CVSID: $Id$
 *
 * blockdev.c : Handling of block devices
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005,2006 Kay Sievers, <kay.sievers@vrfy.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <ctype.h>
#include <unistd.h>
#include <linux/kdev_t.h>

#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <syslog.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../device_info.h"
#include "../hald_dbus.h"
#include "../util.h"
#include "../hald_runner.h"

#include "osspec_linux.h"

#include "coldplug.h"
#include "hotplug_helper.h"

#include "hotplug.h"
#include "blockdev.h"

/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
blockdev_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_property_get_bool (d, "block.is_volume")) {
		const char *label;
		const char *uuid;

		label = hal_device_property_get_string (d, "volume.label");
		uuid = hal_device_property_get_string (d, "volume.uuid");

		if (uuid != NULL && strlen (uuid) > 0) {
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
					      "/org/freedesktop/Hal/devices/volume_uuid_%s", uuid);
		} else if (label != NULL && strlen (label) > 0) {
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
					      "/org/freedesktop/Hal/devices/volume_label_%s", label);
		} else if (hal_device_property_get_bool(d, "volume.is_disc") &&
			   hal_device_property_get_bool(d, "volume.disc.is_blank")) {
			/* this should be a empty CD/DVD */
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
                                             "/org/freedesktop/Hal/devices/volume_empty_%s",
					      hal_device_property_get_string (d, "volume.disc.type"));
		} else {
			/* fallback to partition number, size */
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
					      "/org/freedesktop/Hal/devices/volume_part%d_size_%lld", 
					      hal_device_property_get_int (d, "volume.partition.number"),
					      hal_device_property_get_uint64 (d, "volume.size"));
		}
	} else {
		const char *model;
		const char *serial;

		model = hal_device_property_get_string (d, "storage.model");
		serial = hal_device_property_get_string (d, "storage.serial");

		if (serial != NULL) {
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
					      "/org/freedesktop/Hal/devices/storage_serial_%s", 
					      serial);
		} else if ((model != NULL) && (strlen(model) != 0) ) {
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
					      "/org/freedesktop/Hal/devices/storage_model_%s", 
					      model);
		} else {
			hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
					      "%s_storage", 
					      hal_device_property_get_string (d, "storage.physical_device"));
		}
	}

	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}


static void 
blockdev_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", d->udi));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
blockdev_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Remove callouts completed udi=%s", d->udi));

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

static gboolean
is_mounted_by_hald (const char *mount_point)
{
	int i;
	FILE *hal_mtab;
	int hal_mtab_len;
	int num_read;
	char *hal_mtab_buf;
	char **lines;
	gboolean found;
	int lock_mtab_fd;

	hal_mtab = NULL;
	hal_mtab_buf = NULL;
	found = FALSE;

	/* take the lock on /media/.hal-mtab-lock so we don't race with the Mount() and Unmount() methods */
	
	/* do not attempt to create the file; tools/hal-storage-shared.c will create it and 
	 * set the correct ownership so this unprivileged process (running as haldaemon) can
	 * lock it too
	 */
	lock_mtab_fd = open ("/media/.hal-mtab-lock", 0);
	if (lock_mtab_fd < 0) {
		HAL_INFO (("Cannot open /media/.hal-mtab for locking"));
		goto out;
	}

tryagain:
	if (flock (lock_mtab_fd, LOCK_EX) != 0) {
		if (errno == EINTR)
			goto tryagain;
		HAL_ERROR (("Cannot obtain lock on /media/.hal-mtab"));
		goto out;
	}

	/*HAL_DEBUG (("examining /media/.hal-mtab for %s", mount_point));*/

	hal_mtab = fopen ("/media/.hal-mtab", "r");
	if (hal_mtab == NULL) {
		HAL_ERROR (("Cannot open /media/.hal-mtab"));
		goto out;
	}
	if (fseek (hal_mtab, 0L, SEEK_END) != 0) {
		HAL_ERROR (("Cannot seek to end of /media/.hal-mtab"));
		goto out;
	}
	hal_mtab_len = ftell (hal_mtab);
	if (hal_mtab_len < 0) {
		HAL_ERROR (("Cannot determine size of /media/.hal-mtab"));
		goto out;
	}
	rewind (hal_mtab);

	hal_mtab_buf = g_new0 (char, hal_mtab_len + 1);
	num_read = fread (hal_mtab_buf, 1, hal_mtab_len, hal_mtab);
	if (num_read != hal_mtab_len) {
		HAL_ERROR (("Cannot read from /media/.hal-mtab"));
		goto out;
	}
	fclose (hal_mtab);
	hal_mtab = NULL;

	/*HAL_DEBUG (("hal_mtab = '%s'\n", hal_mtab_buf));*/

	lines = g_strsplit (hal_mtab_buf, "\n", 0);
	g_free (hal_mtab_buf);
	hal_mtab_buf = NULL;

	/* find the entry we're going to unmount */
	for (i = 0; lines[i] != NULL && !found; i++) {
		char **line_elements;

		/*HAL_DEBUG ((" line = '%s'", lines[i]));*/

		if ((lines[i])[0] == '#')
			continue;

		line_elements = g_strsplit (lines[i], "\t", 6);
		if (g_strv_length (line_elements) == 6) {
/*
			HAL_DEBUG (("  devfile     = '%s'", line_elements[0]));
			HAL_DEBUG (("  uid         = '%s'", line_elements[1]));
			HAL_DEBUG (("  session id  = '%s'", line_elements[2]));
			HAL_DEBUG (("  fs          = '%s'", line_elements[3]));
			HAL_DEBUG (("  options     = '%s'", line_elements[4]));
			HAL_DEBUG (("  mount_point = '%s'", line_elements[5]));
			HAL_DEBUG (("  (comparing against '%s')", mount_point));
*/

			if (strcmp (line_elements[5], mount_point) == 0) {
				found = TRUE;
				/*HAL_INFO (("device at '%s' is indeed mounted by HAL's Mount()", mount_point));*/
			}
			
		}

		g_strfreev (line_elements);
	}

out:
	if (lock_mtab_fd >= 0)
		close (lock_mtab_fd);
	if (hal_mtab != NULL)
		fclose (hal_mtab);
	if (hal_mtab_buf != NULL)
		g_free (hal_mtab_buf);

	return found;
}

void
blockdev_refresh_mount_state (HalDevice *d)
{
	FILE *f;
	struct mntent mnt;
	struct mntent *mnte;
	char buf[1024];
	unsigned int major;
	unsigned int minor;
	dev_t devt = makedev(0, 0);
	GSList *volumes = NULL;
	GSList *volume;
        GSList *autofs_mounts = NULL;

	/* open /proc/mounts */
	g_snprintf (buf, sizeof (buf), "%s/mounts", get_hal_proc_path ());
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
	while ((mnte = getmntent_r (f, &mnt, buf, sizeof(buf))) != NULL) {
		struct stat statbuf;

		/* If this is a nfs mount (fstype == 'nfs') ignore the mount. Reason:
		 *  1. we don't list nfs devices in HAL
		 *  2. more problematic: stat on mountpoints with 'stale nfs handle' never come
		 *     back and block complete HAL and all applications using HAL fail.
		 */
		if (strcmp(mnt.mnt_type, "nfs") == 0)
			continue;

		/* If this is an autofs mount (fstype == 'autofs') 
		 * store the mount in a list for later use. 
		 * On mounts managed by autofs accessing files below the mount
		 * point cause the mount point to be remounted after an 
		 * unmount.  We keep the list so we do not check for
		 * the .created-by-hal file on mounts under autofs mount points
		 */
		if (strcmp(mnt.mnt_type, "autofs") == 0) {
			char *mnt_dir;

			if (mnt.mnt_dir[strlen (mnt.mnt_dir) - 1] != '/')
				mnt_dir = g_strdup_printf ("%s/", mnt.mnt_dir);
			else
				mnt_dir = g_strdup (mnt.mnt_dir);

			autofs_mounts = g_slist_append (autofs_mounts,
							mnt_dir);


			continue;
		}

		/* check the underlying device of the mount point */
		if (stat (mnt.mnt_dir, &statbuf) != 0)
			continue;
		if (major(statbuf.st_dev) == 0)
			continue;

		/*HAL_INFO (("* found mounts dev %s (%i:%i)", mnt.mnt_fsname, major(statbuf.st_dev), minor(statbuf.st_dev)));*/
		/* match against all hal volumes */
		for (volume = volumes; volume != NULL; volume = g_slist_next (volume)) {
			HalDevice *dev;

			dev = HAL_DEVICE (volume->data);
			major = hal_device_property_get_int (dev, "block.major");
			if (major == 0)
				continue;
			minor = hal_device_property_get_int (dev, "block.minor");
			devt = makedev(major, minor);
			/*HAL_INFO (("  match %s (%i:%i)", hal_device_get_udi (dev), major, minor));*/

			if (statbuf.st_dev == devt) {
				/* found entry for this device in /proc/mounts */
				device_property_atomic_update_begin ();
				hal_device_property_set_bool (dev, "volume.is_mounted", TRUE);
				hal_device_property_set_bool (dev, "volume.is_mounted_read_only",
							      hasmntopt (&mnt, MNTOPT_RO) ? TRUE : FALSE);
				hal_device_property_set_string (dev, "volume.mount_point", mnt.mnt_dir);
				device_property_atomic_update_end ();
				/*HAL_INFO (("  set %s to be mounted at %s (%s)",
					   hal_device_get_udi (dev), mnt.mnt_dir,
					   hasmntopt (&mnt, MNTOPT_RO) ? "ro" : "rw"));*/
				volumes = g_slist_delete_link (volumes, volume);
				break;
			}
		}
	}

	/* all remaining volumes are not mounted */
	for (volume = volumes; volume != NULL; volume = g_slist_next (volume)) {
		HalDevice *dev;
		char *mount_point;
		GSList *autofs_node;

		dev = HAL_DEVICE (volume->data);
		mount_point = g_strdup (hal_device_property_get_string (dev, "volume.mount_point"));
		device_property_atomic_update_begin ();
		hal_device_property_set_bool (dev, "volume.is_mounted", FALSE);
		hal_device_property_set_bool (dev, "volume.is_mounted_read_only", FALSE);
		hal_device_property_set_string (dev, "volume.mount_point", "");
		device_property_atomic_update_end ();
		/*HAL_INFO (("set %s to unmounted", hal_device_get_udi (dev)));*/

		/* check to see if mount point falls under autofs */
		autofs_node = autofs_mounts;
		while (autofs_node != NULL) {
			char *am = (char *)autofs_node->data;

			if (strncmp (am, mount_point, strlen (am)) == 0);
				break;

			autofs_node = autofs_node->next;
		}

		/* look up in /media/.hal-mtab to see if we mounted this one */
		if (mount_point != NULL && strlen (mount_point) > 0 && is_mounted_by_hald (mount_point)) {
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
	g_slist_free (volumes);
	g_slist_foreach (autofs_mounts, (GFunc) g_free, NULL);
	g_slist_free (autofs_mounts);
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

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, fake_sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	if (device_file != NULL)
		g_strlcpy (hotplug_event->sysfs.device_file, device_file, sizeof (hotplug_event->sysfs.device_file));
	else
		hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	hotplug_event_enqueue (hotplug_event);
	hotplug_event_process_queue ();
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

static const gchar *
blockdev_get_luks_uuid(const gchar *device_file)
{
	const gchar *luks_uuid = NULL;
	unsigned int major;
	unsigned int minor;
	const char *last_elem;

	HAL_INFO (("get_luks_uuid: device_file=%s", device_file));

	major = 253; /* FIXME: replace by devmapper constant */
	last_elem = hal_util_get_last_element (device_file);
	if (sscanf (last_elem, "dm-%d", &minor) == 1) {
		GDir *dir;
		HAL_INFO (("path=%s is a device mapper dev, major/minor=%d/%d", device_file, major, minor));
		/* Ugly hack to see if we're a LUKS crypto device; should
		* be replaced by some ioctl or libdevmapper stuff by where
		* we can ask about the name for /dev/dm-0; as e.g. given by
		* 'dmsetup info'
		*
		* Our assumption is that luks-setup have invoked
		* dmsetup; e.g. the naming convention is 
		*
		*    luks_crypto_<luks_uuid>
		*
		* where <luks_uuid> is the UUID encoded in the luks
		* metadata.
		*/
		/* Ugly sleep of 0.5s here as well to allow dmsetup to do the mknod */
		if (!hald_is_initialising)
			usleep (1000 * 1000 * 5 / 10);
		if ((dir = g_dir_open ("/dev/mapper", 0, NULL)) != NULL) {
			const gchar *f;
			char devpath[256];
			struct stat statbuf;
			while ((f = g_dir_read_name (dir)) != NULL) {
				char luks_prefix[] = "luks_crypto_";
				g_snprintf (devpath, sizeof (devpath), "/dev/mapper/%s", f);
				if (stat (devpath, &statbuf) == 0) {
					HAL_INFO (("looking at /dev/mapper/%s with %d:%d", 
						   f, MAJOR(statbuf.st_rdev), MINOR(statbuf.st_rdev)));
					if (S_ISBLK (statbuf.st_mode) && 
					    MAJOR(statbuf.st_rdev) == major && 
					    MINOR(statbuf.st_rdev) == minor &&
					    strncmp (f, luks_prefix, sizeof (luks_prefix) - 1) == 0) {
						luks_uuid = f + sizeof (luks_prefix) - 1;
						HAL_INFO (("found %s; luks_uuid='%s'!", devpath, luks_uuid));
						break;
					}
				}
			}
			g_dir_close (dir);
		}
	}
	return luks_uuid;
}

static HalDevice *
blockdev_get_luks_parent (const gchar *luks_uuid, HalDevice *device)
{
	HalDevice *parent = NULL;
	HalDevice *backing_volume;

	HAL_INFO (("get_luks_parent: luks_uuid=%s device=0x%08x", 
		   luks_uuid, device));

	backing_volume = hal_device_store_match_key_value_string (hald_get_gdl (),
								  "volume.uuid", 
								  luks_uuid);
	if (backing_volume != NULL) {
		const char *backing_volume_stordev_udi;
		HAL_INFO (("backing_volume udi='%s'!", backing_volume->udi));
		backing_volume_stordev_udi = hal_device_property_get_string (backing_volume, "block.storage_device");
		if (backing_volume_stordev_udi != NULL) {
			HAL_INFO (("backing_volume_stordev_udi='%s'!", backing_volume_stordev_udi));
			parent = hal_device_store_find (hald_get_gdl (), backing_volume_stordev_udi);
			if (parent != NULL) {
				HAL_INFO (("parent='%s'!", parent->udi));
				hal_device_property_set_string (device, "volume.crypto_luks.clear.backing_volume", backing_volume->udi);
			}
		}
	}
	return parent;
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

	HAL_INFO (("block_add: sysfs_path=%s dev=%s is_part=%d, parent=0x%08x", 
		   sysfs_path, device_file, is_partition, parent));

	if (parent != NULL && hal_device_property_get_bool (parent, "info.ignore")) {
		HAL_INFO (("Ignoring block_add since parent has info.ignore==TRUE"));
		goto out;
	}

	if (strcmp (hal_util_get_last_element (sysfs_path), "fakevolume") == 0) {
		is_fakevolume = TRUE;
		sysfs_path_real = hal_util_get_parent_path (sysfs_path);
		HAL_INFO (("Handling %s as fakevolume - sysfs_path_real=%s", device_file, sysfs_path_real));
	} else {
		is_fakevolume = FALSE;
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

	if (parent == NULL) {
		const gchar *luks_uuid = blockdev_get_luks_uuid (device_file);
		if (luks_uuid != NULL) {
			is_partition = TRUE;
			parent = blockdev_get_luks_parent (luks_uuid, d);
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
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.parent", parent->udi);
	hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_SYSFS_BLOCK);

	hal_device_property_set_string (d, "block.device", device_file);
	if ((major_minor = hal_util_get_string_from_file (sysfs_path_real, "dev")) == NULL || 
	    sscanf (major_minor, "%d:%d", &major, &minor) != 2) {
		HAL_INFO (("Ignoring hotplug event - cannot read major:minor"));
		goto error;
	}

	hal_device_property_set_int (d, "block.major", major);
	hal_device_property_set_int (d, "block.minor", minor);
	hal_device_property_set_bool (d, "block.is_volume", is_partition);

	if (hal_device_has_property(parent, "info.bus") &&
		(strcmp(hal_device_property_get_string(parent, "info.bus"), "platform") == 0) &&
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
		hal_device_property_set_string (d, "storage.physical_device", parent->udi);
		hal_device_property_set_bool (d, "storage.removable", TRUE);
		hal_device_property_set_bool (d, "storage.hotpluggable", FALSE);
		hal_device_property_set_bool (d, "storage.requires_eject", FALSE);

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

	if (!is_partition) {
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
		udi_it = parent->udi;
		while (udi_it != NULL) {
			HalDevice *d_it;

			/*************************
			 *
			 * STORAGE
			 *
			 ************************/

			/* Find device */
			d_it = hal_device_store_find (hald_get_gdl (), udi_it);
			g_assert (d_it != NULL);

			/* Check info.bus */
			if ((bus = hal_device_property_get_string (d_it, "info.bus")) != NULL) {
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
				} else if (strcmp (bus, "ccw") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					is_hotpluggable = TRUE;
					hal_device_property_set_string
						(d, "storage.bus", "ccw");
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

		hal_device_property_set_string (d, "storage.physical_device", physdev_udi);

		if (!hal_util_get_int_from_file (sysfs_path, "removable", (gint *) &is_removable, 10)) {
			HAL_WARNING (("Cannot get 'removable' file"));
			goto error;
		}

		hal_device_property_set_bool (d, "storage.removable", is_removable);

		/* by default, do checks for media if, and only if, the removable file is set to 1
		 *
		 * Problematic buses, like IDE, may override this.
		 */
		hal_device_property_set_bool (d, "storage.media_check_enabled", is_removable);

		parent_bus = hal_device_property_get_string (parent, "info.bus");
		HAL_INFO (("parent_bus is %s", parent_bus));

		/* per-bus specific properties */
		if (strcmp (parent_bus, "ide") == 0) {
			char buf[256];
			gchar *media;
			gchar *model;

			/* Be conservative and don't poll IDE drives at all (except CD-ROM's, see below) */
			hal_device_property_set_bool (d, "storage.media_check_enabled", FALSE);

			/* according to kernel source, media can assume the following values:
			 *
			 * "disk", "cdrom", "tape", "floppy", "UNKNOWN"
			 */
			snprintf (buf, sizeof (buf), "%s/ide/%s", get_hal_proc_path (), hal_util_get_last_element (sysfs_path));
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

		/*************************
		 *
		 * VOLUMES
		 *
		 ************************/
		hal_device_property_set_string (d, "block.storage_device", parent->udi);

		/* defaults */
		hal_device_property_set_string (d, "storage.model", "");
		hal_device_property_set_string (d, "volume.fstype", "");
		hal_device_property_set_string (d, "volume.fsusage", "");
		hal_device_property_set_string (d, "volume.fsversion", "");
		hal_device_property_set_string (d, "volume.uuid", "");
		hal_device_property_set_string (d, "volume.label", "");
		hal_device_property_set_string (d, "volume.mount_point", "");
		hal_device_property_set_bool (d, "volume.is_mounted", FALSE);
		hal_device_property_set_bool (d, "volume.is_mounted_read_only", FALSE);
		hal_device_property_set_bool (
			d, "volume.is_disc", 
			strcmp (hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0);
		hal_device_property_set_bool (d, "volume.is_partition", TRUE);

		hal_device_property_set_string (d, "info.category", "volume");
		if (strcmp(hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0) {
			hal_device_add_capability (d, "volume.disc");
		}
		hal_device_add_capability (d, "volume");
		hal_device_add_capability (d, "block");

		/* determine partition number - unless, of course, we're a fakevolume */
		sysfs_path_len = strlen (sysfs_path);
		if (!is_fakevolume) {
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
		if (!hal_util_set_int_from_file (d, "volume.num_blocks", sysfs_path_real, "size", 0)) {
			HAL_INFO (("Ignoring hotplug event - cannot read 'size'"));
			goto error;
		}
		hal_device_property_set_uint64 (
			d, "volume.size",
			((dbus_uint64_t)(512)) * ((dbus_uint64_t)(hal_device_property_set_int (d, "volume.block_size", 512))));

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
}

static void
force_unmount_cb (HalDevice *d, guint32 exit_type, 
		  gint return_code, gchar **error,
		  gpointer data1, gpointer data2)
{
	void *end_token = (void *) data1;

	HAL_INFO (("force_unmount_cb for udi='%s', exit_type=%d, return_code=%d", d->udi, exit_type, return_code));

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

static void
force_unmount (HalDevice *d, void *end_token)
{
	const char *device_file;
	const char *mount_point;

	device_file = hal_device_property_get_string (d, "block.device");
	mount_point = hal_device_property_get_string (d, "volume.mount_point");

	/* look up in /media/.hal-mtab to see if we mounted this one */
	if (mount_point != NULL && strlen (mount_point) > 0 && is_mounted_by_hald (mount_point)) {
		char *unmount_stdin;
		char *extra_env[2];

		extra_env[0] = "HAL_METHOD_INVOKED_BY_UID=0";
		extra_env[1] = NULL;
		
		HAL_INFO (("force_unmount for udi='%s'", d->udi));
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

/*
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
*/
	}


}

void
hotplug_event_begin_remove_blockdev (const gchar *sysfs_path, gboolean is_partition, void *end_token)
{
	HalDevice *d;

	HAL_INFO (("block_rem: sysfs_path=%s is_part=%d", sysfs_path, is_partition));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Error removing device"));
		hotplug_event_end (end_token);
	} else {
		const char *stor_udi;
		HalDevice *stor_dev;
		gboolean is_fakevolume;

		is_partition = hal_device_property_get_bool (d, "volume.is_partition");

		if (strcmp (hal_util_get_last_element (sysfs_path), "fakevolume") == 0)
			is_fakevolume = TRUE;
		else
			is_fakevolume = FALSE;		

		/* ignore hotplug events on IDE partitions since ide-cs and others causes hotplug 
		 * rem/add when the last closer (including mount) closes the device. (Unless it's
		 * a fakevolume)
		 *
		 * This causes an infinite loop since we open the device to probe. How nice.
		 *
		 * Instead - we'll be removing the partition once the main block device
		 * goes away
		 */
		stor_udi = hal_device_property_get_string (d, "block.storage_device");
		if (is_partition && 
		    !is_fakevolume &&
		    stor_udi != NULL && 
		    ((stor_dev = hal_device_store_find (hald_get_gdl (), stor_udi)) != NULL)) {
			const char *stor_bus;
			stor_bus = hal_device_property_get_string (stor_dev, "storage.bus");
			if (strcmp (stor_bus, "ide") == 0) {
				/* unless we are already delayed, cf. the code below */
				if (hal_device_property_get_bool (d, ".already_delayed") != TRUE) {
					HAL_INFO (("Ignoring hotplug event"));
					hotplug_event_end (end_token);
					goto out;
				}
			}
		} else if (!is_partition) {
			GSList *i;
			GSList *partitions;
			unsigned int num_childs;
			/* see if there any partitions lying around that we refused to remove above */

			partitions = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
										       "block.storage_device",
										       stor_udi);

			/* have to count number of childs first */
			num_childs = 0;
			for (i = partitions; i != NULL; i = g_slist_next (i)) {
				HalDevice *child;
				child = HAL_DEVICE (i->data);
				/* ignore ourself */
				if (child == d)
					continue;
				num_childs++;
			}

			if (num_childs > 0) {

				/* OK, so we did have childs to remove before removing ourself
				 *
				 * Enqueue at front of queue; the childs will get in front of us
				 * because they will also queue up in front of use (damn kids!)
				 */
				HAL_INFO (("Delaying hotplug event until childs are done and gone"));
				hotplug_event_enqueue_at_front ((HotplugEvent *) end_token);
				
				for (i = partitions; i != NULL; i = g_slist_next (i)) {
					HalDevice *child;
					HotplugEvent *hotplug_event;
					child = HAL_DEVICE (i->data);
					/* ignore ourself */
					if (child == d)
						continue;

					/* set a flag such that we *will* get removed above */
					hal_device_property_set_bool (child, ".already_delayed", TRUE);

					HAL_INFO (("Generating hotplug rem for ignored fakevolume/ide_part with udi %s",
						   child->udi));
					/* yay! - gen hotplug event and fast track us to the front of the queue :-) */
					hotplug_event = blockdev_generate_remove_hotplug_event (child);
					hotplug_event_enqueue_at_front (hotplug_event);
				}

				g_slist_free (partitions);

				/* since we pushed ourselves into the queue again above, say that we're done
				 * but the event shouldn't get deleted
				 */
				hotplug_event_reposted (end_token);
				goto out;

			} else {
				/* No childs to remove before ourself; just remove ourself, e.g. carry on below */
				g_slist_free (partitions);
			}
		}

		/* if we're mounted, then do a lazy unmount so the system can gracefully recover */
		if (hal_device_property_get_bool (d, "volume.is_mounted")) {
			force_unmount (d, end_token);
		} else {
			hal_util_callout_device_remove (d, blockdev_callouts_remove_done, end_token, NULL);
		}
	}
out:
	;
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
		/* we've got a fs on the main block device - add fakevolume if we haven't got one already */
		if (fakevolume == NULL) {
			generate_fakevolume_hotplug_event_add_for_storage_device (d);
		}
	} else {
		/* no fs on the main block device - remove fakevolume if we have one */
		if (fakevolume != NULL) {
			/* generate hotplug event to remove the fakevolume */
			HotplugEvent *hotplug_event;
			hotplug_event = blockdev_generate_remove_hotplug_event (fakevolume);
			if (hotplug_event != NULL) {
				hotplug_event_enqueue (hotplug_event);
				hotplug_event_process_queue ();
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

	HAL_INFO (("Entering, udi=%s", d->udi));

	/* This only makes sense on storage devices */
	if (hal_device_property_get_bool (d, "block.is_volume")) {
		HAL_INFO (("No action on volumes", d->udi));
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

	hotplug_event = g_new0 (HotplugEvent, 1);
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

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}
