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
 
#include "../hald.h"
#include "../hald_dbus.h"
#include "../logger.h"
#include "../device_store.h"
#include "class_device.h"
#include "common.h"
 
#include "linux_dvd_rw_utils.h"

/**
 * @defgroup HalDaemonLinuxBlock Block device class
 * @ingroup HalDaemonLinux
 * @brief Block device class
 * @{
 */

typedef struct {
	HalDevice *device;
	ClassDeviceHandler *handler;
} AsyncInfo;

static void
block_class_visit (ClassDeviceHandler *self,
		   const char *path,
		   struct sysfs_class_device *class_device,
		   dbus_bool_t is_probing)
{
	HalDevice *d;
	char *parent_sysfs_path;
	AsyncInfo *ai;

	/* only care about given sysfs class name */
	if (strcmp (class_device->classname, "block") != 0)
		return;

	d = hal_device_new ();
	hal_device_store_add (hald_get_tdl (), d);
	hal_device_property_set_string (d, "info.bus", self->hal_class_name);
	hal_device_property_set_string (d, "linux.sysfs_path", path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", path);

	if (class_device->sysdevice == NULL) {
		parent_sysfs_path = get_parent_sysfs_path (path);
		hal_device_property_set_bool (d, "block.is_volume", TRUE);
	} else {
		parent_sysfs_path = class_device->sysdevice->path;
		hal_device_property_set_bool (d, "block.is_volume", FALSE);
	}

	/* temporary property used for _udev_event() */
	hal_device_property_set_string (d, ".udev.sysfs_path", path);
	hal_device_property_set_string (d, ".udev.class_name", "block");

	/* Property name we should store the device file in */
	hal_device_property_set_string (d, ".target_dev", "block.device");

	/* Ask udev about the device file if we are probing */
	if (self->require_device_file && is_probing) {
		char dev_file[SYSFS_PATH_MAX];

		if (!class_device_get_device_file (path, dev_file, 
						   SYSFS_PATH_MAX)) {
			HAL_WARNING (("Couldn't get device file for class "
				      "device with sysfs path", path));
			return;
		}

		/* If we are not probing this function will be called upon
		 * receiving a dbus event */
		self->udev_event (self, d, dev_file);
	}

	/* Now find the parent device; this happens asynchronously as it
	 * might be added later. */
	ai = g_new0 (AsyncInfo, 1);
	ai->device = d;
	ai->handler = self;
		
	hal_device_store_match_key_value_string_async (
		hald_get_gdl (),
		"linux.sysfs_path_device",
		parent_sysfs_path,
		class_device_got_parent_device, ai,
		is_probing ? 0 : HAL_LINUX_HOTPLUG_TIMEOUT);
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
cdrom_check(HalDevice *d, const char *device_file)
{
	int fd, capabilities;
	int read_speed, write_speed;

	/* Check handling */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;
		
	ioctl (fd, CDROM_SET_OPTIONS, CDO_USE_FFLAGS);
		
	capabilities = ioctl (fd, CDROM_GET_CAPABILITY, 0);
	if (capabilities < 0) {
		close(fd);
		return;
	}

	if (capabilities & CDC_CD_R) {
		hal_device_add_capability (d, "storage.cdr");
		hal_device_property_set_bool (d, "storage.cdr", TRUE);
	}
	
	if (capabilities & CDC_CD_RW) {
		hal_device_add_capability (d, "storage.cdrw");
		hal_device_property_set_bool (d, "storage.cdrw", TRUE);
	}
	if (capabilities & CDC_DVD) {
		int profile;
		
		hal_device_add_capability (d, "storage.dvd");
		hal_device_property_set_bool (d, "storage.dvd", TRUE);
		
		profile = get_dvd_r_rw_profile (fd);
		HAL_INFO (("profile %d\n", profile));
		if (profile == 2) {
			hal_device_add_capability (d, "storage.dvdplusr");
			hal_device_property_set_bool (d, "storage.dvdplusr", TRUE);
			hal_device_add_capability (d, "storage.dvdplusrw");
			hal_device_property_set_bool (d, "storage.dvdplusrw", TRUE);
		} else if (profile == 0) {
			hal_device_add_capability(d, "storage.dvdplusr");
			hal_device_property_set_bool(d, "storage.dvdplusr",
					     TRUE);
		} else if (profile == 1) {
			hal_device_add_capability (d, "storage.dvdplusrw");
			hal_device_property_set_bool (d, "storage.dvdplusrw", TRUE);
		}
	}
	if (capabilities & CDC_DVD_R) {
		hal_device_add_capability (d, "storage.dvdr");
		hal_device_property_set_bool (d, "storage.dvdr", TRUE);
	}
	if (capabilities & CDC_DVD_RAM) {
		hal_device_add_capability (d, "storage.dvdram");
		hal_device_property_set_bool (d, "storage.dvdram", TRUE);
	}
	
	/* while we're at it, check if we support media changed */
	if (ioctl (fd, CDROM_MEDIA_CHANGED) >= 0) {
		hal_device_property_set_bool (d, "storage.cdrom.support_media_changed", TRUE);
	}
	
	if (get_read_write_speed(fd, &read_speed, &write_speed) >= 0) {
		hal_device_property_set_int (d, "storage.cdrom.read_speed", read_speed);
		if (write_speed > 0)
			hal_device_property_set_int(d, "storage.cdrom.write_speed", write_speed);
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
	    hal_device_property_get_string (d, "block.mount_point");

	umount_argv[2] = device_file;

	if (hal_device_has_property (d, "block.is_volume") &&
	    hal_device_property_get_bool (d, "block.is_volume") &&
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
			HAL_INFO (("/bin/umount returned %d",
				   umount_exitcode));
		} else {
			/* Tell clients we are going to unmount so they close
			 * can files - otherwise this unmount is going to stall
			 *
			 * One candidate for catching this would be FAM - the
			 * File Alteration Monitor
			 *
			 * Lazy unmount been in Linux since 2.4.11, so we're
			 * homefree (but other OS'es might not support this)
			 */
			HAL_INFO (("Goint to emit BlockForcedUnmountPartition('%s', '%s', TRUE)", device_file, device_mount_point));
			device_send_signal_condition (d,
						      "BlockForcedUnmountPartition",
						      DBUS_TYPE_STRING,
						      device_file,
						      DBUS_TYPE_STRING,
						      device_mount_point,
						      DBUS_TYPE_BOOLEAN,
						      TRUE,
						      DBUS_TYPE_INVALID);

			/* Woohoo, have to change block.mount_point *afterwards*, other
			 * wise device_mount_point points to garbage and D-BUS throws
			 * us off the bus, in fact it's doing exiting with code 1
			 * for us - not nice
			 */
			device_property_atomic_update_begin ();
			hal_device_property_set_string (d, "block.mount_point",
						"");
			hal_device_property_set_string (d, "block.fstype", "");
			hal_device_property_set_bool (d, "block.is_mounted",
					      FALSE);
			device_property_atomic_update_end ();
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

		}		/* for all children */

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

		/* Finally, send a single signal on the device - this
		 * is useful for desktop policy clients such as g-v-m
		 * such that only a single annoying "dude, you need to
		 * *stop* the device before pulling it out" popup is
		 * displayed */
		HAL_INFO (("Goint to emit BlockForcedUnmount('%s')",
			   device_file));
		device_send_signal_condition (d, "BlockForcedUnmount",
					      DBUS_TYPE_STRING, device_file,
					      DBUS_TYPE_INVALID);

	}			/* childs!=NULL */
}


/** Check for media on a block device that is not a volume
 *
 *  @param  d                   Device to inspect; can be any device, but
 *                              it will only have effect if the device is
 *                              in the GDL and is of capability block and
 *                              is not a volume
 *  @param                      TRUE iff the GDL was modified
 */
static dbus_bool_t
detect_media (HalDevice * d)
{
	int fd;
	dbus_bool_t is_cdrom;
	const char *device_file;
	HalDevice *child;

	/* need to be in GDL */
	if (!hal_device_store_find (hald_get_gdl (),
				    hal_device_get_udi (d)))
	    return FALSE;
	    
	/* need to have a device and not be a volume */
	if (!hal_device_has_property (d, "block.is_volume") ||
	    !hal_device_has_property (d, "block.device") ||
	    hal_device_property_get_bool (d, "block.is_volume"))
		return FALSE;

	device_file = hal_device_property_get_string (d, "block.device");
	if (device_file == NULL)
		return FALSE;

	/* we do special treatment for optical discs */
	is_cdrom = hal_device_has_property (d, "storage.media") &&
	    strcmp (hal_device_property_get_string (d, "storage.media"),
		    "cdrom") == 0
	    && hal_device_property_get_bool (d,
				     "storage.cdrom.support_media_changed");

	if (!is_cdrom) {
		fd = open (device_file, O_RDONLY);

		if (fd == -1) {
			/* open failed */
			HAL_WARNING (("open(\"%s\", O_RDONLY) failed, "
				      "errno=%d", device_file, errno));

			if (errno == ENOMEDIUM) {
				force_unmount_of_all_childs (d);
			}

		}

	} /* device is not an optical drive */
	else {
		int drive;
		dbus_bool_t got_disc = FALSE;

		fd = open (device_file, O_RDONLY | O_NONBLOCK | O_EXCL);

		if (fd == -1) {
			/* open failed */
			HAL_WARNING (("open(\"%s\", O_RDONLY|O_NONBLOCK|O_EXCL) failed, " "errno=%d", device_file, errno));
			return FALSE;
		}

		drive = ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
		switch (drive) {
			/* explicit fallthrough */
		case CDS_NO_INFO:
		case CDS_NO_DISC:
		case CDS_TRAY_OPEN:
		case CDS_DRIVE_NOT_READY:
			break;

		case CDS_DISC_OK:
			got_disc = TRUE;
			break;

		default:
			break;
		}

		if (!got_disc) {
			/* we get to here if there is no disc in the drive */
			child = hal_device_store_match_key_value_string (
				hald_get_gdl (), "info.parent",
				hal_device_get_udi (d));

			if (child != NULL) {
				HAL_INFO (("Removing volume for optical device %s", device_file));
				hal_device_store_remove (hald_get_gdl (), child);
				g_object_unref (child);

				close (fd);

				/* GDL was modified */
				return TRUE;
			}

			close (fd);
			return FALSE;
		}


		/* got a disc in drive, */

		/* disc in drive; check if the HAL device representing
		 * the optical drive already got a child (it can have
		 * only one child)
		 */

		child = hal_device_store_match_key_value_string (
			hald_get_gdl (), "info.parent",
			hal_device_get_udi (d));

		if (child == NULL) {
			int type;
			char udi[256];

			/* nope, add child */
			HAL_INFO (("Adding volume for optical device %s",
				   device_file));

			child = hal_device_new ();

			/* copy from parent */
			hal_device_merge (child, d);

			/* modify some properties */
			hal_device_property_set_string (child, "info.parent",
						d->udi);
			hal_device_property_set_bool (child, "block.is_volume",
					      TRUE);
			hal_device_property_set_string (child, "info.capabilities",
						"block volume");
			hal_device_property_set_string (child, "info.category",
						"volume");
			hal_device_property_set_string (child, "info.product",
						"Disc");

			/* set UDI as appropriate */
			strncpy (udi,
				 hal_device_property_get_string (d, "info.udi"),
				 256);
			strncat (udi, "-disc", 256);
			hal_device_property_set_string (child, "info.udi", udi);
			hal_device_set_udi (child, udi);

			/* set disc media type as appropriate */
			type = ioctl (fd, CDROM_DISC_STATUS, CDSL_CURRENT);
			close(fd);
			switch (type) {
			case CDS_AUDIO:		/* audio CD */
				hal_device_property_set_string (child,
						"storage.cdrom.media_type",
						"audio");
				break;
			case CDS_MIXED:		/* mixed mode CD */
				hal_device_property_set_string (child,
						"storage.cdrom.media_type",
						"mixed");
				break;
			case CDS_DATA_1:	/* data CD */
			case CDS_DATA_2:
			case CDS_XA_2_1:
			case CDS_XA_2_2:
				hal_device_property_set_string (child,
						"storage.cdrom.media_type",
						"data");
				break;
			case CDS_NO_INFO:	/* blank or invalid CD */
				hal_device_property_set_string (child,
						"storage.cdrom.media_type",
						"blank");
				break;

			default:		/* should never see this */
				hal_device_property_set_string (child,
						"storage.cdrom.media_type",
						"unknown");
				break;
			}


			/* add new device */
			hal_device_store_add (hald_get_gdl (), child);
			g_object_unref (child);

			/* GDL was modified */
			return TRUE;
		}

	}			/* if( is optical drive ) */

	close (fd);

	return FALSE;
}

static void 
block_class_post_process (ClassDeviceHandler *self,
			  HalDevice *d,
			  const char *sysfs_path,
			  struct sysfs_class_device *class_device)
{
	int major, minor;
	HalDevice *parent;
	HalDevice *stordev;
	char *stordev_udi;
	const char *device_file;

	parent = hal_device_store_find (hald_get_gdl (),
					hal_device_property_get_string (d, "info.parent"));
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
		stordev_udi = parent->udi;
		stordev = parent;
	} else {
		stordev_udi = d->udi;
		stordev = d;
	}

	hal_device_property_set_string (d, "block.storage_device",
					stordev_udi);

	if (hal_device_property_get_bool (d, "block.is_volume")) {
		/* We are a volume */
		find_and_set_physical_device (d);
		hal_device_property_set_bool (d, "info.virtual", TRUE);
		hal_device_add_capability (d, "volume");
		hal_device_property_set_string (d, "info.category", "volume");

		/* block device that is a partition; e.g. a storage volume */

		/** @todo  Guestimate product name; use volume label */
		hal_device_property_set_string (d, "info.product", "Volume");

	} else {
		dbus_bool_t removable_media = FALSE;

		/* be pessimistic */
		hal_device_property_set_bool (stordev, "storage.cdr", FALSE);
		hal_device_property_set_bool (stordev, "storage.cdrw", FALSE);
		hal_device_property_set_bool (stordev, "storage.dvd", FALSE);
		hal_device_property_set_bool (stordev, "storage.dvdr", FALSE);
		hal_device_property_set_bool (stordev, "storage.dvdram", FALSE);

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

			ide_name = get_last_element (hal_device_property_get_string
						     (d, "linux.sysfs_path"));

			model = read_single_line ("/proc/ide/%s/model",
						  ide_name);
			if (model != NULL) {
				hal_device_property_set_string (stordev, "storage.model", model);
				hal_device_property_set_string (d, "info.product",
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
				hal_device_property_set_string (stordev, "storage.media",
							media);

				/* Set for removable media */
				if (strcmp (media, "disk") == 0) {
					hal_device_add_capability (stordev, "storage");
					hal_device_property_set_string (stordev,
							       "info.category",
								"storage");
				} else if (strcmp (media, "cdrom") == 0) {

					hal_device_add_capability (stordev, "storage");
					hal_device_add_capability (stordev,
							   "storage.removable");
					hal_device_property_set_string (stordev,
							      "info.category",
							  "storage.removable");

					removable_media = TRUE;
				} else if (strcmp (media, "floppy") == 0) {
					hal_device_add_capability (stordev, "storage");
					hal_device_add_capability (stordev,
							  "storage.removable");
					hal_device_property_set_string (stordev,
						       	"info.category",
						       	"storage.removable");
					removable_media = TRUE;
				} else if (strcmp (media, "tape") == 0) {
					hal_device_add_capability (stordev, "storage");
					hal_device_add_capability (stordev,
							  "storage.removable");
					hal_device_property_set_string (stordev,
							      "info.category",
							  "storage.removable");
					removable_media = TRUE;
				}

			}

		} 
		else if (strcmp (hal_device_property_get_string (parent, "info.bus"),
				 "scsi_device") == 0) {
			const char *sysfs_path;
			char attr_path[SYSFS_PATH_MAX];
			struct sysfs_attribute *attr;
			
			sysfs_path = hal_device_property_get_string (
				d, 
				"linux.sysfs_path");

			snprintf (attr_path, SYSFS_PATH_MAX,
				  "%s/device/vendor", sysfs_path);
			attr = sysfs_open_attribute (attr_path);
			if (sysfs_read_attribute (attr) >= 0) {
				hal_device_property_set_string (d, "info.vendor",
							strip_space (attr->
								     value));
				sysfs_close_attribute (attr);
			}

			snprintf (attr_path, SYSFS_PATH_MAX,
				  "%s/device/model", sysfs_path);
			attr = sysfs_open_attribute (attr_path);
			if (sysfs_read_attribute (attr) >= 0) {
				hal_device_property_set_string (d, "info.product",
							strip_space (attr->
								     value));
				sysfs_close_attribute (attr);
			}

			snprintf (attr_path, SYSFS_PATH_MAX,
				  "%s/device/type", sysfs_path);
			attr = sysfs_open_attribute (attr_path);
			if (sysfs_read_attribute (attr) >= 0) {
				int type = parse_dec (attr->value);
				switch (type) {
				case 0:	/* Disk */
					hal_device_add_capability (stordev, "storage");
					hal_device_property_set_string (
						stordev, "info.category", "storage");
					hal_device_property_set_string (
						stordev, "storage.media", "disk");
					break;
				case 1:	/* Tape */
					hal_device_add_capability (stordev, "storage");
					hal_device_add_capability (
						stordev, "storage.removable");
					hal_device_property_set_string (
						stordev, "info.category",
						"storage.removable");
					hal_device_property_set_string (
						stordev,
						"storage.media", "tape");
					removable_media = TRUE;
					break;
				case 5:	/* CD-ROM */
					hal_device_add_capability (stordev, "storage");
					hal_device_add_capability (
						stordev, "storage.removable");
					hal_device_property_set_string (
						stordev, "storage.media", "cdrom");
					hal_device_property_set_string (
						stordev, "info.category",
						"storage.removable");

					removable_media = TRUE;
					break;
				default:
					/** @todo add more SCSI types */
					HAL_WARNING (("Don't know how to "
						      "handle SCSI type %d", 
						      type));
				}
			}
		} else {
			/** @todo block device on non-IDE and non-SCSI device;
			 *  how to find the name and the media-type? Right now
			 *  we just assume that the disk is fixed and of type
			 *  flash.
			 *       
			 */
			
			hal_device_property_set_string (stordev, "storage.media",
						"flash");
			
			hal_device_add_capability (stordev, "storage");
			hal_device_property_set_string (stordev, "info.category",
						"storage");
			
			/* guestimate product name */
			hal_device_property_set_string (d, "info.product", "Disk");
			
		}

		hal_device_property_set_bool (stordev, "storage.removable", removable_media);
	}


	if (hal_device_has_property (stordev, "storage.media") &&
	    strcmp (hal_device_property_get_string (stordev, "storage.media"), "cdrom") == 0) {
		cdrom_check (stordev, device_file);
	}
					
	/* check for media on the device */
	detect_media (d);

}

static char *
block_class_compute_udi (HalDevice * d, int append_num)
{
        char *format;
        static char buf[256];
 
        if (append_num == -1)
                format = "/org/freedesktop/Hal/devices/block_%d_%d";
        else
                format = "/org/freedesktop/Hal/devices/block_%d_%d-%d";
 
        snprintf (buf, 256, format,
                  hal_device_property_get_int (d, "block.major"),
                  hal_device_property_get_int (d, "block.minor"), append_num);
 
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
		return FALSE;
	}

	if (!force && etc_mtab_mtime == stat_buf.st_mtime) {
		/*printf("No modification, etc_mtab_mtime=%d\n", etc_mtab_mtime); */
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

	close (fd);

	return TRUE;
}

static void sigio_handler (int sig);

/** Global to see if we have setup the watcher on /etc */
static dbus_bool_t have_setup_watcher = FALSE;

static gboolean
foreach_block_device (HalDeviceStore *store, HalDevice *d,
		      gpointer user_data)
{
	const char *bus;
	int major, minor;
	dbus_bool_t found_mount_point;
	struct mount_point_s *mp;
	int i;

	bus = hal_device_property_get_string (d, "info.bus");
	if (bus == NULL ||
	    strncmp (bus, "block", 5) != 0 ||
	    !hal_device_property_get_bool (d, "block.is_volume"))
		return TRUE;

	major = hal_device_property_get_int (d, "block.major");
	minor = hal_device_property_get_int (d, "block.minor");

	/* Search all mount points */
	found_mount_point = FALSE;
	for (i = 0; i < num_mount_points; i++) {
		mp = &mount_points[i];
			
		if (mp->major == major && mp->minor == minor) {
			const char *existing_block_device;
			dbus_bool_t was_mounted;

			HAL_INFO (("%s mounted at %s, major:minor=%d:%d, fstype=%s, udi=%s", mp->device, mp->mount_point, mp->major, mp->minor, mp->fs_type, d->udi));

			device_property_atomic_update_begin ();

			existing_block_device =
				hal_device_property_get_string (d,
								"block.device");

			was_mounted =
				hal_device_property_get_bool (d,
							      "block.is_mounted");

			/* Yay! Found a mount point; set properties accordingly */
			hal_device_property_set_string (d,
							"block.mount_point",
							mp->mount_point);
			hal_device_property_set_string (d, "block.fstype",
							mp->fs_type);
			hal_device_property_set_bool (d,
						      "block.is_mounted",
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
				device_send_signal_condition (
					d,
					"BlockMountEvent",
					DBUS_TYPE_STRING,
					hal_device_property_get_string (
						d,
						"block.device"),
					DBUS_TYPE_STRING,
					mp->mount_point,
					DBUS_TYPE_STRING,
					mp->fs_type,
					DBUS_TYPE_INVALID);
			}

			found_mount_point = TRUE;
			break;
		}
	}

	/* No mount point found; (possibly) remove all information */
	if (!found_mount_point) {
		dbus_bool_t was_mounted;

		device_property_atomic_update_begin ();
		
		was_mounted =
			hal_device_property_get_bool (d, "block.is_mounted");

		hal_device_property_set_bool (d, "block.is_mounted",
					      FALSE);
		hal_device_property_set_string (d, "block.mount_point",
						"");
		hal_device_property_set_string (d, "block.fstype", "");

		device_property_atomic_update_end ();

		if (was_mounted) {
			device_send_signal_condition (
				d, "BlockUnmountEvent",
				DBUS_TYPE_STRING,
				hal_device_property_get_string (
					d,
					"block.device"),
				DBUS_TYPE_INVALID);
		}
		
	}

	return TRUE;
}

/** Load /etc/mtab and process all HAL block devices and set properties
 *  according to mount status. Also, optionally, sets up a watcher to do
 *  this whenever /etc/mtab changes
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

	HAL_INFO (("/etc/mtab changed, processing all block devices"));

	hal_device_store_foreach (hald_get_gdl (), foreach_block_device, NULL);
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
	if (hal_device_has_property (d, "block.is_volume")) {
		if (hal_device_property_get_bool (d, "block.is_volume")) {
			force_unmount (d);
		} else {
			force_unmount_of_all_childs (d);
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
		if (detect_media (device))
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

static void
block_class_detection_done (ClassDeviceHandler *self)
{
	etc_mtab_process_all_block_devices (TRUE);
}

/** Method specialisations for block device class */
ClassDeviceHandler block_class_handler = {
	class_device_init,                  /**< init function */
	block_class_detection_done,        /**< detection is done */
	class_device_shutdown,              /**< shutdown function */
	block_class_tick,                   /**< timer function */
	block_class_visit,                  /**< visitor function */
	block_class_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	block_class_post_process,           /**< add more properties */
	block_class_compute_udi,            /**< UDI computation */
	"block",                            /**< sysfs class name */
	"block",                            /**< hal class name */
	TRUE,                               /**< require device file */
	FALSE                               /**< merge onto sysdevice */
};

/** @} */
