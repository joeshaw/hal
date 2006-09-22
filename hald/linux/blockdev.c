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
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

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

	g_slist_foreach (autofs_mounts, (GFunc) g_free, NULL);
	g_slist_free (autofs_mounts);

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
			HAL_INFO (("/proc/mounts tells that %s is unmounted - waiting for Unmount() to complete to change mount state", dev->udi));
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

	hotplug_event = g_new0 (HotplugEvent, 1);
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
	GError *error;
	char *dir;
	char *link;
	char *f;
	char *f1;

	f = g_strdup (file);

	while (g_file_test (f, G_FILE_TEST_IS_SYMLINK)) {
		link = g_file_read_link (f, &error);
		if (link == NULL) {
			g_warning ("Cannot resolve symlink %s: %s", f, error->message);
			g_error_free (error);
			g_free (f);
			f = NULL;
			goto out;
		}
		
		dir = g_path_get_dirname (f);
		f1 = g_strdup_printf ("%s/%s", dir, link);
		g_free (dir);
		g_free (link);
		g_free (f);
		f = f1;
	}

out:
	if (f != NULL)
		canonicalize_filename (f);
	return f;
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

	is_device_mapper = FALSE;

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

	/* OK, no parent... it might a device-mapper device => check slaves/ subdir in sysfs */
	if (parent == NULL && !is_partition && !is_fakevolume) {
		GDir *dir;
		GError *err = NULL;
		char path[HAL_PATH_MAX];


		/* sleep one second since device mapper needs additional
		 * time before the device file is ready
		 *
		 * this is a hack and will only affect device mapper block
		 * devices. It can go away once the kernel emits a "changed"
		 * event for the device file (this is about to go upstream)
		 * and we can depend on a released kernel with this feature.
		 */
		if (strncmp (hal_util_get_last_element (sysfs_path), "dm-", 3) == 0) {
			HAL_INFO (("Waiting 1000ms to wait for device mapper to be ready", path));
			usleep (1000 * 1000);
		}

		g_snprintf (path, HAL_PATH_MAX, "%s/slaves", sysfs_path);
		HAL_INFO (("Looking in %s", path));
		if ((dir = g_dir_open (path, 0, &err)) == NULL) {
			HAL_WARNING (("Unable to open %s: %s", path, err->message));
			g_error_free (err);
		} else {
			const char *f;
			while (((f = g_dir_read_name (dir)) != NULL) && (parent == NULL)) {
				char *link;
				char *target;

				link = g_strdup_printf ("%s/%s", path, f);
				target = resolve_symlink (link);
				HAL_INFO ((" %s -> %s", link, target));

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
								HAL_INFO ((" parent='%s'!", parent->udi));
								hal_device_property_set_string (d, "volume.crypto_luks.clear.backing_volume", slave_volume->udi);
								is_device_mapper = TRUE;
							}
						}
					}
				}
				g_free (target);
			}
			g_dir_close (dir);
			HAL_INFO (("Done looking in %s", path));
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
	hal_device_property_set_bool (d, "block.is_volume", is_partition || is_device_mapper || is_fakevolume);

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

		hal_device_property_set_bool (d, "storage.removable.media_available", FALSE);
		hal_device_property_set_bool (d, "storage.removable", is_removable);
		/* set storage.size only if we have fixed media */
		if (!is_removable) {
			guint64 num_blocks;
			if (hal_util_get_uint64_from_file (sysfs_path, "size", &num_blocks, 0)) {
				/* TODO: sane to assume this is always 512 for non-removable? 
				 * I think genhd.c guarantees this... */
				hal_device_property_set_uint64 (d, "storage.size", num_blocks * 512);
			}
		} else {
			hal_device_property_set_uint64 (d, "storage.size", 0);
		}

		/* by default, do checks for media if, and only if, the removable file is set to 1
		 *
		 * Problematic buses, like IDE, may override this.
		 */
		hal_device_property_set_bool (d, "storage.media_check_enabled", is_removable);

		parent_bus = hal_device_property_get_string (parent, "info.bus");
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
		gboolean is_physical_partition;

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
		hal_device_property_set_bool (d, "volume.linux.is_device_mapper", is_device_mapper);
		hal_device_property_set_bool (
			d, "volume.is_disc", 
			strcmp (hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0);


		is_physical_partition = TRUE;
		if (is_fakevolume || is_device_mapper)
			is_physical_partition = FALSE;

		hal_device_property_set_bool (d, "volume.is_partition", is_physical_partition);

		hal_device_property_set_string (d, "info.category", "volume");
		if (strcmp(hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0) {
			hal_device_add_capability (d, "volume.disc");
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
		if (!hal_util_set_int_from_file (d, "volume.num_blocks", sysfs_path_real, "size", 0)) {
			HAL_INFO (("Ignoring hotplug event - cannot read 'size'"));
			goto error;
		}
		hal_device_property_set_uint64 (
			d, "volume.size",
			((dbus_uint64_t)(512)) * ((dbus_uint64_t)(hal_device_property_get_int (d, "volume.num_blocks"))));
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
	if (mount_point != NULL && strlen (mount_point) > 0 && hal_util_is_mounted_by_hald (mount_point)) {
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
				   "synthesizing hotplug rem for fakevolume %s", fakevolume->udi));
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
			HAL_INFO (("Media removal detected; synthesizing hotplug rem for fakevolume %s", fakevolume->udi));
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
