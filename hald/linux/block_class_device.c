/***************************************************************************
 * CVSID: $Id$
 *
 * Block device class
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

#define _GNU_SOURCE 1

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <glib.h>
#include <mntent.h>
 
#include "../hald.h"
#include "../hald_dbus.h"
#include "../logger.h"
#include "../device_store.h"
#include "../callout.h"
#include "../hald_conf.h"
#include "../osspec.h"
#include "class_device.h"
#include "common.h"

#include "volume_id/volume_id.h"
#include "drive_id/drive_id.h"
#include "linux_dvd_rw_utils.h"

/**
 * @defgroup HalDaemonLinuxBlock Block device class
 * @ingroup HalDaemonLinux
 * @brief Block device class
 * @{
 */

static void etc_mtab_process_all_block_devices (dbus_bool_t force);

static char *block_class_compute_udi (HalDevice * d, int append_num);

#if 0
static gboolean deferred_check_for_non_partition_media (gpointer data);
#endif


static void
set_volume_id_values(HalDevice *d, struct volume_id *vid)
{
	char *product;
	const char *usage;

	switch (vid->usage_id) {
	case VOLUME_ID_FILESYSTEM:
		usage = "filesystem";
		break;
	case VOLUME_ID_PARTITIONTABLE:
		usage = "partitiontable";
		break;
	case VOLUME_ID_OTHER:
		usage = "other";
		break;
	case VOLUME_ID_RAID:
		usage = "raid";
		break;
	case VOLUME_ID_UNUSED:
		hal_device_property_set_string (d, "info.product", "Volume (unused)");
		usage = "unused";
		return;
	default:
		usage = "";
	}
	hal_device_property_set_string (d, "volume.fsusage", usage);
	HAL_INFO (("volume.fsusage = '%s'", usage));

	hal_device_property_set_string (d, "volume.fstype", vid->type);
	HAL_INFO (("volume.fstype = '%s'", vid->type));
	if (vid->type_version[0] != '\0') {
		hal_device_property_set_string (d, "volume.fsversion", vid->type_version);
		HAL_INFO (("volume.fsversion = '%s'", vid->type_version));
	}
	hal_device_property_set_string (d, "volume.uuid", vid->uuid);
	HAL_INFO (("volume.uuid = '%s'", vid->uuid));
	hal_device_property_set_string (d, "volume.label", vid->label);
	HAL_INFO (("volume.label = '%s'", vid->label));

	if (vid->label[0] != '\0') {
		hal_device_property_set_string (d, "info.product", vid->label);
	} else {
		product = g_strdup_printf ("Volume (%s)", vid->type);
		hal_device_property_set_string (d, "info.product", product);
		g_free (product);
	}
}

static HalDevice *
get_child_device_gdl (HalDevice *d)
{
	HalDevice *child;

	child = hal_device_store_match_key_value_string (
		hald_get_gdl (), "info.parent",
		hal_device_get_udi (d));

	return child;
}

static HalDevice *
get_child_device_tdl (HalDevice *d)
{
	HalDevice *child;

	child = hal_device_store_match_key_value_string
		(hald_get_tdl (), "info.parent",
		hal_device_get_udi (d));

	return child;
}

ClassDeviceHandler block_class_handler;

static dbus_bool_t
block_class_accept (ClassDeviceHandler *self,
		    const char *path,
		    struct sysfs_class_device *class_device)
{

	/*HAL_INFO (("path = %s, classname = %s", 
	  path, self->sysfs_class_name));*/

	/* only care about given sysfs class name */
	if (strcmp (class_device->classname, self->sysfs_class_name) == 0) {
		return TRUE;
	}

	return FALSE;
}


static HalDevice *
block_class_visit (ClassDeviceHandler *self,
		   const char *path,
		   struct sysfs_class_device *class_device)
{
	HalDevice *d;
	HalDevice *parent;
	char *parent_sysfs_path;
	ClassAsyncData *cad;
	struct sysfs_device *sysdevice;

	/* only care about given sysfs class name */
	if (strcmp (class_device->classname, "block") != 0)
		return NULL;

	d = hal_device_new ();
	hal_device_store_add (hald_get_tdl (), d);
	g_object_unref (d);

	hal_device_property_set_string (d, "info.bus", self->hal_class_name);
	hal_device_property_set_string (d, "linux.sysfs_path", path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", path);

	hal_device_property_set_bool (d, "block.no_partitions", FALSE);
	hal_device_property_set_bool (d, "block.have_scanned", FALSE);

	sysdevice = sysfs_get_classdev_device (class_device);

	if (sysdevice == NULL) {
		parent_sysfs_path = get_parent_sysfs_path (path);
		hal_device_property_set_bool (d, "block.is_volume", TRUE);
	} else {
		parent_sysfs_path = sysdevice->path;
		hal_device_property_set_bool (d, "block.is_volume", FALSE);
	}

	/* temporary property used for _udev_event() */
	hal_device_property_set_string (d, ".target_dev", "block.device");
	hal_device_property_set_string (d, ".udev.class_name", "block");
	hal_device_property_set_string (d, ".udev.sysfs_path", path);

	/* Ask udev about the device file if we are probing */
	if (self->require_device_file && hald_is_initialising) {
		char dev_file[SYSFS_PATH_MAX];

		if (!class_device_get_device_file (path, dev_file, 
						   SYSFS_PATH_MAX)) {
			/*HAL_WARNING (("Couldn't get device file for class "
			  "device with sysfs path", path));*/
			return NULL;
		}

		/* If we are not probing this function will be called upon
		 * receiving a dbus event */
		self->udev_event (self, d, dev_file);
	}

	cad = g_new0 (ClassAsyncData, 1);
	cad->device = d;
	cad->handler = self;

	/* Find parent; this can happen synchronously as our parent is
	 * sure to be added before us (we probe devices in the right order
	 * and we reorder hotplug events)
	 */
	parent = hal_device_store_match_key_value_string (hald_get_gdl (), 
							  "linux.sysfs_path_device", 
							  parent_sysfs_path);
	if (parent == NULL) {
		hal_device_store_remove (hald_get_tdl (), d);
		d = NULL;
		goto out;
	}

	class_device_got_parent_device (hald_get_tdl (), parent, cad);

out:
	return d;
}


static char *
strip_space (char *str)
{
	int i, len;

	len = strlen (str);
	for (i = len - 1; i > 0 && isspace (str[i]); --i)
		str[i] = '\0';

	return str;
}


static void
cdrom_get_properties (HalDevice *d, const char *device_file)
{
	int fd, capabilities;
	int read_speed, write_speed;

	/* Check handling */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;
		
	if( ioctl (fd, CDROM_SET_OPTIONS, CDO_USE_FFLAGS) < 0 )
	    HAL_ERROR(("CDROM_SET_OPTIONS failed: %s\n", strerror(errno)));
		
	capabilities = ioctl (fd, CDROM_GET_CAPABILITY, 0);
	if (capabilities < 0) {
		close(fd);
		return;
	}

	hal_device_property_set_bool (d, "storage.cdrom.cdr", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.cdrw", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvd", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdr", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdrw", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdram", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdplusr", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdplusrw", FALSE);

	if (capabilities & CDC_CD_R) {
		hal_device_property_set_bool (d, "storage.cdrom.cdr", TRUE);
	}
	
	if (capabilities & CDC_CD_RW) {
		hal_device_property_set_bool (d, "storage.cdrom.cdrw", TRUE);
	}
	if (capabilities & CDC_DVD) {
		int profile;

		/** @todo FIXME BUG XXX: need to check for dvdrw (prolly need to rewrite much of 
		 *  the linux_dvdrw_utils.c file)
		 */
		
		hal_device_property_set_bool (d, "storage.cdrom.dvd", TRUE);
		
		profile = get_dvd_r_rw_profile (fd);
		HAL_INFO (("profile %d\n", profile));
		if (profile == 2) {
			hal_device_property_set_bool (d, "storage.cdrom.dvdplusr", TRUE);
			hal_device_property_set_bool (d, "storage.cdrom.dvdplusrw", TRUE);
		} else if (profile == 0) {
			hal_device_property_set_bool(d, "storage.cdrom.dvdplusr", TRUE);
		} else if (profile == 1) {
			hal_device_property_set_bool (d, "storage.cdrom.dvdplusrw", TRUE);
		}
	}
	if (capabilities & CDC_DVD_R) {
		hal_device_property_set_bool (d, "storage.cdrom.dvdr", TRUE);
	}
	if (capabilities & CDC_DVD_RAM) {
		hal_device_property_set_bool (d, "storage.dvdram", TRUE);
	}
	
	/* while we're at it, check if we support media changed */
	if (ioctl (fd, CDROM_MEDIA_CHANGED) >= 0) {
		hal_device_property_set_bool (d, "storage.cdrom.support_media_changed", TRUE);
	} else {
		hal_device_property_set_bool (d, "storage.cdrom.support_media_changed", FALSE);
	}
	
	if (get_read_write_speed(fd, &read_speed, &write_speed) >= 0) {
		hal_device_property_set_int (d, "storage.cdrom.read_speed", read_speed);
		if (write_speed > 0)
			hal_device_property_set_int(d, "storage.cdrom.write_speed", write_speed);
		else
			hal_device_property_set_int(d, "storage.cdrom.write_speed", 0);
	}

	close (fd);
}



/** Force unmount of a patition. Must have block.volume=1 and valid
 *  block.device
 *
 *  @param  d                   Device
 */
static void
force_unmount (HalDevice * d)
{
	const char *device_file;
	const char *device_mount_point;
	const char *umount_argv[4] = { "/bin/umount", "-l", NULL, NULL };
	char *umount_stdout;
	char *umount_stderr;
	int umount_exitcode;

	device_file = hal_device_property_get_string (d, "block.device");
	device_mount_point =
	    hal_device_property_get_string (d, "volume.mount_point");

	umount_argv[2] = device_file;

	if (hal_device_has_property (d, "block.is_volume") &&
	    hal_device_property_get_bool (d, "block.is_volume") &&
	    hal_device_property_get_bool (d, "volume.is_mounted") &&
	    device_mount_point != NULL &&
	    strlen (device_mount_point) > 0) {
		HAL_INFO (("attempting /bin/umount -l %s", device_file));

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
			HAL_ERROR (("/bin/umount returned %d",
				    umount_exitcode));
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

			/* Woohoo, have to change volume.mount_point *afterwards*, other
			 * wise device_mount_point points to garbage and D-BUS throws
			 * us off the bus, in fact it's doing exiting with code 1
			 * for us - not nice
			 */
/*
			device_property_atomic_update_begin ();
			hal_device_property_set_string (d, "volume.mount_point",
						"");
			hal_device_property_set_string (d, "volume.fstype", "");
			hal_device_property_set_bool (d, "volume.is_mounted",
					      FALSE);
			device_property_atomic_update_end ();
*/
		}
	}
}

/** Unmount all partitions that stems from this block device. Must have
 *  block.is_volume==0
 *
 *  @param  d                   Device
 */
static void
force_unmount_of_all_childs (HalDevice * d)
{
	int fd;
	const char *device_file;
	GSList *children;

	device_file = hal_device_property_get_string (d, "block.device");

	children = hal_device_store_match_multiple_key_value_string (
		hald_get_gdl (),
		"info.parent",
		hal_device_get_udi (d));

	if (children != NULL) {
		GSList *iter;

		for (iter = children; iter != NULL; iter = iter->next) {
			HalDevice *child = HAL_DEVICE (iter->data);

			force_unmount (child);

		} /* for all children */

		g_slist_free (children);

		HAL_INFO (("Rereading partition table for %s",
			   device_file));
		fd = open (device_file, O_RDONLY | O_NONBLOCK);
		if (fd != -1) {
			ioctl (fd, BLKRRPART);
		}
		close (fd);

		/* All this work should generate hotplug events to actually
		 * remove the child devices 
		 */

	} /* childs!=NULL */
}

static void
volume_remove_from_gdl (HalDevice *device, gpointer user_data)
{
	g_signal_handlers_disconnect_by_func (device, volume_remove_from_gdl, 
					      user_data);
	hal_device_store_remove (hald_get_gdl (), device);
}


/** Check if a filesystem on a special device file is mounted
 *
 *  @param  device_file         Special device file, e.g. /dev/cdrom
 *  @return                     TRUE iff there is a filesystem system mounted
 *                              on the special device file
 */
static dbus_bool_t
is_mounted (const char *device_file)
{
	FILE *f;
	dbus_bool_t rc;
	struct mntent mnt;
	struct mntent *mnte;
	char buf[512];

	rc = FALSE;

	if ((f = setmntent ("/etc/mtab", "r")) == NULL)
		goto out;

	while ((mnte = getmntent_r (f, &mnt, buf, sizeof(buf))) != NULL) {
		if (strcmp (device_file, mnt.mnt_fsname) == 0) {
			rc = TRUE;
			goto out1;
		}
	}

out1:
	endmntent (f);
out:
	return rc;
}

/**
 * if volume_id returns a parsed partition map instead of a filesystem,
 * we just probe all partitions for a valid filesystem and return it
 */
static struct volume_id *
get_first_valid_partition(struct volume_id *id)
{
	struct volume_id *p = volume_id_open_fd(id->fd);
	if (p == NULL)
		return NULL;

	if (id->partition_count > 0) {
		unsigned int i;

		for (i = 0; i < id->partition_count; i++) {
			unsigned long long off;
			unsigned long long len;

			off = id->partitions[i].off;
			len = id->partitions[i].len;

			if (len == 0)
				continue;

			if (volume_id_probe(p, VOLUME_ID_ALL, off, 0) == 0 &&
			    p->usage_id == VOLUME_ID_FILESYSTEM)
				return p;
		}
	}

	volume_id_close(p);
	return NULL;
}


static void
volume_set_size (HalDevice *d, dbus_bool_t force)
{
	int fd;
	int num_blocks;
	int block_size;
	dbus_uint64_t vol_size;
	const char *sysfs_path;
	const char *storudi;
	const char *device_file;
	HalDevice *stordev;
	char attr_path[SYSFS_PATH_MAX];
	struct sysfs_attribute *attr;

	storudi = hal_device_property_get_string (d, "block.storage_device");
	stordev = hal_device_store_find (hald_get_gdl (), storudi);

	if (force || hal_device_property_get_bool (stordev, "storage.media_check_enabled")) {

		sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");
		/* no-partition volumes don't have a sysfs path */
		if (sysfs_path == NULL)
			sysfs_path = hal_device_property_get_string (stordev, "linux.sysfs_path");

		if (sysfs_path == NULL)
			return;

		device_file = hal_device_property_get_string (d, "block.device");

		snprintf (attr_path, SYSFS_PATH_MAX, "%s/size", sysfs_path);
		attr = sysfs_open_attribute (attr_path);
		if (sysfs_read_attribute (attr) >= 0) {
			num_blocks = atoi (attr->value);

			hal_device_property_set_int (d, "volume.num_blocks", num_blocks);
			HAL_INFO (("volume.num_blocks = %d", num_blocks));
			sysfs_close_attribute (attr);
		}

		fd = open (device_file, O_RDONLY);
		if (fd >= 0) {
			if (ioctl (fd, BLKSSZGET, &block_size) == 0) {
				hal_device_property_set_int (d, "volume.block_size", block_size);
				HAL_INFO (("volume.block_size = %d", block_size));
			}

			if (ioctl (fd, BLKGETSIZE64, &vol_size) == 0) {
				hal_device_property_set_uint64 (d, "volume.size", vol_size);
				HAL_INFO (("volume.size = %lld", vol_size));
			}

			close (fd);
		}
	}
}

static void
detect_disc (HalDevice *d, const char *device_file)
{
	int fd;
	int type;

	/* defaults */
	hal_device_property_set_string (d, "volume.disc.type", "unknown");
	hal_device_property_set_bool (d, "volume.disc.has_audio", FALSE);
	hal_device_property_set_bool (d, "volume.disc.has_data", FALSE);
	hal_device_property_set_bool (d, "volume.disc.is_blank", FALSE);
	hal_device_property_set_bool (d, "volume.disc.is_appendable", FALSE);
	hal_device_property_set_bool (d, "volume.disc.is_rewritable", FALSE);

	/* Check handling */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;
	
	/* check for audio/data/blank */
	type = ioctl (fd, CDROM_DISC_STATUS, CDSL_CURRENT);
	switch (type) {
	case CDS_AUDIO:		/* audio CD */
		hal_device_property_set_bool (d, "volume.disc.has_audio", TRUE);
		HAL_INFO (("Disc in %s has audio", device_file));
		break;
	case CDS_MIXED:		/* mixed mode CD */
		hal_device_property_set_bool (d, "volume.disc.has_audio", TRUE);
		hal_device_property_set_bool (d, "volume.disc.has_data", TRUE);
		HAL_INFO (("Disc in %s has audio+data", device_file));
		break;
	case CDS_DATA_1:	/* data CD */
	case CDS_DATA_2:
	case CDS_XA_2_1:
	case CDS_XA_2_2:
		hal_device_property_set_bool (d, "volume.disc.has_data", TRUE);
		HAL_INFO (("Disc in %s has data", device_file));
		break;
	case CDS_NO_INFO:	/* blank or invalid CD */
		hal_device_property_set_bool (d, "volume.disc.is_blank", TRUE);
		HAL_INFO (("Disc in %s is blank", device_file));
		break;
		
	default:		/* should never see this */
		hal_device_property_set_string (d, "volume.disc_type", "unknown");
		HAL_INFO (("Disc in %s returned unknown CDROM_DISC_STATUS", device_file));
		break;
	}
	
	/* see table 373 in MMC-3 for details on disc type
	 * http://www.t10.org/drafts.htm#mmc3
	 */
	type = get_disc_type (fd);
	HAL_INFO (("get_disc_type returned 0x%02x", type));
	if (type != -1) {
		switch (type) {
		case 0x08: /* CD-ROM */
			hal_device_property_set_string (d, "volume.disc.type", "cd_rom");
			break;
		case 0x09: /* CD-R */
			hal_device_property_set_string (d, "volume.disc.type", "cd_r");
			break;
		case 0x0a: /* CD-RW */
			hal_device_property_set_string (d, "volume.disc.type", "cd_rw");
			hal_device_property_set_bool (d, "volume.disc.is_rewritable", TRUE);
			break;
		case 0x10: /* DVD-ROM */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_rom");
			break;
		case 0x11: /* DVD-R Sequential */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_r");
			break;
		case 0x12: /* DVD-RAM */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_ram");
			hal_device_property_set_bool (d, "volume.disc.is_rewritable", TRUE);
			break;
		case 0x13: /* DVD-RW Restricted Overwrite */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_rw");
			hal_device_property_set_bool (d, "volume.disc.is_rewritable", TRUE);
			break;
		case 0x14: /* DVD-RW Sequential */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_rw");
			hal_device_property_set_bool (d, "volume.disc.is_rewritable", TRUE);
			break;
		case 0x1A: /* DVD+RW */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_plus_rw");
			hal_device_property_set_bool (d, "volume.disc.is_rewritable", TRUE);
			break;
		case 0x1B: /* DVD+R */
			hal_device_property_set_string (d, "volume.disc.type", "dvd_plus_r");
			hal_device_property_set_bool (d, "volume.disc.is_rewritable", TRUE);
			break;
		default: 
			break;
		}
	}

	/* On some hardware the get_disc_type call fails,
	   so we use this as a backup */
	if (disc_is_rewritable (fd)) {
		hal_device_property_set_bool (
			d, "volume.disc.is_rewritable", TRUE);
	}

	if (disc_is_appendable (fd)) {
		hal_device_property_set_bool (
			d, "volume.disc.is_appendable", TRUE);
	}

	close (fd);
	return;
}

/** Check for media on a block device that is not a volume
 *
 *  @param  d                   Device to inspect; can be any device, but
 *                              it will only have effect if the device is
 *                              in the GDL and is of capability block and
 *                              is not a volume
 *  @param force_poll           If TRUE, do polling even though 
 *                              storage.media_check_enabled==FALSE
 *  @return                     TRUE if the GDL was modified
 */
static dbus_bool_t
detect_media (HalDevice * d, dbus_bool_t force_poll)
{
	const char *device_file;
	HalDevice *child;
	int fd;
	struct volume_id *vid;
	gboolean no_partitions;
	dbus_bool_t is_cdrom;
	dbus_bool_t got_media;

	/* respect policy unless we force */
	if (!force_poll &&
	    !hal_device_property_get_bool (d, "storage.media_check_enabled"))
		return FALSE;

	/* Refuse to poll on storage devices without removable media */
	if (!force_poll && !hal_device_property_get_bool (d, "storage.removable"))
		return FALSE;

	/* need to be in GDL */
	if (!hal_device_store_find (hald_get_gdl (), hal_device_get_udi (d)))
		return FALSE;

	/* need to be a drive and not a volume */
	if (!hal_device_has_property (d, "block.is_volume") ||
	    !hal_device_has_property (d, "block.device") ||
	    hal_device_property_get_bool (d, "block.is_volume"))
		return FALSE;

	/* we have to do special treatment for optical drives */
	is_cdrom = hal_device_has_property (d, "storage.drive_type") &&
		strcmp (hal_device_property_get_string (d, "storage.drive_type"), "cdrom") == 0 &&
		hal_device_property_get_bool (d, "storage.cdrom.support_media_changed");

	/* current state of media */
	no_partitions = hal_device_property_get_bool (d, "block.no_partitions");

	got_media = FALSE;

	device_file = hal_device_property_get_string (d, "block.device");

	if (device_file == NULL)
		return FALSE;

	if (force_poll) {
		HAL_INFO (("Forcing check for media check on device %s", device_file));
	}

	if (is_cdrom)
		fd = open (device_file, O_RDONLY | O_NONBLOCK | O_EXCL);
	else
		fd = open (device_file, O_RDONLY);

	if (is_cdrom) {
		int drive;

		if (fd < 0 && errno == EBUSY) {
			/* this means the disc is mounted or some other app,
			 * like a cd burner, has already opened O_EXCL */
			
			/* HOWEVER, when starting hald, a disc may be
			 * mounted; so check /etc/mtab to see if it
			 * actually is mounted. If it is we retry to open
			 * without O_EXCL
			 */
			if (!is_mounted (device_file))
				return FALSE;
		
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
		}

		if (fd < 0)
			return FALSE;

		/* Check if a disc is in the drive
		 *
		 * @todo Use MMC-2 API if applicable
		 */
		drive = ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
		switch (drive) {
			/* explicit fallthrough */
		case CDS_NO_INFO:
		case CDS_NO_DISC:
		case CDS_TRAY_OPEN:
		case CDS_DRIVE_NOT_READY:
			break;
			
		case CDS_DISC_OK:
			got_media = TRUE;
			break;
			
		case -1:
		    HAL_ERROR(("CDROM_DRIVE_STATUS failed: %s\n", strerror(errno)));
		    break;

		default:
			break;
		}

	} else {
		if (fd < 0 && errno == ENOMEDIUM) {
			got_media = FALSE;
		} else {
			got_media = TRUE;
		}

	}

	if (!got_media) {
		/* No media in drive; if we had volumes for this drive,
		 * now is a good time to remove them.. */
		
		if (no_partitions) {
			child = hal_device_store_match_key_value_string (
				hald_get_gdl (), "info.parent",
				hal_device_get_udi (d));
			
			/* We can have only one child, simply remove it */
			
			if (child != NULL ) {
				HAL_INFO (("Removing volume for "
					   "no_partitions device %s",
					   device_file));

				force_unmount (child);
				
				g_signal_connect (child, "callouts_finished",
						  G_CALLBACK (volume_remove_from_gdl), NULL);
				hal_callout_device (child, FALSE);
				
				hal_device_property_set_bool (d, "block.no_partitions", 
							      hal_device_property_get_bool (
								      d, "storage.no_partitions_hint"));
				hal_device_property_set_bool (d, "block.have_scanned", FALSE);

				if (fd >= 0)
					close (fd);
				return TRUE;
			}
			
		} else {
			
			/* Just unmount the childs already mounted (otherwise
			 * no hotplug will happen). This causes storm of hotplug
			 * remove events and The Right Stuff Happens(tm)
			 */
			
			force_unmount_of_all_childs (d);
			
			hal_device_property_set_bool (d, "block.no_partitions", 
						      hal_device_property_get_bool (
							      d, "storage.no_partitions_hint"));

			hal_device_property_set_bool (d, "block.have_scanned", FALSE);
		}

		if (fd >= 0)
			close (fd);
		return FALSE;
	}

	if (fd < 0)
		return FALSE;

	/* Now got_media==TRUE and fd>0.. So, Yay!, we got media
	 *
	 * See if we already got children (or children underway :-),
	 * if so, just exit this function as life is good as nothing
	 * really changed
	 */

	child = get_child_device_gdl (d);
	if (child == NULL) {
		get_child_device_tdl (d);
	}
	if (child != NULL) {
		close (fd);
		return FALSE;
	}

	/* No children... So, this means that the media have just been
	 * inserted. The poll we just did will make the kernel search
	 * for partition tables. If there is indeed a known, to the
	 * kernel, partition table then we'll get hotplug events and
	 * life is good because these are handled elsewhere.
	 *
	 * In the event there is no partition table, then we get no
	 * feedback whatsoever. Which is kind of rude of the kernel,
	 * but hey...
	 *
	 * Therefore, we need to scan the media to see if if the
	 * top-level block device is indeed a filesystem we know. If
	 * yes, then there can't be any partition table. If we find
	 * anything, we set the block.no_partitions to TRUE. And then
	 * we add a child. This needs to be set to the value of
	 * storage.no_partitions_hint when we detect that the media 
	 * is removed.
	 *
	 * We also set block.have_scanned to TRUE so we won't do this
	 * again (in the event that the kernel didn't find a partition
	 * table and the top-level block device didn't contain an
	 * known (to HAL) filesystem). Clearly, this must be set to
	 * FALSE when media is removed.
	 */

	if (!hal_device_property_get_bool (d, "block.have_scanned")) {
		ClassAsyncData *cad;

		hal_device_property_set_bool (d, "block.have_scanned", TRUE);

		child = hal_device_new ();

		/* copy block.* properties from parent */
		hal_device_merge_with_rewrite (child, d, "block", "block");
		/* in particular, set this to the value of storage.no_partitions_hint */
		hal_device_property_set_bool (child, "block.no_partitions", 
					      hal_device_property_get_bool (d, "block.no_partitions"));

		/* modify some properties */
		hal_device_property_set_bool (child, "block.is_volume", TRUE);
		hal_device_property_set_bool (child, "block.have_scanned", FALSE);

		/* add info.* properties */
		hal_device_property_set_string (child, "info.bus", "block");
		hal_device_property_set_string (child, "info.capabilities", "block volume");
		hal_device_property_set_string (child, "info.category", "volume");
		hal_device_property_set_string (child, "info.parent", d->udi);
		hal_device_property_set_string (child, "info.product", "Volume");

		/* set defaults */
		hal_device_property_set_string (child, "volume.fstype", "");
		hal_device_property_set_string (child, "volume.fsusage", "");
		hal_device_property_set_string (child, "volume.fsversion", "");
		hal_device_property_set_string (child, "volume.uuid", "");
		hal_device_property_set_string (child, "volume.label", "");
		hal_device_property_set_string (child, "volume.mount_point", "");
		hal_device_property_set_bool (child, "volume.is_mounted", FALSE);
		hal_device_property_set_bool (child, "volume.is_disc", is_cdrom);
		hal_device_property_set_bool (d, "volume.is_partition", FALSE);

		/* set the size */
		volume_set_size (child, force_poll);

		if (is_cdrom )
			detect_disc (child, device_file);

		vid = NULL;

		if (!is_cdrom || 
		    (is_cdrom && hal_device_property_get_bool (child, "volume.disc.has_data"))) {

			HAL_INFO (("Detecting if %s contains a fs", device_file));

			vid = volume_id_open_fd (fd);
			if (vid == NULL) {
				close (fd);
				g_object_unref (child);
				return FALSE;
			}
			
			if (volume_id_probe (vid, VOLUME_ID_ALL, 0, 0) != 0) {
				if (is_cdrom) {
					/* volume_id cannot probe blank/audio discs etc,
					 * so don't fail for them, just set vid to NULL */
					volume_id_close (vid);
					vid = NULL;
				} else {
					close (fd);
					g_object_unref (child);
					volume_id_close (vid);
					return FALSE;
				}
			}
		}

		/* Unfortunally, linux doesn't scan optical discs for partition
		 * tables. We only get the main device, the kernel doesn't
		 * create childs for us.
		 */
		no_partitions = is_cdrom || (vid != NULL && vid->usage_id == VOLUME_ID_FILESYSTEM);
		hal_device_property_set_bool (d, "block.no_partitions", no_partitions);

		if (!no_partitions) {
			if (vid != NULL)
				volume_id_close (vid);
			close (fd);
			g_object_unref (child);
			return FALSE;
		}

		HAL_INFO (("Media in no_partitions device %s", device_file));

		hal_device_store_add (hald_get_tdl (), child);
		g_object_unref (child);

		/* If _we_ detect a partition table like a apple hfs cd, we just
		 * stop at the first partiton with a valid filesystem
		 */
		if (vid != NULL) {
			if (vid->partition_count > 0) {
				struct volume_id *p;
				
				p = get_first_valid_partition(vid);
				if (p != NULL) {
					set_volume_id_values(child, p);
					volume_id_close(p);
				}
			} else {
				set_volume_id_values(child, vid);
			}
		}

		/* set UDI (will scan .fdi files and merge from unplugged) */
		rename_and_merge (child, block_class_compute_udi, "volume");

		cad = g_new0 (ClassAsyncData, 1);
		cad->device = child;
		cad->handler = &block_class_handler;
		cad->merge_or_add = block_class_handler.merge_or_add;

		/* add new device */
		g_signal_connect (child, "callouts_finished",
			G_CALLBACK (class_device_move_from_tdl_to_gdl), cad);
		hal_callout_device (child, TRUE);

		volume_id_close (vid);
		close (fd);
		return TRUE;
	}

	close (fd);
	return FALSE;
}


static void
block_class_got_udi (ClassDeviceHandler *self,
		     HalDevice *d,
		     const char *udi)
{
	const char *stordev_udi;
	char temp_prefix[] = "/org/freedesktop/Hal/devices/temp";

	/* fixup from setting block.storage_device below to a temporary 
	 * device */
	stordev_udi = hal_device_property_get_string (d, 
						      "block.storage_device");

	if (strncmp (stordev_udi, temp_prefix, strlen(temp_prefix)) == 0) {
		hal_device_property_set_string (d,
						"block.storage_device",
						udi);
	}
}


static void
block_class_pre_process (ClassDeviceHandler *self,
			 HalDevice *d,
			 const char *sysfs_path,
			 struct sysfs_class_device *class_device)
{
	int major, minor;
	HalDevice *parent;
	HalDevice *stordev = NULL;
	HalDevice *physdev = NULL;
	HalDevice *scsidev = NULL;
	const char *stordev_udi;
	const char *device_file;
	dbus_bool_t has_removable_media = FALSE;
	dbus_bool_t is_hotpluggable = FALSE;
	char attr_path[SYSFS_PATH_MAX];
	struct sysfs_attribute *attr;

	parent = hal_device_store_find (hald_get_gdl (),
					hal_device_property_get_string (
						d, "info.parent"));
	assert (parent != NULL);

	/* add capabilities for device */
	hal_device_property_set_string (d, "info.category", "block");
	hal_device_add_capability (d, "block");

	class_device_get_major_minor (sysfs_path, &major, &minor);
	hal_device_property_set_int (d, "block.major", major);
	hal_device_property_set_int (d, "block.minor", minor);

	device_file = hal_device_property_get_string (d, "block.device");

	/* Determine physical device that is backing this block device */
	if (hal_device_property_get_bool (d, "block.is_volume")) {
		/* Take the block parent */
		stordev_udi = hal_device_property_get_string (
			parent, "block.storage_device");
		stordev = hal_device_store_find (hald_get_gdl (), stordev_udi);
	} else {
		const char *udi_it;
		const char *physdev_udi = NULL;

		/* Set ourselves to be the storage.* keeper */
		stordev_udi = d->udi;
		stordev = d;

		/* Defaults */
		hal_device_property_set_string (stordev, "storage.bus", "unknown");
		hal_device_property_set_bool (stordev, "storage.media_check_enabled", 
					      (hald_get_conf ())->storage_media_check_enabled);
		hal_device_property_set_bool (stordev, "storage.no_partitions_hint", FALSE);

		hal_device_property_set_bool (
			stordev, 
			"storage.automount_enabled_hint", 
			(hald_get_conf ())->storage_automount_enabled_hint);

		hal_device_property_set_string (stordev, 
						"storage.model", "");
		hal_device_property_set_string (stordev, 
						"storage.vendor", "");

		/* walk up the device chain to find the physical device, 
		 * start with our parent. On the way, optionally pick up
		 * the scsi if it exists */
		udi_it = parent->udi;

		while (udi_it != NULL) {
			HalDevice *d_it;
			const char *bus;

			/* Find device */
			d_it = hal_device_store_find (hald_get_gdl (), udi_it);
			assert (d_it != NULL);

			/* Check info.bus */
			bus = hal_device_property_get_string (d_it,"info.bus");

			if (strcmp (bus, "scsi") == 0) {
				scsidev = d_it;
				physdev = d_it;
				physdev_udi = udi_it;
				hal_device_property_set_string (
					stordev, "storage.bus", "scsi");
			}

			if (strcmp (bus, "usb") == 0) {
				physdev = d_it;
				physdev_udi = udi_it;
				is_hotpluggable = TRUE;
				hal_device_property_set_string (
					stordev, "storage.bus", "usb");
								
				break;
			} else if (strcmp (bus, "ieee1394") == 0) {
				physdev = d_it;
				physdev_udi = udi_it;
				is_hotpluggable = TRUE;
				hal_device_property_set_string (
					stordev, "storage.bus", "ieee1394");
				break;
			} else if (strcmp (bus, "ide") == 0) {
				physdev = d_it;
				physdev_udi = udi_it;
				hal_device_property_set_string (
					stordev, "storage.bus", "ide");
				break;
			}

			/* Go to parent */
			udi_it = hal_device_property_get_string (
				d_it, "info.parent");
		}

		if (physdev_udi != NULL) {
			hal_device_property_set_string (
				stordev, 
				"storage.physical_device",
				physdev_udi);
		}
	}

	hal_device_property_set_string (d, "block.storage_device",
					stordev_udi);

	if (hal_device_property_get_bool (d, "block.is_volume")) {
		/* block device that is a partition; e.g. a storage volume */
		struct volume_id *vid;
		const char *last_elem;
		const char *s;
		unsigned int partition_number;

		hal_device_add_capability (d, "volume");
		hal_device_property_set_string (d, "info.category", "volume");
		hal_device_property_set_string (d, "info.product", "Volume");
		hal_device_property_set_string (d, "volume.fstype", "");
		hal_device_property_set_string (d, "volume.fsusage", "");
		hal_device_property_set_string (d, "volume.fsversion", "");
		hal_device_property_set_string (d, "volume.label", "");
		hal_device_property_set_string (d, "volume.uuid", "");
		hal_device_property_set_bool (d, "volume.is_disc", FALSE);
		hal_device_property_set_bool (d, "volume.is_mounted", FALSE);
		hal_device_property_set_bool (d, "volume.is_partition", TRUE);

		/* get partition number */
		last_elem = get_last_element (sysfs_path);
		for (s = last_elem; *s != '\0' && !isdigit(*s); s++)
			;
		partition_number = (unsigned int) atoi (s);
		hal_device_property_set_int (d, "volume.partition.number", partition_number);

		/* only check for volume_id if we are allowed to poll, otherwise we may
		 * cause inifite loops of hotplug events, cf. broken ide-cs driver and
		 * broken zip drives. Merely accessing the top-level block device if it
		 * or any of it partitions are not mounted causes the loop.
		 */
		if (hal_device_property_get_bool (stordev, "storage.media_check_enabled")) {
			dbus_uint64_t size = 0;
			const char *stordev_device_file;

			volume_set_size (d, FALSE);
			size = hal_device_property_get_uint64 (d, "volume.size");

			vid = volume_id_open_node(device_file);
			if (vid != NULL) {
				if (volume_id_probe(vid, VOLUME_ID_ALL, 0, size) == 0) {
					set_volume_id_values(d, vid);
				}
				volume_id_close(vid);
			}

			/* get partition type - presently we only support PC style partition tables */
			stordev_device_file = hal_device_property_get_string (stordev, "block.device");
			vid = volume_id_open_node (stordev_device_file);
			if (vid != NULL) {
				if (volume_id_probe(vid, VOLUME_ID_MSDOSPARTTABLE, 0, size) == 0) {
					HAL_INFO (("Number of partitions = %d", vid->partition_count));

					if (partition_number >= 0 && partition_number < vid->partition_count) {
						struct volume_id_partition *p;
						p = &vid->partitions[partition_number];


						hal_device_property_set_int (
							d, "volume.partition.x86_type",
							p->partition_type_raw);
						
						/* NOTE: We trust the type from the partition table
						 * if it explicitly got correct entries for RAID and
						 * LVM partitions.
						 *
						 * Btw, in general it's not a good idea to trust the
						 * partition table type as many geek^Wexpert users use 
						 * FAT filesystems on type 0x83 which is Linux.
						 *
						 * Linux RAID autodetect is 0xfd and Linux LVM is 0x8e
						 */
						if (p->partition_type_raw == 0xfd ||
						    p->partition_type_raw == 0x8e ) {
							hal_device_property_set_string (
								d, "volume.fsusage", "raid");
						}
							
					} else {
						HAL_WARNING (("partition_number=%d not in [0;%d[", 
							      partition_number, vid->partition_count));
					}
				}
				
				volume_id_close(vid);
			}

		} else {
			/* gee, so at least set volume.fstype vfat,msdos,auto so 
			 * mount(1) doesn't screw up and causes hotplug events
			 *
			 * GRRRR!!!
			 */
			hal_device_property_set_string (d, "volume.fstype", "vfat,auto");
			hal_device_property_set_string (d, "volume.usage", "filesystem");
		}
		return;
	}


	/************************************************************
	 * FOLLOWING CODE ONLY APPLY FOR TOPLEVEL BLOCK DEVICES
	 *       (e.g. for setting storage.* properties)
	 ************************************************************/


	/* defaults */
	hal_device_property_set_string (stordev, "storage.drive_type", "disk");

	/* We are a disk or cdrom drive; maybe we even offer 
	 * removable media 
	 */
	hal_device_property_set_string (d, "info.category", "block");

	HAL_INFO (("Bus type is %s!",
		   hal_device_property_get_string (parent, "info.bus")));

	if (strcmp (hal_device_property_get_string (parent, "info.bus"),
			    "ide") == 0) {
		const char *ide_name;
		char *model;
		char *media;


		/* blacklist the broken ide-cs driver */
		if (physdev != NULL) {
			size_t len;
			char buf[256];
			const char *physdev_sysfs_path;
			
			snprintf (buf, 256, "%s/devices/ide", sysfs_mount_path);
			len = strlen (buf);
			
			physdev_sysfs_path = hal_device_property_get_string (physdev, "linux.sysfs_path");
			
			if (strncmp (physdev_sysfs_path, buf, len) == 0) {
				hal_device_property_set_bool (stordev, "storage.media_check_enabled", FALSE);
			}
			
			HAL_INFO (("Working around broken ide-cs driver for %s", physdev->udi));
		}


		ide_name = get_last_element (hal_device_property_get_string
					     (d, "linux.sysfs_path"));

		model = read_single_line ("/proc/ide/%s/model", ide_name);
		if (model != NULL) {
			hal_device_property_set_string (stordev,
							"storage.model",
							model);
			hal_device_property_set_string (d, 
							"info.product",
							model);
		}

		/* According to the function proc_ide_read_media() in 
		 * drivers/ide/ide-proc.c in the Linux sources, media
		 * can only assume "disk", "cdrom", "tape", "floppy", 
		 * "UNKNOWN"
		 */
		
		/** @todo Given floppy how
		 *        do we determine it's LS120?
		 */
			
		media = read_single_line ("/proc/ide/%s/media",
					  ide_name);
		if (media != NULL) {
			hal_device_property_set_string (stordev, 
							"storage.drive_type",
							media);
			
			/* Set for removable media */
			if (strcmp (media, "disk") == 0) {
				/* left blank */
			} else if (strcmp (media, "cdrom") == 0) {
				has_removable_media = TRUE;
			} else if (strcmp (media, "floppy") == 0) {
				has_removable_media = TRUE;

				/* I've got a LS120 that identifies as a
				 * floppy; polling doesn't work so disable
				 * media check and automount
				 */
				hal_device_property_set_bool (
					d, "storage.media_check_enabled", FALSE);
				hal_device_property_set_bool (
					d, "storage.automount_enabled_hint", FALSE);

			} else if (strcmp (media, "tape") == 0) {
				has_removable_media = TRUE;

				/* TODO: Someone test with tape drives! */
			}			
		}

		/* only check for drive_id if we are allowed to poll, otherwise we may
		 * cause inifite loops of hotplug events, cf. broken ide-cs driver and
		 * broken zip drives. Merely accessing the top-level block device if it
		 * or any of it partitions are not mounted causes the loop.
		 */
		if (hal_device_property_get_bool (stordev, "storage.media_check_enabled")) {
			const char *device_file;
			struct drive_id *did;

			device_file = hal_device_property_get_string (d, "block.device");
			did = drive_id_open_node(device_file);
			if (drive_id_probe(did, DRIVE_ID_ATA) == 0) {
				if (did->serial[0] != '\0')
					hal_device_property_set_string (stordev, 
									"storage.serial",
									did->serial);
				if (did->firmware[0] != '\0')
					hal_device_property_set_string (stordev, 
									"storage.firmware_version",
									did->firmware);
			}
			drive_id_close(did);
		}

		
	} else if (strcmp (hal_device_property_get_string (parent,
							 "info.bus"),
			   "scsi") == 0) {
		const char *device_file;
		struct drive_id *did;
		const char *sysfs_path;
		int scsi_host;
		char *scsi_protocol;
		
		sysfs_path = hal_device_property_get_string (
			d, "linux.sysfs_path");
		
		snprintf (attr_path, SYSFS_PATH_MAX,
			  "%s/device/vendor", sysfs_path);
		attr = sysfs_open_attribute (attr_path);
		if (sysfs_read_attribute (attr) >= 0) {
			strip_space (attr->value);
			hal_device_property_set_string (d, "info.vendor",
							attr->value);
			hal_device_property_set_string (stordev,
							"storage.vendor",
							attr->value);
			sysfs_close_attribute (attr);
		}
		
		snprintf (attr_path, SYSFS_PATH_MAX,
			  "%s/device/model", sysfs_path);
		attr = sysfs_open_attribute (attr_path);
		if (sysfs_read_attribute (attr) >= 0) {
			strip_space (attr->value);
			hal_device_property_set_string (d,
							"info.product",
							attr->value);
			hal_device_property_set_string (stordev,
							"storage.model",
							attr->value);
			sysfs_close_attribute (attr);
		}

		device_file = hal_device_property_get_string (d, "block.device");
		did = drive_id_open_node(device_file);
		if (drive_id_probe(did, DRIVE_ID_SCSI) == 0) {
			if (did->serial[0] != '\0')
				hal_device_property_set_string (stordev,
								"storage.serial",
								did->serial);
			if (did->revision[0] != '\0')
				hal_device_property_set_string (stordev, 
								"storage.revision",
								did->revision);
		}
		drive_id_close(did);

		snprintf (attr_path, SYSFS_PATH_MAX,
			  "%s/device/type", sysfs_path);
		attr = sysfs_open_attribute (attr_path);
		if (sysfs_read_attribute (attr) >= 0) {
			int type = parse_dec (attr->value);
			switch (type) {
			case 0:	/* Disk */
				hal_device_property_set_string (
					stordev, 
					"storage.drive_type", 
					"disk");
				break;
			case 1:	/* Tape */
				hal_device_property_set_string (
					stordev,
					"storage.drive_type", 
					"tape");
				has_removable_media = TRUE;
				break;
			case 5:	/* CD-ROM */
				hal_device_property_set_string (
					stordev, 
					"storage.drive_type", 
					"cdrom");
				has_removable_media = TRUE;
				break;
			default:
				/** @todo add more SCSI types */
				HAL_WARNING (("Don't know how to "
					      "handle SCSI type %d", 
					      type));
			}
		}

		/* Check for (USB) floppy - Yuck, this is pretty ugly in
		 * quite a few ways!! Reading /proc-files, looking at
		 * some hardcoded string from the kernel etc. etc.
		 *
		 * @todo : Get Protocol into sysfs or something more sane
		 * giving us the type of the SCSI device
		 */
		scsi_host = hal_device_property_get_int (
			parent, "scsi.host");
		scsi_protocol = read_single_line_grep (
			"     Protocol: ", 
			"/proc/scsi/usb-storage/%d", 
			scsi_host);
		if (scsi_protocol != NULL &&
		    strcmp (scsi_protocol,
			    "Uniform Floppy Interface (UFI)") == 0) {

			/* Indeed a (USB) floppy drive */

			hal_device_property_set_string (d, 
							"storage.drive_type", 
							"floppy");

			/* My experiments with my USB LaCie Floppy disk
			 * drive is that polling indeed work (Yay!), so
			 * we don't set storage.media_check_enabled to 
			 * FALSE - for devices where this doesn't work,
			 * we can override it with .fdi files
			 */
			has_removable_media = TRUE;
		}

	} else {
		/** @todo block device on non-IDE and non-SCSI device;
		 *  how to find the name and the media-type? 
		 */
		
		/* guestimate product name */
		hal_device_property_set_string (d, "info.product", "Disk");
		
	}

	snprintf (attr_path, SYSFS_PATH_MAX, "%s/removable", sysfs_path);
	attr = sysfs_open_attribute (attr_path);
	if (sysfs_read_attribute (attr) >= 0) {
		if (attr->value [0] == '0')
			has_removable_media = FALSE;
		else
			has_removable_media = TRUE;

		sysfs_close_attribute (attr);
	} 
	
	hal_device_property_set_bool (stordev, "storage.removable", has_removable_media);

	if (hal_device_has_property (stordev, "storage.drive_type") &&
	    strcmp (hal_device_property_get_string (stordev, "storage.drive_type"), 
		    "cdrom") == 0) {
		cdrom_get_properties (stordev, device_file);
		hal_device_add_capability (stordev, "storage.cdrom");
		hal_device_property_set_bool (stordev, "storage.no_partitions_hint", TRUE);
	}


	if (hal_device_has_property (stordev, "storage.drive_type") &&
	    strcmp (hal_device_property_get_string (stordev, "storage.drive_type"), 
		    "floppy") == 0) {
		hal_device_property_set_bool (stordev, "storage.no_partitions_hint", TRUE);
	}

	hal_device_property_set_string (stordev, "info.category", "storage");
	hal_device_add_capability (stordev, "storage");

	hal_device_property_set_bool (stordev, "storage.hotpluggable",
				      is_hotpluggable);

	hal_device_property_set_bool (stordev, "block.no_partitions", 
				      hal_device_property_get_bool (
					      stordev, "storage.no_partitions_hint"));


	/* FINALLY, merge information derived from a .fdi file, from the 
	 * physical device that is backing this block device.
	 *
	 * - physdev is either NULL or the physical device (ide,
	 *   usb, iee1394 etc.) of which we are the offspring
	 *
	 * - scsidev is either NULL or the SCSI device inbetween
	 *   us and the physical device 
	 */
	
	/* Merge storage.lun%d.* to storage.* from physical device for the
	 * appropriate LUN */
	if (physdev != NULL) {

		/* Merge storage.* from physdev to stordev */
		hal_device_merge_with_rewrite (stordev, physdev, 
					       "storage.", "storage.");
		
		/* If there's a scsi device inbetween merge all 
		 * storage_lun%d.* properties */
		if (scsidev != NULL) {
			int lun;
			char propname[64];
			
			lun = hal_device_property_get_int (
				scsidev, "scsi.lun");
		
			/* See 6in1-card-reader.fdi for an example */
			
			snprintf (propname, sizeof (propname), 
				  "storage_lun%d.", lun);
			
			hal_device_merge_with_rewrite (stordev, physdev, 
						       "storage.", propname);
		}
	}
}

static void
block_class_in_gdl (ClassDeviceHandler *self,
		    HalDevice *d,
		    const char *udi)
{
	dbus_bool_t force_initial_poll;

	/* check for media on the device 
	 *
	 * Force the initial poll if we support media_check_enabled; should fix some issues with
	 * the IBM USB Memory Stick that shockingly report /sys/block/<drive>/removable set to
	 * 0.
	 */
	force_initial_poll = hal_device_property_get_bool (d, "storage.media_check_enabled");
	detect_media (d, force_initial_poll);

	/* Check the mtab to see if the device is mounted */
	etc_mtab_process_all_block_devices (TRUE);

	if (!hal_device_property_get_bool (d, "block.is_volume") &&
	    !hal_device_property_get_bool (d, "storage.media_check_enabled")) {
		/* Right, if we don't have media_check_enabled we don't really know
		 * if the storage device contains media without partition
		 * tables. This is because of the hotplug infinite loops if trying
		 * to access the top-level block device.
		 *
		 * (man, if only the kernel could tell us that it didn't find
		 *  any partition tables!)
		 *
		 * We could try to setup a timer that fires in a few seconds to
		 * see whether we got any childs added and, if not, then resort
		 * to actually doing a detect_media on the top-level block device.
		 *
		 * This works on the ide-cs driver, but there are many other drivers
		 * it might not work on, so I've commented it out. Oh well.
		 */
		/*g_timeout_add (3000, deferred_check_for_non_partition_media, g_strdup(udi));*/
	}
}

#if 0
static gboolean
deferred_check_for_non_partition_media (gpointer data)
{
	gchar *stordev_udi = (gchar *) data;
	HalDevice *d;
	HalDevice *child;

	HAL_INFO (("Entering, udi %s", stordev_udi));

	/* See if device is still there */
	d = hal_device_store_find (hald_get_gdl (), stordev_udi);
	if (d == NULL)
		goto out;

	/* See if we already got children (check both TDL and GDL) */
	child = hal_device_store_match_key_value_string (hald_get_gdl (), "info.parent",
							 stordev_udi);
	if (child == NULL)
		child = hal_device_store_match_key_value_string (hald_get_tdl (), "info.parent",
								 stordev_udi);

	if (child != NULL)
		goto out;

	HAL_INFO (("Forcing check for media check on udi %s", stordev_udi));

	/* no children so force this check */
	detect_media (d, TRUE);	

out:
	g_free (stordev_udi);
	return FALSE;
}
#endif 

static char *
block_class_compute_udi (HalDevice * d, int append_num)
{
	char *format;
	static char buf[256];
	char id[256] = "";

	if (hal_device_property_get_bool (d, "block.is_volume")) {
		const char *label =
			hal_device_property_get_string (d, "volume.label");
		const char *uuid =
			hal_device_property_get_string (d, "volume.uuid");

		if (uuid != NULL && uuid[0] != '\0') {
			strcpy(id, uuid);
		} else if (label != NULL && label[0] != '\0') {
			if (strcmp(label, "/") == 0)
				strcpy(id, "root");
			else
				strcpy(id, label);
		}
	} else {
		const char *model =
			hal_device_property_get_string (d, "storage.model");
		const char *serial =
			hal_device_property_get_string (d, "storage.serial");

		if (model != NULL && serial != NULL && serial[0] != '\0') {
			snprintf (id, 255, "%s-%s", model, serial);
		} else {
			const char *phys_device;
			int lun = -1;
			const char *i;

			/* use UDI from storage.physical_device and append
			 * the LUN */

			phys_device = hal_device_property_get_string (
				d, "storage.physical_device");
			if (phys_device != NULL) {

				/* go up the chain and to find the LUN */
				
				i = hal_device_property_get_string (d, "info.parent");
				while (i != NULL) {
					HalDevice *idev;

					idev = hal_device_store_find (hald_get_gdl (), i);
					if (idev == NULL)
						break;

					if (hal_device_has_capability (idev, "scsi")) {
						lun = hal_device_property_get_int (idev, "scsi.lun");
						break;
					}


					i = hal_device_property_get_string (idev, "info.parent");
				}

				if (lun != -1 ) {
					snprintf (id, 255, 
						  "%s-lun%d", 
						  get_last_element (phys_device),
						  lun);
				}
			}
		}
	}
	id[255] = '\0';

	/* FIXME: do something better then fallback to major/minor */
	if (id[0] == '\0') {
		int major = hal_device_property_get_int (d, "block.major");
		int minor = hal_device_property_get_int (d, "block.minor");
		sprintf (id, "%d_%d", major, minor);
	}

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/block_%s";
	else
		format = "/org/freedesktop/Hal/devices/block_%s-%d";

	snprintf (buf, 255, format, id, append_num);
	buf[255] = '\0';

	return buf;
}



#define MOUNT_POINT_MAX 256
#define MOUNT_POINT_STRING_SIZE 128

/** Structure for holding mount point information */
struct mount_point_s {
	int major;			       /**< Major device number */
	int minor;			       /**< Minor device number */
	char device[MOUNT_POINT_STRING_SIZE];  /**< Device filename */
	char mount_point[MOUNT_POINT_STRING_SIZE];
					       /**< Mount point */
	char fs_type[MOUNT_POINT_STRING_SIZE]; /**< Filesystem type */
};

/** Array holding (valid) mount points from /etc/mtab. */
static struct mount_point_s mount_points[MOUNT_POINT_MAX];

/** Number of elements in #mount_points array */
static int num_mount_points;

static int etc_fd = -1;


/** Process a line in /etc/mtab. The given string will be modifed by
 *  this function.
 *
 *  @param  s                   Line of /etc/mtab
 */
static void
etc_mtab_process_line (char *s)
{
	int i;
	char *p;
	char *delim = " \t\n";
	char buf[256];
	char *bufp = buf;
	struct stat stat_buf;
	int major = 0;
	int minor = 0;
	char *device = NULL;
	char *mount_point = NULL;
	char *fs_type = NULL;

	i = 0;
	p = strtok_r (s, delim, &bufp);
	while (p != NULL) {
		/*printf("token: '%s'\n", p); */
		switch (i) {
		case 0:
			if (strcmp (p, "none") == 0)
				return;
			if (p[0] != '/')
				return;
			device = p;
			/* Find major/minor for this device */

			if (stat (p, &stat_buf) != 0) {
				return;
			}
			major = MAJOR (stat_buf.st_rdev);
			minor = MINOR (stat_buf.st_rdev);
			break;

		case 1:
			mount_point = p;
			break;

		case 2:
			fs_type = p;
			break;

		case 3:
			break;

		case 4:
			break;

		case 5:
			break;
		}

		p = strtok_r (NULL, delim, &bufp);
		i++;
	}

    /** @todo  FIXME: Use a linked list or something that doesn't restrict
     *         us like this
     */
	if (num_mount_points == MOUNT_POINT_MAX)
		return;

	mount_points[num_mount_points].major = major;
	mount_points[num_mount_points].minor = minor;
	strncpy (mount_points[num_mount_points].device, device,
		 MOUNT_POINT_STRING_SIZE);
	strncpy (mount_points[num_mount_points].mount_point, mount_point,
		 MOUNT_POINT_STRING_SIZE);
	strncpy (mount_points[num_mount_points].fs_type, fs_type,
		 MOUNT_POINT_STRING_SIZE);

	num_mount_points++;
}

/** Last mtime when /etc/mtab was processed */
static time_t etc_mtab_mtime = 0;


/** Reads /etc/mtab and fill out #mount_points and #num_mount_points 
 *  variables accordingly
 *
 *  This function holds the file open for further access
 *
 *  @param  force               Force reading of mtab
 *  @return                     FALSE if there was no changes to /etc/mtab
 *                              since last invocation or an error occured
 */
static dbus_bool_t
read_etc_mtab (dbus_bool_t force)
{
	int fd;
	char buf[256];
	FILE *f;
	struct stat stat_buf;

	num_mount_points = 0;

	fd = open ("/etc/mtab", O_RDONLY);

	if (fd == -1) {
		HAL_ERROR (("Cannot open /etc/mtab"));
		return FALSE;
	}

	if (fstat (fd, &stat_buf) != 0) {
		HAL_ERROR (("Cannot fstat /etc/mtab fd, errno=%d", errno));
		close(fd);
		return FALSE;
	}

	if (!force && etc_mtab_mtime == stat_buf.st_mtime) {
		/*printf("No modification, etc_mtab_mtime=%d\n", etc_mtab_mtime); */
		close(fd);
		return FALSE;
	}

	etc_mtab_mtime = stat_buf.st_mtime;

	/*printf("Modification, etc_mtab_mtime=%d\n", etc_mtab_mtime); */

	f = fdopen (fd, "r");

	if (f == NULL) {
		HAL_ERROR (("Cannot fdopen /etc/mtab fd"));
		return FALSE;
	}

	while (!feof (f)) {
		if (fgets (buf, 256, f) == NULL)
			break;
		/*printf("got line: '%s'\n", buf); */
		etc_mtab_process_line (buf);
	}

	fclose (f);
	return TRUE;
}

static void sigio_handler (int sig);

/** Global to see if we have setup the watcher on /etc */
static dbus_bool_t have_setup_watcher = FALSE;

static gboolean
mtab_handle_storage (HalDevice *d)
{
	HalDevice *child;
	int major, minor;
	dbus_bool_t found_mount_point;
	struct mount_point_s *mp = NULL;
	int i;
	
	if (!hal_device_has_property (d, "storage.media_check_enabled") ||
	    hal_device_property_get_bool (d, "storage.media_check_enabled"))
		return TRUE;

	/* Only handle storage devices we don't regulary poll for media.
	 *
	 * So, in this very specific situation we do poll for media
	 */

	major = hal_device_property_get_int (d, "block.major");
	minor = hal_device_property_get_int (d, "block.minor");

	/* See if we already got children */
	child = get_child_device_gdl (d);
	if (child == NULL)
		get_child_device_tdl (d);

	/* Search all mount points */
	found_mount_point = FALSE;
	for (i = 0; i < num_mount_points; i++) {

		mp = &mount_points[i];

		if (mp->major != major || mp->minor != minor)
			continue;

		if (child != NULL ) {
			found_mount_point = TRUE;
			return FALSE;
		}

		/* is now mounted, and we didn't have a child.. */
		HAL_INFO (("%s now mounted at %s, fstype=%s, udi=%s",
			   mp->device, mp->mount_point, mp->fs_type, d->udi));

		/* detect the media and do indeed force this check */
		detect_media (d, TRUE);

		return TRUE;
	}

	return TRUE;
}

static gboolean
mtab_handle_volume (HalDevice *d)
{
	int major, minor;
	dbus_bool_t found_mount_point;
	struct mount_point_s *mp;
	const char *storudi;
	HalDevice *stor;
	int i;

	major = hal_device_property_get_int (d, "block.major");
	minor = hal_device_property_get_int (d, "block.minor");

	/* these are handled in mtab_handle_storage */
	storudi = hal_device_property_get_string (d, "block.storage_device");
	stor = hal_device_store_find (hald_get_gdl (), storudi);
	if (hal_device_property_get_bool (stor, "block.no_partitions") &&
	    hal_device_has_property (d, "storage.media_check_enabled") &&
	    !hal_device_property_get_bool (d, "storage.media_check_enabled"))
		return TRUE;

	/* Search all mount points */
	found_mount_point = FALSE;
	for (i = 0; i < num_mount_points; i++) {
		mp = &mount_points[i];
			
		if (mp->major == major && mp->minor == minor) {
			const char *existing_block_device;
			dbus_bool_t was_mounted;

			device_property_atomic_update_begin ();

			existing_block_device =
				hal_device_property_get_string (d,
								"block.device");

			was_mounted =
				hal_device_property_get_bool (d,
							      "volume.is_mounted");

			/* Yay! Found a mount point; set properties accordingly */
			hal_device_property_set_string (d,
							"volume.mount_point",
							mp->mount_point);
			hal_device_property_set_string (d, "volume.fstype",
							mp->fs_type);
			hal_device_property_set_bool (d,
						      "volume.is_mounted",
						      TRUE);

			/* only overwrite block.device if it's not set */
			if (existing_block_device == NULL ||
			    (existing_block_device != NULL &&
			     strcmp (existing_block_device,
				     "") == 0)) {
				hal_device_property_set_string (d,
								"block.device",
								mp->
								device);
			}

			device_property_atomic_update_end ();

			if (!was_mounted) {

				HAL_INFO (("%s is mounted at %s, major:minor=%d:%d, fstype=%s, udi=%s", mp->device, mp->mount_point, mp->major, mp->minor, mp->fs_type, d->udi));

				device_send_signal_condition (
					d,
					"VolumeMount",
					DBUS_TYPE_STRING,
					hal_device_property_get_string (
						d,
						"block.device"),
					DBUS_TYPE_STRING,
					mp->mount_point,
					DBUS_TYPE_INVALID);
			}

			found_mount_point = TRUE;
			break;
		}
	}

	/* No mount point found; (possibly) remove all information */
	if (!found_mount_point) {
		dbus_bool_t was_mounted;
		gchar *device_mount_point;

		device_mount_point = g_strdup (hal_device_property_get_string (d, "volume.mount_point"));
		device_property_atomic_update_begin ();
		
		was_mounted =
			hal_device_property_get_bool (d, "volume.is_mounted");

		hal_device_property_set_bool (d, "volume.is_mounted",
					      FALSE);
		hal_device_property_set_string (d, "volume.mount_point",
						"");

		device_property_atomic_update_end ();

		if (was_mounted) {
			device_send_signal_condition (
				d, "VolumeUnmount",
				DBUS_TYPE_STRING,
				hal_device_property_get_string (
					d,
					"block.device"),
				DBUS_TYPE_STRING,
				device_mount_point,
				DBUS_TYPE_INVALID);

			/* Alrighty, we were unmounted and we are some media without
			 * partition tables and we don't like to be polled. So now
			 * the user could actually remove the media and insert some
			 * other media and we wouldn't notice. Ever.
			 *
			 * So, remove the hal device object to be on the safe side.
			 */
			if ( strcmp (hal_device_property_get_string (d, "block.device"),
				     hal_device_property_get_string (stor, "block.device")) == 0 &&
			     !hal_device_property_get_bool (stor, "storage.media_check_enabled")) {

				HAL_INFO (("Removing hal device object %s since it was unmounted", d->udi));

				/* remove device */
				g_signal_connect (d, "callouts_finished",
						  G_CALLBACK (volume_remove_from_gdl), NULL);
				hal_callout_device (d, FALSE);

				/* allow to scan again */
				hal_device_property_set_bool (stor, "block.have_scanned", FALSE);

				g_free (device_mount_point);
				return FALSE;
			}

		}

		g_free (device_mount_point);
		
	}

	return TRUE;
}


static gboolean
mtab_foreach_device (HalDeviceStore *store, HalDevice *d,
		     gpointer user_data)
{
	if (hal_device_has_capability (d, "volume")) {
		return mtab_handle_volume (d);
	} else if (hal_device_has_capability (d, "storage")) {
		return mtab_handle_storage (d);
	}

	return TRUE;
}


/** Load /etc/mtab and process all HAL storage and volume devices
 *  (that in turn each will process every entry) and set properties
 *  according to mount status. Also, optionally, sets up a watcher to
 *  do this whenever /etc/mtab changes
 *
 *  @param  force               Force reading of mtab
 */
static void
etc_mtab_process_all_block_devices (dbus_bool_t force)
{
	/* Start or continue watching /etc */
	if (!have_setup_watcher) {
		have_setup_watcher = TRUE;

		signal (SIGIO, sigio_handler);
		etc_fd = open ("/etc", O_RDONLY);
		fcntl (etc_fd, F_NOTIFY, DN_MODIFY | DN_MULTISHOT);
	}

	/* Just return if /etc/mtab wasn't modified */
	if (!read_etc_mtab (force))
		return;

	if (!force)
		HAL_INFO (("/etc/mtab changed, processing all block devices"));
	else {
		/*HAL_INFO (("processing /etc/mtab"));*/
	}

	hal_device_store_foreach (hald_get_gdl (), mtab_foreach_device, NULL);
}


/** Will be set to true by the SIGIO handler */
static dbus_bool_t sigio_etc_changed = FALSE;

/** Signal handler for watching /etc
 *
 *  @param  sig                 Signal number
 */
static void
sigio_handler (int sig)
{
	/* Set a variable instead of handling it now - this is *much* safer
	 * since this handler must be very careful - man signal for more 
	 * information 
	 */

	sigio_etc_changed = TRUE;
}


static void
block_class_removed (ClassDeviceHandler* self, 
		     const char *sysfs_path, 
		     HalDevice *d)
{
	HalDevice *child;

	if (hal_device_has_property (d, "block.is_volume")) {
		if (hal_device_property_get_bool (d, "block.is_volume")) {
			force_unmount (d);
		} else {
			force_unmount_of_all_childs (d);

			/* if we have no partitions our child is just
			 * made up here in HAL, so remove it
			 */
			if (hal_device_property_get_bool (d, "block.no_partitions")) {
				child = hal_device_store_match_key_value_string (
					hald_get_gdl (), "info.parent",
					hal_device_get_udi (d));

				if (child != NULL) {
					g_signal_connect (child, "callouts_finished",
							  G_CALLBACK (volume_remove_from_gdl), NULL);
					hal_callout_device (child, FALSE);
				}
			}

		}
	}
}


static gboolean
foreach_detect_media (HalDeviceStore *store, HalDevice *device,
		      gpointer user_data)
{
	/** @todo FIXME GDL was modifed so we have to break here because we
         *        are iterating over devices and this will break it
         */
	if (detect_media (device, FALSE))
		return FALSE;
	else
		return TRUE;
}

static void
block_class_tick (ClassDeviceHandler *self)
{
	/*HAL_INFO(("entering")); */

	hal_device_store_foreach (hald_get_gdl (), foreach_detect_media, NULL);

	/* check if the SIGIO signal handler delivered something to us */
	if (sigio_etc_changed) {
		/* acknowledge we got it */
		sigio_etc_changed = FALSE;

		HAL_INFO (("Directory /etc changed"));
		/* don't force reloading of /etc/mtab */
		etc_mtab_process_all_block_devices (FALSE);
	}

	/* HAL_INFO (("exiting")); */
}

/** Method specialisations for block device class */
ClassDeviceHandler block_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	block_class_tick,                   /**< timer function */
	block_class_accept,                 /**< accept function */
	block_class_visit,                  /**< visitor function */
	block_class_removed,                /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	block_class_pre_process,            /**< add more properties */
	class_device_post_merge,            /**< post merge function */
	block_class_got_udi,                /**< got UDI */
	block_class_compute_udi,            /**< UDI computation */
	block_class_in_gdl,                 /**< in GDL */
	"block",                            /**< sysfs class name */
	"block",                            /**< hal class name */
	TRUE,                               /**< require device file */
	FALSE                               /**< merge onto sysdevice */
};

/** @} */
