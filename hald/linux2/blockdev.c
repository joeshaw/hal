/***************************************************************************
 * CVSID: $Id$
 *
 * blockdev.c : Handling of block devices
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <ctype.h>
#include <unistd.h>

#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../device_info.h"
#include "../hald_conf.h"
#include "../hald_dbus.h"

#include "osspec_linux.h"

#include "util.h"
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
					      "/org/freedesktop/Hal/devices/volume_label_%s", uuid);
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
		} else if (model != NULL) {
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
blockdev_callouts_add_done (HalDevice *d, gpointer userdata)
{
	void *end_token = (void *) userdata;

	HAL_INFO (("Add callouts completed udi=%s", d->udi));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
blockdev_callouts_remove_done (HalDevice *d, gpointer userdata)
{
	void *end_token = (void *) userdata;

	HAL_INFO (("Remove callouts completed udi=%s", d->udi));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	hotplug_event_end (end_token);
}

static void 
update_mount_point (HalDevice *d)
{
	FILE *f;
	struct mntent mnt;
	struct mntent *mnte;
	const char *device_file;
	char buf[512];

	if ((device_file = hal_device_property_get_string (d, "block.device")) == NULL)
		goto out;
	
	if ((f = setmntent ("/etc/mtab", "r")) == NULL) {
		HAL_ERROR (("Could not open /etc/mtab"));
		goto out;
	}
		
	while ((mnte = getmntent_r (f, &mnt, buf, sizeof(buf))) != NULL) {
		if (strcmp (mnt.mnt_fsname, device_file) == 0) {
			device_property_atomic_update_begin ();
			hal_device_property_set_bool (d, "volume.is_mounted", TRUE);
			hal_device_property_set_string (d, "volume.mount_point", mnt.mnt_dir);
			device_property_atomic_update_end ();
			goto found;
		}
	}

	device_property_atomic_update_begin ();
	hal_device_property_set_bool (d, "volume.is_mounted", FALSE);
	hal_device_property_set_string (d, "volume.mount_point", "");
	device_property_atomic_update_end ();

found:		
	endmntent (f);
out:
	;
}

void 
blockdev_mtab_changed (void)
{
	GSList *i;
	GSList *volumes;

	volumes = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "volume.fsusage",
								    "filesystem");
	for (i = volumes; i != NULL; i = g_slist_next (i)) {
		HalDevice *d;

		d = HAL_DEVICE (i->data);
		update_mount_point (d);
	}
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
	hotplug_event->is_add = TRUE;
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
add_blockdev_probing_helper_done (HalDevice *d, gboolean timed_out, gint return_code, 
				  gpointer data1, gpointer data2, HalHelperData *helper_data)
{
	void *end_token = (void *) data1;
	gboolean is_volume;

	HAL_INFO (("entering; timed_out=%d, return_code=%d", timed_out, return_code));

	is_volume = hal_device_property_get_bool (d, "block.is_volume");

	/* Discard device if probing reports failure 
	 * 
	 * (return code 2 means fs found on main block device (for non-volumes)) 
	 */
	if (timed_out || !(return_code == 0 || (!is_volume && return_code == 2))) {
		hal_device_store_remove (hald_get_tdl (), d);
		hotplug_event_end (end_token);
		goto out;
	}

	if (!blockdev_compute_udi (d)) {
		hal_device_store_remove (hald_get_tdl (), d);
		hotplug_event_end (end_token);
		goto out;
	}

	/* set block.storage_device for storage devices since only now we know the UDI */
	if (!is_volume) {
		hal_device_copy_property (d, "info.udi", d, "block.storage_device");
	} else {
		/* check for mount point */
		update_mount_point (d);
	}

	/* Merge properties from .fdi files */
	di_search_and_merge (d);
	
	/* TODO: Merge persistent properties */

	/* Run callouts */
	hal_util_callout_device_add (d, blockdev_callouts_add_done, end_token);

	/* Yay, got a file system on the main block device...
	 *
	 * Generate a fake hotplug event to get this added
	 */
	if (!is_volume && return_code == 2) {
		generate_fakevolume_hotplug_event_add_for_storage_device (d);
	}


out:
	;
}



void
hotplug_event_begin_add_blockdev (const gchar *sysfs_path, const gchar *device_file, gboolean is_partition,
				  HalDevice *parent, void *end_token)
{
	gchar *major_minor;
	HalDevice *d;
	unsigned int major, minor;
	gboolean is_fakevolume;
	char *sysfs_path_real;

	HAL_INFO (("block_add: sysfs_path=%s dev=%s is_part=%d, parent=0x%08x", 
		   sysfs_path, device_file, is_partition, parent));

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

	if (parent == NULL) {
		HAL_INFO (("Ignoring hotplug event - no parent"));
		goto error;
	}

	d = hal_device_new ();
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
		hal_device_property_set_string (d, "storage.model", "");
		hal_device_property_set_string (d, "storage.vendor", "");
		hal_device_property_set_string (d, "storage.drive_type", "disk");

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
					break;
				} else if (strcmp (bus, "mmc") == 0) {
					physdev = d_it;
					physdev_udi = udi_it;
					hal_device_property_set_string (d, "storage.bus", "mmc");
					break;
				}
			}

			/* Go to parent */
			udi_it = hal_device_property_get_string (d_it, "info.parent");
		}

		/* needs physical device */
		if (physdev_udi == NULL)
			goto error;

		hal_device_property_set_string (d, "storage.physical_device", physdev_udi);

		if (!hal_util_get_int_from_file (sysfs_path, "removable", (gint *) &is_removable, 10))
			goto error;

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
				    strcmp (media, "floppy")) {
					hal_device_property_set_string (d, "storage.drive_type", media);
				} else {
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
			gint type;

			if (hal_util_set_string_from_file (d, "storage.vendor", sysfs_path, "device/vendor"))
				hal_device_copy_property (d, "storage.vendor", d, "info.vendor");
			if (hal_util_set_string_from_file (d, "storage.model", sysfs_path, "device/model"))
				hal_device_copy_property (d, "storage.model", d, "info.product");

			if (!hal_util_get_int_from_file (sysfs_path, "device/type", &type, 0))
				goto error;

			/* These magic values are documented in the kernel source */
			switch (type) {
			case 0:	/* Disk */
				hal_device_property_set_string (d, "storage.drive_type", "disk");
				break;

			case 5:	/* CD-ROM */
				hal_device_property_set_string (d, "storage.drive_type", "cdrom");
				break;

			default:
				goto error;
			}

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

		/* add to TDL so prober can access it */
		hal_device_store_add (hald_get_tdl (), d);

		/* TODO: run prober for 
		 *
		 *  - drive_id
		 *  - cdrom drive properties
		 *  - non-partitioned filesystem on main block device
		 */

		/* probe the device */
		if (hal_util_helper_invoke ("hald-probe-storage", NULL, d, (gpointer) end_token, 
					    NULL, add_blockdev_probing_helper_done, 
					    HAL_HELPER_TIMEOUT) == NULL) {
			hal_device_store_remove (hald_get_tdl (), d);
			goto error;
		}

	} else {
		guint sysfs_path_len;

		/*************************
		 *
		 * VOLUMES
		 *
		 ************************/

		hal_device_property_set_string (d, "block.storage_device", parent->udi);

		/* set defaults */
		hal_device_property_set_string (d, "volume.fstype", "");
		hal_device_property_set_string (d, "volume.fsusage", "");
		hal_device_property_set_string (d, "volume.fsversion", "");
		hal_device_property_set_string (d, "volume.uuid", "");
		hal_device_property_set_string (d, "volume.label", "");
		hal_device_property_set_string (d, "volume.mount_point", "");
		hal_device_property_set_bool (d, "volume.is_mounted", FALSE);
		hal_device_property_set_bool (
			d, "volume.is_disc", 
			strcmp (hal_device_property_get_string (parent, "storage.drive_type"), "cdrom") == 0);
		hal_device_property_set_bool (d, "volume.is_partition", TRUE);

		hal_device_property_set_string (d, "info.category", "volume");
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
					goto error;
				}
			} else {
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
			((dbus_uint64_t)(512)) * 
			((dbus_uint64_t)(hal_device_property_set_int (d, "volume.block_size", 512))));

		/* add to TDL so prober can access it */
		hal_device_store_add (hald_get_tdl (), d);

		/* probe the device */
		if (hal_util_helper_invoke ("hald-probe-volume", NULL, d, (gpointer) end_token, 
					    NULL, add_blockdev_probing_helper_done, 
					    HAL_HELPER_TIMEOUT) == NULL) {
			hal_device_store_remove (hald_get_tdl (), d);
			goto error;
		}
	}

	g_free (sysfs_path_real);
	return;

error:
	if (d != NULL)
		g_object_unref (d);
out:
	hotplug_event_end (end_token);

	g_free (sysfs_path_real);
}

static void
force_unmount (HalDevice *d)
{
	const char *storudi;
	HalDevice *stordev;
	const char *device_file;
	const char *device_mount_point;
	const char *umount_argv[4] = { "/bin/umount", "-l", NULL, NULL };
	char *umount_stdout;
	char *umount_stderr;
	int umount_exitcode;

	device_file = hal_device_property_get_string (d, "block.device");
	device_mount_point = hal_device_property_get_string (d, "volume.mount_point");

	HAL_INFO (("Entering... udi=%s device_file=%s mount_point=%s", d->udi, device_file, device_mount_point));

	/* Only attempt to 'umount -l' if some hal policy piece are performing policy on the device */
	storudi = hal_device_property_get_string (d, "block.storage_device");
	if (storudi == NULL) {
		HAL_WARNING (("Could not get block.storage_device"));
		goto out;
	}
	stordev = hal_device_store_find (hald_get_gdl (), storudi);
	if (stordev == NULL) {
		HAL_WARNING (("Could not get device object for storage device"));
		goto out;
	} else {
		if ((!hal_device_has_property (stordev, "storage.policy.should_mount")) ||
		    (!hal_device_property_get_bool (stordev, "storage.policy.should_mount"))) {
			HAL_WARNING (("storage device doesn't have storage.policy.should_mount"));
			goto out;
		}
	}

	umount_argv[2] = device_file;

	if (hal_device_has_property (d, "block.is_volume") &&
	    hal_device_property_get_bool (d, "block.is_volume") &&
	    hal_device_property_get_bool (d, "volume.is_mounted") &&
	    device_mount_point != NULL &&
	    strlen (device_mount_point) > 0) {
		HAL_INFO (("attempting /bin/umount -l %s", device_file));

		/* TODO: this is a bit dangerous; rather spawn async and do some timout on it */

		/* invoke umount */
		if (g_spawn_sync ("/",
				  (char **) umount_argv,
				  NULL,
				  0,
				  NULL,
				  NULL,
				  &umount_stdout,
				  &umount_stderr,
				  &umount_exitcode, NULL) != TRUE) {
			HAL_ERROR (("Couldn't invoke /bin/umount"));
		}

		if (umount_exitcode != 0) {
			HAL_ERROR (("/bin/umount returned %d", umount_exitcode));
		} else {
			/* Tell clients we are going to unmount so they close
			 * can files - otherwise this unmount is going to stall
			 *
			 * One candidate for catching this would be FAM - the
			 * File Alteration Monitor
			 *
			 * Lazy unmount been in Linux since 2.4.11, so we're
			 * homefree (but other kernels might not support this)
			 */
			HAL_INFO (("Goint to emit VolumeUnmountForced('%s', '%s', TRUE)", device_file, device_mount_point));
			device_send_signal_condition (d,
						      "VolumeUnmountForced",
						      DBUS_TYPE_STRING,
						      device_file,
						      DBUS_TYPE_STRING,
						      device_mount_point,
						      DBUS_TYPE_INVALID);
		}
	} else {
		HAL_INFO (("Didn't want to unmount %s", device_file));
	}
out:
	;
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
			force_unmount (d);
		}

		hal_util_callout_device_remove (d, blockdev_callouts_remove_done, end_token);
	}
out:
	;
}

static void 
block_rescan_storage_done (HalDevice *d, gboolean timed_out, gint return_code, 
			   gpointer data1, gpointer data2, HalHelperData *helper_data)
{
	const char *sysfs_path;
	HalDevice *fakevolume;
	char fake_sysfs_path[HAL_PATH_MAX];

	HAL_ERROR (("hald-probe-storage --only-check-for-media returned %d", return_code));

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
	if (hal_util_helper_invoke ("hald-probe-storage --only-check-for-media", NULL, d, NULL, 
				    NULL, block_rescan_storage_done, 
				    HAL_HELPER_TIMEOUT) == NULL) {
		HAL_INFO (("Could not invoke 'hald-probe-storage --only-check-for-media'"));
		goto out;
	}
	
	ret = TRUE;

out:
	return ret;
}


HotplugEvent *
blockdev_generate_add_hotplug_event (HalDevice *d)
{
	const char *sysfs_path;
	const char *device_file;
	HotplugEvent *hotplug_event;

	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");
	device_file = hal_device_property_get_string (d, "block.device");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->is_add = TRUE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	if (device_file != NULL)
		g_strlcpy (hotplug_event->sysfs.device_file, device_file, sizeof (hotplug_event->sysfs.device_file));
	else
		hotplug_event->sysfs.device_file[0] = '\0';
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
	hotplug_event->is_add = FALSE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, "block", sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}
