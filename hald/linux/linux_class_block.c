/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_block.c : Block device handling on Linux 2.6
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#include <glib.h>

#define _GNU_SOURCE 1
#include <linux/fcntl.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"
#include "linux_class_block.h"
#include "linux_dvd_rw_utils.h"

/* fwd decl */
static void etc_mtab_process_all_block_devices (dbus_bool_t force);
static dbus_bool_t detect_media (HalDevice * d);

/**
 * @defgroup HalDaemonLinuxBlock Block class
 * @ingroup HalDaemonLinux
 * @brief Block class
 * @{
 */

/** This function will compute the device uid based on other properties
 *  of the device. For block device it's simply block appended with the
 *  major and minor number
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char *
block_compute_udi (HalDevice * d, int append_num)
{
	char *format;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/block_%d_%d";
	else
		format = "/org/freedesktop/Hal/devices/block_%d_%d-%d";

	snprintf (buf, 256, format,
		  ds_property_get_int (d, "block.major"),
		  ds_property_get_int (d, "block.minor"), append_num);

	return buf;
}

/* fwd decl */
static void visit_class_device_block_got_parent (HalDevice * parent,
						 void *data1, void *data2);

/** Visitor function for block device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void
visit_class_device_block (const char *path,
			  struct sysfs_class_device *class_device)
{
	HalDevice *d;
	char *parent_sysfs_path;
	char attr_name[SYSFS_NAME_LEN];
	struct sysfs_attribute *cur;
	dbus_bool_t is_disk = FALSE;
	dbus_bool_t not_partition = FALSE;

	if (sysfs_get_classdev_attr (class_device, "dev") == NULL) {
		/* Must have major:minor number before we are interested */
		/*HAL_INFO(("Block device with sysfs path %s doesn't have major:minor",
		   path)); */
		return;
	}

	d = ds_device_new ();
	ds_property_set_string (d, "info.bus", "block");
	ds_property_set_string (d, "linux.sysfs_path", path);
	ds_property_set_string (d, "linux.sysfs_path_device", path);

	ds_add_capability (d, "block");

	dlist_for_each_data (sysfs_get_classdev_attributes (class_device),
			     cur, struct sysfs_attribute) {
		if (sysfs_get_name_from_path
		    (cur->path, attr_name, SYSFS_NAME_LEN) != 0)
			continue;

		if (strcmp (attr_name, "dev") == 0) {
			int major, minor;
			if (sscanf (cur->value, "%d:%d", &major, &minor) ==
			    2) {
				is_disk = TRUE;
				ds_property_set_int (d, "block.major",
						     major);
				ds_property_set_int (d, "block.minor",
						     minor);
			}
		} else if (strcmp (attr_name, "size") == 0) {
			ds_property_set_int (d, "block.size",
					     parse_dec (cur->value));
		} else if (strcmp (attr_name, "start") == 0) {
			ds_property_set_int (d, "block.start",
					     parse_dec (cur->value));
		} else if (strcmp (attr_name, "range") == 0) {
			not_partition = TRUE;
		}
	}
	ds_property_set_bool (d, "block.is_volume", !not_partition);

    /** @todo FIXME is a block always 512 bytes?? Must check kernel source */
	ds_property_set_int (d, "block.block_size", 512);

	if (class_device->sysdevice == NULL) {
		/* there is no sys device corresponding to us.. this means we
		 * must be the child of another block device ie. a partition 
		 */

		/* check if our parent is there; example: if we are sda1 then
		 * our parent is sda
		 */

		parent_sysfs_path = get_parent_sysfs_path (path);
	} else {
		/* There is a sys device corresponding to us; This is normally
		 * an IDE interface or a SCSI device */

		/* Now find the corresponding physical device */

		parent_sysfs_path = class_device->sysdevice->path;
	}


	/* Find parent; this happens asynchronously as our parent might
	 * be added later. If we are probing this can't happen so the
	 * timeout is set to zero in that event..
	 */
	ds_device_async_find_by_key_value_string
	    ("linux.sysfs_path_device", parent_sysfs_path, TRUE,
	     visit_class_device_block_got_parent, (void *) d,
	     /*(void*) parent_sysfs_path */ NULL,
	     is_probing ? 0 : HAL_LINUX_HOTPLUG_TIMEOUT);

	/*HAL_INFO(("*** finding parent_sysfs_path=%s, 0x%08x, callback=0x%08x", 
	   parent_sysfs_path, parent_sysfs_path, 
	   visit_class_device_block_got_parent));
	 */
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

/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void
visit_class_device_block_got_parent (HalDevice * parent,
				     void *data1, void *data2)
{
	char *new_udi = NULL;
	HalDevice *new_d = NULL;
	HalDevice *d = (HalDevice *) data1;

	HAL_INFO (("data2=0x%08x, d=0x%08x, d->udi=%s, parent->udi=%s, parent->in_gdl=%d", data2, d, d->udi, parent != NULL ? parent->udi : "no parent", parent != NULL ? parent->in_gdl : 42));
	HAL_INFO (("d: linux.sysfs_path=%s",
		   ds_property_get_string (d, "linux.sysfs_path")));
	HAL_INFO (("d: linux.sysfs_path_device=%s",
		   ds_property_get_string (d, "linux.sysfs_path_device")));

	if (parent == NULL) {
		HAL_WARNING (("No parent for block device!"));
		ds_device_destroy (d);
		return;
	}
	ds_property_set_string (d, "info.parent", parent->udi);


	/* Ask udev about the device file if we are probing.. otherwise 
	 * we'll just receive a dbus message from udev later */
	if (is_probing) {
		int i;
		const char *path;
		int sysfs_mount_path_len;
		char sysfs_path_trunc[SYSFS_NAME_LEN];
		char *udev_argv[7] =
		    { udevinfo_path (), "-r", "-q", "name", "-p",
			sysfs_path_trunc, NULL
		};
		char *udev_stdout;
		char *udev_stderr;
		int udev_exitcode;

		path = ds_property_get_string (d, "linux.sysfs_path");

		/* compute truncated sysfs path */
		sysfs_mount_path_len = strlen (sysfs_mount_path);
		if (strlen (path) > sysfs_mount_path_len) {
			strncpy (sysfs_path_trunc,
				 path + sysfs_mount_path_len,
				 SYSFS_NAME_LEN);
		}
		HAL_INFO (("*** sysfs_path_trunc = '%s'",
			   sysfs_path_trunc));

		/* Now invoke udevinfo */
		if (udev_argv[0] == NULL || g_spawn_sync ("/",
							  udev_argv,
							  NULL,
							  0,
							  NULL,
							  NULL,
							  &udev_stdout,
							  &udev_stderr,
							  &udev_exitcode,
							  NULL) != TRUE) {
			HAL_ERROR (("Couldn't invoke udevinfo"));
			goto error;
		}

		if (udev_exitcode != 0) {
			HAL_ERROR (("%s returned %d", udevinfo_path (),
				    udev_exitcode));
			goto error;
		}

		/* sanitize string returned by udev */
		for (i = 0; udev_stdout[i] != 0; i++) {
			if (udev_stdout[i] == '\r'
			    || udev_stdout[i] == '\n') {
				udev_stdout[i] = 0;
				break;
			}
		}

		HAL_INFO (("device file = '%s'", udev_stdout));

		ds_property_set_string (d, "block.device", udev_stdout);

	/** @todo FIXME free udev_stdout, udev_stderr? */
	} else {
	error:
		/* woow, be careful not to overwrite */
		if (!ds_property_exists (d, "block.device")) {
			ds_property_set_string (d, "block.device", "");
		}
	}



	if (ds_property_get_bool (d, "block.is_volume")) {
		/* We are a volume */
		find_and_set_physical_device (d);
		ds_property_set_bool (d, "info.virtual", TRUE);
		ds_add_capability (d, "volume");
		ds_property_set_string (d, "info.category", "volume");

		/* block device that is a partition; e.g. a storage volume */

		/** @todo  Guestimate product name; use volume label */
		ds_property_set_string (d, "info.product", "Volume");

		/* update storage.removable.media_inserted if applicable */

/*
        if( ds_property_get_bool(parent, "storage.removable") )
        {
            ds_property_set_bool(parent, "storage.removable.media_inserted", 
                                 TRUE)
        }
*/

	} else {
		dbus_bool_t removable_media = FALSE;

		/* be pessimistic */
		ds_property_set_bool (d, "storage.cdr", FALSE);
		ds_property_set_bool (d, "storage.cdrw", FALSE);
		ds_property_set_bool (d, "storage.dvd", FALSE);
		ds_property_set_bool (d, "storage.dvdr", FALSE);
		ds_property_set_bool (d, "storage.dvdram", FALSE);

		/* We are a disk or cdrom drive; maybe we even offer 
		 * removable media 
		 */
		ds_property_set_string (d, "info.category", "block");

		HAL_INFO (("Bus type is %s!",
			   ds_property_get_string (parent, "info.bus")));

		if (strcmp
		    (ds_property_get_string (parent, "info.bus"),
		     "ide") == 0) {
			const char *ide_name;
			char *model;
			char *media;

			ide_name =
			    get_last_element (ds_property_get_string
					      (d, "linux.sysfs_path"));

			model =
			    read_single_line ("/proc/ide/%s/model",
					      ide_name);
			if (model != NULL) {
				ds_property_set_string (d, "storage.model",
							model);
				ds_property_set_string (d, "info.product",
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
			
			media =
			    read_single_line ("/proc/ide/%s/media",
					      ide_name);
			if (media != NULL) {
				ds_property_set_string (d, "storage.media",
							media);

				/* Set for removable media */
				if (strcmp (media, "disk") == 0) {
					ds_add_capability (d, "storage");
					ds_property_set_string (d,
							       "info.category",
								"storage");
				} else if (strcmp (media, "cdrom") == 0) {
					ds_add_capability (d, "storage");
					ds_add_capability (d,
							   "storage.removable");
					ds_property_set_string (d,
							      "info.category",
							  "storage.removable");

					removable_media = TRUE;
				} else if (strcmp (media, "floppy") == 0) {
					ds_add_capability (d, "storage");
					ds_add_capability (d,
							  "storage.removable");
					ds_property_set_string (d,
						       	"info.category",
						       	"storage.removable");
					removable_media = TRUE;
				} else if (strcmp (media, "tape") == 0) {
					ds_add_capability (d, "storage");
					ds_add_capability (d,
							  "storage.removable");
					ds_property_set_string (d,
							      "info.category",
							  "storage.removable");
					removable_media = TRUE;
				}

			}

		} 
		else if (strcmp (ds_property_get_string (parent,"info.bus"),
				 "scsi_device") == 0) {
			const char *sysfs_path;
			char attr_path[SYSFS_PATH_MAX];
			struct sysfs_attribute *attr;
			
			sysfs_path = ds_property_get_string (
				d, 
				"linux.sysfs_path");
			
			snprintf (attr_path, SYSFS_PATH_MAX,
				  "%s/device/vendor", sysfs_path);
			
			attr = sysfs_open_attribute (attr_path);
			
			if (sysfs_read_attribute (attr) >= 0) {
				ds_property_set_string (d, "info.vendor",
							strip_space (attr->
								     value));
				sysfs_close_attribute (attr);
			}
			
			snprintf (attr_path, SYSFS_PATH_MAX,
				  "%s/device/model", sysfs_path);
			
			attr = sysfs_open_attribute (attr_path);
			
			if (sysfs_read_attribute (attr) >= 0) {
				ds_property_set_string (d, "info.product",
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
					ds_add_capability (d, "storage");
					ds_property_set_string (
						d, "info.category", "storage");
					ds_property_set_string (
						d, "storage.media", "disk");
					break;
				case 1:	/* Tape */
					ds_add_capability (d, "storage");
					ds_add_capability (
						d, "storage.removable");
					ds_property_set_string (
						d, "info.category",
						"storage.removable");
					ds_property_set_string (
						d,
						"storage.media", "tape");
					removable_media = TRUE;
					break;
				case 5:	/* CD-ROM */
					ds_add_capability (d, "storage");
					ds_add_capability (
						d, "storage.removable");
					ds_property_set_string (
						d, "storage.media", "cdrom");
					ds_property_set_string (
						d, "info.category",
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
			
			ds_property_set_string (d, "storage.media",
						"flash");
			
			ds_add_capability (d, "storage");
			ds_property_set_string (d, "info.category",
						"storage");
			
			/* guestimate product name */
			ds_property_set_string (d, "info.product", "Disk");
			
		}

		ds_property_set_bool (d, "storage.removable",
				      removable_media);
/*
        if( removable_media )
        {
            ds_property_set_bool(d, "storage.removable.media_inserted", 
                                 FALSE);
        }
*/
	}

	/* check /etc/mtab, forces reload of the file */
	etc_mtab_process_all_block_devices (TRUE);
	
	new_udi = rename_and_merge (d, block_compute_udi, "block");
	if (new_udi != NULL) {
		new_d = ds_device_find (new_udi);
		if (new_d != NULL) {
			linux_class_block_check_if_ready_to_add (new_d);
		}
	}
}

/** Check if all the required properties are in place so we can announce
 *  this device to the world.
 *
 *  @param  d                   Device
 */
void
linux_class_block_check_if_ready_to_add (HalDevice * d)
{
	const char *parent;
	const char *device_file;

	/* we know, a'priori, that the only thing we possibly get later
	 * is block.device, so just check for the presence of this
	 */

	/* but do check that we already got our parent sorted out to avoid
	 * a race between udev add and find parent */
	parent = ds_property_get_string (d, "info.parent");
	if (parent == NULL)
		return;

	device_file = ds_property_get_string (d, "block.device");
	HAL_INFO (("Entering, udi=%s, device_file=%s", d->udi,
		   device_file));
	
	if (device_file != NULL && strcmp (device_file, "") != 0) {
		const char *media;
		
		/* now we have block.device (and storage.media since got_parent
		 * was called) 
		 */
		media = ds_property_get_string (d, "storage.media");
		if (media != NULL && strcmp (media, "cdrom") == 0) {
			int fd, capabilities;

			/* Check handling */
			fd = open (device_file, O_RDONLY | O_NONBLOCK);

			ioctl (fd, CDROM_SET_OPTIONS, CDO_USE_FFLAGS);

			if (fd >= 0) {
				capabilities =
				    ioctl (fd, CDROM_GET_CAPABILITY, 0);

				if (capabilities >= 0) {
					int read_speed, write_speed;

					if (capabilities & CDC_CD_R) {
						ds_add_capability (
							d,
							"storage.cdr");
						ds_property_set_bool (
							d,
							"storage.cdr", TRUE);
					}
					if (capabilities & CDC_CD_RW) {
						ds_add_capability (
							d,
							"storage.cdrw");
						ds_property_set_bool (
							d,
							"storage.cdrw", TRUE);
					}
					if (capabilities & CDC_DVD) {
						int profile;

						ds_add_capability (
							d,
							"storage.dvd");
						ds_property_set_bool (
							d,
							"storage.dvd", TRUE);

						profile =
						    get_dvd_r_rw_profile (fd);
						HAL_WARNING (("profile %d\n", profile));
						if (profile == 2) {
							ds_add_capability
							    (d, "storage.dvdplusr");
							ds_property_set_bool
							    (d, "storage.dvdplusr",
							     TRUE);
							ds_add_capability
							    (d, "storage.dvdplusrw");
							ds_property_set_bool
							    (d, "storage.dvdplusrw",
							     TRUE);
						} else if (profile == 0) {
							ds_add_capability
							    (d, "storage.dvdplusr");
							ds_property_set_bool
							    (d, "storage.dvdplusr",
							     TRUE);
						} else if (profile == 1) {
							ds_add_capability
							    (d, "storage.dvdplusrw");
							ds_property_set_bool
							    (d, "storage.dvdplusrw",
							     TRUE);
						}
					}
					if (capabilities & CDC_DVD_R) {
						ds_add_capability (d, "storage.dvdr");
						ds_property_set_bool (d, "storage.dvdr",
								      TRUE);
					}
					if (capabilities & CDC_DVD_RAM) {
						ds_add_capability (d, "storage.dvdram");
						ds_property_set_bool (d, "storage.dvdram",
								      TRUE);
					}

					/* while we're at it, check if we support media changed */
					if (ioctl (fd, CDROM_MEDIA_CHANGED)
					    >= 0) {
						ds_property_set_bool (d, "storage.cdrom.support_media_changed",
								      TRUE);
					}

					if (get_read_write_speed
					    (fd, &read_speed,
					     &write_speed) >= 0) {
						ds_property_set_int (d, "storage.cdrom.read_speed",
								     read_speed);
						if (write_speed > 0)
							ds_property_set_int
							    (d, "storage.cdrom.write_speed",
							     write_speed);
					}
				}
				close (fd);
			}
		}

		if (ds_property_get_bool (d, "block.is_volume")) {
			/* the parent block device always carries storage.* */
			ds_property_set_string (d, "block.storage_device",
						parent);
		} else {
			/* if we are not a volume we are the top block device and
			 * thus also carry the storage properties so we point to
			 * ourselves (this may change later; e.g. we want the
			 * physical USB device to carry the storage properties)
			 */
			ds_property_set_string (d, "block.storage_device",
						d->udi);
		}


		ds_gdl_add (d);

		/* check for media on the device */
		detect_media (d);
	}
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

/** Load /etc/mtab and process all HAL block devices and set properties
 *  according to mount status. Also, optionally, sets up a watcher to do
 *  this whenever /etc/mtab changes
 *
 *  @param  force               Force reading of mtab
 */
static void
etc_mtab_process_all_block_devices (dbus_bool_t force)
{
	int i;
	const char *bus;
	HalDevice *d;
	int major, minor;
	dbus_bool_t found_mount_point;
	struct mount_point_s *mp;
	HalDeviceIterator diter;

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

	/* Iterate over all HAL devices */
	for (ds_device_iter_begin (&diter);
	     ds_device_iter_has_more (&diter);
	     ds_device_iter_next (&diter)) {

		d = ds_device_iter_get (&diter);

		bus = ds_property_get_string (d, "info.bus");
		if (bus == NULL || strcmp (bus, "block") != 0 ||
		    !ds_property_get_bool (d, "block.is_volume"))
			continue;

		major = ds_property_get_int (d, "block.major");
		minor = ds_property_get_int (d, "block.minor");

		/* Search all mount points */
		found_mount_point = FALSE;
		for (i = 0; i < num_mount_points; i++) {
			mp = &mount_points[i];

			if (mp->major == major && mp->minor == minor) {
				const char *existing_block_device;
				dbus_bool_t was_mounted;

				HAL_INFO (("%s mounted at %s, major:minor=%d:%d, fstype=%s, udi=%s", mp->device, mp->mount_point, mp->major, mp->minor, mp->fs_type, d->udi));

				property_atomic_update_begin ();

				existing_block_device =
				    ds_property_get_string (d,
							    "block.device");

				was_mounted =
				    ds_property_get_bool (d,
							  "block.is_mounted");

				/* Yay! Found a mount point; set properties accordingly */
				ds_property_set_string (d,
							"block.mount_point",
							mp->mount_point);
				ds_property_set_string (d, "block.fstype",
							mp->fs_type);
				ds_property_set_bool (d,
						      "block.is_mounted",
						      TRUE);

				/* only overwrite block.device if it's not set */
				if (existing_block_device == NULL ||
				    (existing_block_device != NULL &&
				     strcmp (existing_block_device,
					     "") == 0)) {
					ds_property_set_string (d,
								"block.device",
								mp->
								device);
				}

				property_atomic_update_end ();

				if (!was_mounted) {
					emit_condition (d,
							"BlockMountEvent",
							DBUS_TYPE_STRING,
							ds_property_get_string
							(d,
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

			property_atomic_update_begin ();

			was_mounted =
			    ds_property_get_bool (d, "block.is_mounted");

			ds_property_set_bool (d, "block.is_mounted",
					      FALSE);
			ds_property_set_string (d, "block.mount_point",
						"");
			ds_property_set_string (d, "block.fstype", "");

			property_atomic_update_end ();

			if (was_mounted) {
				emit_condition (d, "BlockUnmountEvent",
						DBUS_TYPE_STRING,
						ds_property_get_string (d,
									"block.device"),
						DBUS_TYPE_INVALID);
			}

		}
	}
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

/** Init function for block device handling
 *
 */
void
linux_class_block_init ()
{
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

	device_file = ds_property_get_string (d, "block.device");
	device_mount_point =
	    ds_property_get_string (d, "block.mount_point");

	umount_argv[2] = device_file;

	if (ds_property_exists (d, "block.is_volume") &&
	    ds_property_get_bool (d, "block.is_volume") &&
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
			emit_condition (d, "BlockForcedUnmountPartition",
					DBUS_TYPE_STRING, device_file,
					DBUS_TYPE_STRING,
					device_mount_point,
					DBUS_TYPE_BOOLEAN, TRUE,
					DBUS_TYPE_INVALID);

			/* Woohoo, have to change block.mount_point *afterwards*, other
			 * wise device_mount_point points to garbage and D-BUS throws
			 * us off the bus, in fact it's doing exiting with code 1
			 * for us - not nice
			 */
			property_atomic_update_begin ();
			ds_property_set_string (d, "block.mount_point",
						"");
			ds_property_set_string (d, "block.fstype", "");
			ds_property_set_bool (d, "block.is_mounted",
					      FALSE);
			property_atomic_update_end ();
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
	int num_childs;
	const char *device_file;
	HalDevice *child;
	HalDevice **childs;

	device_file = ds_property_get_string (d, "block.device");

	childs =
	    ds_device_find_multiple_by_key_value_string ("info.parent",
							 d->udi, TRUE,
							 &num_childs);
	if (childs != NULL) {
		int n;

		for (n = 0; n < num_childs; n++) {
			child = childs[n];

			force_unmount (child);

		}		/* for all childs */

		free (childs);

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
		emit_condition (d, "BlockForcedUnmount",
				DBUS_TYPE_STRING, device_file,
				DBUS_TYPE_INVALID);

	}			/* childs!=NULL */
}



/** Called when this device is about to be removed
 *
 *  @param  d                   Device
 */
void
linux_class_block_removed (HalDevice * d)
{
	if (ds_property_exists (d, "block.is_volume")) {
		if (ds_property_get_bool (d, "block.is_volume")) {
			force_unmount (d);
		} else {
			force_unmount_of_all_childs (d);
		}
	}
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

	/* need to be in GDL, need to have block.deve and 
	 * have block.is_volume==FALSE 
	 */
	if (!d->in_gdl ||
	    (!ds_property_exists (d, "block.is_volume")) ||
	    (!ds_property_exists (d, "block.device")) ||
	    ds_property_get_bool (d, "block.is_volume"))
		return FALSE;

	device_file = ds_property_get_string (d, "block.device");
	if (device_file == NULL)
		return FALSE;

	/* we do special treatment for optical discs */
	is_cdrom = ds_property_exists (d, "storage.media") &&
	    strcmp (ds_property_get_string (d, "storage.media"),
		    "cdrom") == 0
	    && ds_property_get_bool (d,
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
			child =
			    ds_device_find_by_key_value_string
			    ("info.parent", d->udi, TRUE);

			if (child != NULL) {
				HAL_INFO (("Removing volume for optical device %s", device_file));
				ds_device_destroy (child);

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

		close (fd);

		child = ds_device_find_by_key_value_string ("info.parent",
							    d->udi, TRUE);
		if (child == NULL) {
			char udi[256];

			/* nope, add child */
			HAL_INFO (("Adding volume for optical device %s",
				   device_file));

			child = ds_device_new ();

			/* copy from parent */
			ds_device_merge (child, d);

			/* modify some properties */
			ds_property_set_string (child, "info.parent",
						d->udi);
			ds_property_set_bool (child, "block.is_volume",
					      TRUE);
			ds_property_set_string (child, "info.capabilities",
						"block volume");
			ds_property_set_string (child, "info.category",
						"volume");
			ds_property_set_string (child, "info.product",
						"Disc");

			/* set UDI as appropriate */
			strncpy (udi,
				 ds_property_get_string (d, "info.udi"),
				 256);
			strncat (udi, "-disc", 256);
			ds_property_set_string (child, "info.udi", udi);
			ds_device_set_udi (child, udi);

			/* add new device */
			ds_gdl_add (child);

			/* GDL was modified */
			return TRUE;
		}

	}			/* if( is optical drive ) */

	close (fd);

	return FALSE;
}

/** Timeout handler for polling for media on block devices
 *
 *  @param  data                User data when setting up timer
 *  @return                     TRUE iff timer should be kept
 */
static gboolean
media_detect_timer_handler (gpointer data)
{
	int fd;
	const char *device_file;
	HalDevice *d;
	HalDeviceIterator iter;

	/*HAL_INFO(("entering")); */

	/* Iterate all devices */
	for (ds_device_iter_begin (&iter);
	     ds_device_iter_has_more (&iter);
	     ds_device_iter_next (&iter)) {
		d = ds_device_iter_get (&iter);

	/** @todo FIXME GDL was modifed so we have to break here because we
         *        are iterating over devices and this will break it
         */
		if (detect_media (d))
			break;
	}

	/* check if the SIGIO signal handler delivered something to us */
	if (sigio_etc_changed) {
		/* acknowledge we got it */
		sigio_etc_changed = FALSE;

		HAL_INFO (("Directory /etc changed"));
		/* don't force reloading of /etc/mtab */
		etc_mtab_process_all_block_devices (FALSE);
	}

	HAL_INFO (("exiting"));

	return TRUE;
}


/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void
linux_class_block_detection_done ()
{
	g_timeout_add (2000, media_detect_timer_handler, NULL);

	etc_mtab_process_all_block_devices (TRUE);
}

/** Shutdown function for block device handling
 *
 */
void
linux_class_block_shutdown ()
{

}
