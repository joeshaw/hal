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
#include "../callout.h"
#include "../hald_conf.h"
#include "class_device.h"
#include "common.h"
 
#include "volume_id/volume_id.h"
#include "linux_dvd_rw_utils.h"

/**
 * @defgroup HalDaemonLinuxBlock Block device class
 * @ingroup HalDaemonLinux
 * @brief Block device class
 * @{
 */

static void etc_mtab_process_all_block_devices (dbus_bool_t force);

static void detect_fs (HalDevice *d);

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


static void
block_class_visit (ClassDeviceHandler *self,
		   const char *path,
		   struct sysfs_class_device *class_device)
{
	HalDevice *d;
	char *parent_sysfs_path;
	ClassAsyncData *cad;

	/* only care about given sysfs class name */
	if (strcmp (class_device->classname, "block") != 0)
		return;

	d = hal_device_new ();
	hal_device_store_add (hald_get_tdl (), d);
	g_object_unref (d);

	hal_device_property_set_string (d, "info.bus", self->hal_class_name);
	hal_device_property_set_string (d, "linux.sysfs_path", path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", path);

	hal_device_property_set_bool (d, "block.no_partitions", FALSE);

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
	if (self->require_device_file && hald_is_initialising) {
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
	cad = g_new0 (ClassAsyncData, 1);
	cad->device = d;
	cad->handler = self;

	hal_device_store_match_key_value_string_async (
		hald_get_gdl (),
		"linux.sysfs_path_device",
		parent_sysfs_path,
		class_device_got_parent_device, cad,
		HAL_LINUX_HOTPLUG_TIMEOUT);

	hal_device_print (d);
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

	hal_device_property_set_bool (d, "storage.cdrom.cdr", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.cdrw", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvd", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdr", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdram", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdplusr", FALSE);
	hal_device_property_set_bool (d, "storage.cdrom.dvdplusrw", FALSE);

	hal_device_property_set_bool (
		d, 
		"storage.cdrom.eject_check_enabled", 
		(hald_get_conf ())->storage_cdrom_eject_check_enabled);

	if (capabilities & CDC_CD_R) {
		hal_device_property_set_bool (d, "storage.cdrom.cdr", TRUE);
	}
	
	if (capabilities & CDC_CD_RW) {
		hal_device_property_set_bool (d, "storage.cdrom.cdrw", TRUE);
	}
	if (capabilities & CDC_DVD) {
		int profile;
		
		hal_device_property_set_bool (d, "storage.cdrom.dvd", TRUE);
		
		profile = get_dvd_r_rw_profile (fd);
		HAL_INFO (("profile %d\n", profile));
		if (profile == 2) {
			hal_device_property_set_bool (d, "storage.cdrom.dvdplusr", TRUE);
			hal_device_property_set_bool (d, "storage.cdrom.dvdplusrw", TRUE);
		} else if (profile == 0) {
			hal_device_property_set_bool(d, "storage.cdrom.dvdplusr",
					     TRUE);
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

			/* Woohoo, have to change volume.mount_point *afterwards*, other
			 * wise device_mount_point points to garbage and D-BUS throws
			 * us off the bus, in fact it's doing exiting with code 1
			 * for us - not nice
			 */
			device_property_atomic_update_begin ();
			hal_device_property_set_string (d, "volume.mount_point",
						"");
			hal_device_property_set_string (d, "volume.fstype", "");
			hal_device_property_set_bool (d, "volume.is_mounted",
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

static void
volume_remove_from_gdl (HalDevice *device, gpointer user_data)
{
	g_signal_handlers_disconnect_by_func (device, volume_remove_from_gdl, 
					      user_data);
	hal_device_store_remove (hald_get_gdl (), device);
}


/** Check for media on a block device that is not a volume
 *
 *  @param  d                   Device to inspect; can be any device, but
 *                              it will only have effect if the device is
 *                              in the GDL and is of capability block and
 *                              is not a volume
 *  @param force_poll           If TRUE, do polling even though 
 *                              storage.media_check_enabled==FALSE
 *  @return                     TRUE iff the GDL was modified
 */
static dbus_bool_t
detect_media (HalDevice * d, dbus_bool_t force_poll)
{
	int fd;
	dbus_bool_t is_cdrom;
	const char *device_file;
	HalDevice *child;

	/* respect policy unless we force */
	if (!force_poll && 
	    !hal_device_property_get_bool (d, "storage.media_check_enabled"))
		return FALSE;

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
	is_cdrom = hal_device_has_property (d, "storage.drive_type") &&
	    strcmp (hal_device_property_get_string (d, "storage.drive_type"),
		    "cdrom") == 0
	    && hal_device_property_get_bool (d,
				     "storage.cdrom.support_media_changed");

	if (!is_cdrom) {
		fd = open (device_file, O_RDONLY);

		if (fd == -1) {
			/* open failed */
			/*HAL_WARNING (("open(\"%s\", O_RDONLY) failed, "
			  "errno=%d", device_file, errno));*/

			if (errno == ENOMEDIUM &&
			    hal_device_property_get_bool (
				    d, "block.no_partitions") ) {
				

				child = hal_device_store_match_key_value_string (
					hald_get_gdl (), "info.parent",
					hal_device_get_udi (d));

				if (child != NULL ) {
					force_unmount_of_all_childs (d);

					HAL_INFO (("Removing volume for "
						   "no_partitions device %s", 
						   device_file));

					g_signal_connect (child, "callouts_finished",
							  G_CALLBACK (volume_remove_from_gdl), NULL);
					hal_callout_device (child, FALSE);
					
					close (fd);

					/* GDL was modified */
					return TRUE;
				}
			} 
			
		} else if (hal_device_property_get_bool (
				   d, "block.no_partitions")) {

			/* For drives with partitions, we simply get hotplug
			 * events so only do something for e.g. floppies */

			/* media in drive; check if the HAL device representing
			 * the drive already got a child (it can have
			 * only one child)
			 */

			child = hal_device_store_match_key_value_string (
				hald_get_gdl (), "info.parent",
				hal_device_get_udi (d));

			if (child == NULL) {
				child = hal_device_store_match_key_value_string
					(hald_get_tdl (), "info.parent",
					hal_device_get_udi (d));
			}

			if (child == NULL) {				
				char udi[256];

				HAL_INFO (("Media in no_partitions device %s",
					   device_file));

				child = hal_device_new ();
				hal_device_store_add (hald_get_tdl (), child);
				g_object_unref (child);

				/* copy from parent */
				hal_device_merge (child, d);

				/* modify some properties */
				hal_device_property_set_string (
					child, "info.parent", d->udi);
				hal_device_property_set_bool (
					child, "block.is_volume", TRUE);
				hal_device_property_set_string (
					child, "info.capabilities",
					"block volume");
				hal_device_property_set_string (
					child, "info.category","volume");
				hal_device_property_set_string (
					child, "info.product", "Volume");
				/* clear these otherwise we'll
				 * imposter the parent on hotplug
				 * remove
				 */
				hal_device_property_set_string (
					child, "linux.sysfs_path", "");
				hal_device_property_set_string (
					child, "linux.sysfs_path_device", "");

				/* set defaults */
				hal_device_property_set_string (
					child, "volume.label", "");
				hal_device_property_set_string (
					child, "volume.uuid", "");
				hal_device_property_set_string (
					child, "volume.fstype", "");
				hal_device_property_set_string (
					child, "volume.mount_point", "");
				hal_device_property_set_bool (
					child, "volume.is_mounted", FALSE);
				hal_device_property_set_bool (
					child, "volume.is_disc", FALSE);

				/* set UDI as appropriate */
				strncpy (udi,
					 hal_device_property_get_string (
						 d, 
						 "info.udi"), 256);
				strncat (udi, "-volume", 256);
				hal_device_property_set_string (
					child, "info.udi", udi);
				hal_device_set_udi (child, udi);


				close(fd);
				
				detect_fs (child);
				
				/* If we have a nice volume label, set it */
				if (hal_device_has_property (child, 
							     "volume.label")) {
					const char *label;
					
					label = hal_device_property_get_string
						(child, "volume.label");
					
					if (label != NULL && 
					    label[0] != '\0') {
						hal_device_property_set_string
							(child, "info.product",
							 label);
					}
				}

				/* add new device */
				g_signal_connect (
					child, "callouts_finished",
					G_CALLBACK (device_move_from_tdl_to_gdl), NULL);
				hal_callout_device (child, TRUE);

				/* GDL was modified */
				return TRUE;

			} /* no child */

		} /* media in no_partitions device */
		
	} /* device is not an optical drive */
	else {
		int drive;
		dbus_bool_t got_disc = FALSE;
		dbus_bool_t eject_pressed = FALSE;
		struct request_sense sense;
		struct cdrom_generic_command cgc;
		unsigned char buffer[8];
		int ret;

		fd = open (device_file, O_RDONLY | O_NONBLOCK);

		if (fd == -1) {
			/* open failed */
			/*HAL_WARNING (("open(\"%s\", O_RDONLY|O_NONBLOCK|O_EXCL) failed, " "errno=%d", device_file, errno));*/
			return FALSE;
		}

		/* respect policy */
		if (hal_device_property_get_bool (
			    d, "storage.cdrom.eject_check_enabled")) {

			/* Check whether the 'eject' button is pressed..
			 * Supposedly only works on MMC-2 drivers or higher.
			 *
			 * From http://www.ussg.iu.edu/hypermail/linux/kernel/0202.0/att-0603/01-cd_poll.c
			 */
		
			memset (&cgc, 0, sizeof(struct cdrom_generic_command));
			memset (buffer, 0, sizeof(buffer));		
			cgc.cmd[0] = GPCMD_GET_EVENT_STATUS_NOTIFICATION;
			cgc.cmd[1] = 1;
			cgc.cmd[4] = 16;
			cgc.cmd[8] = sizeof (buffer);
			memset (&sense, 0, sizeof (sense));
			cgc.timeout = 600;
			cgc.buffer = buffer;
			cgc.buflen = sizeof (buffer);
			cgc.data_direction = CGC_DATA_READ;
			cgc.sense = &sense;
			cgc.quiet = 1;
			ret = ioctl (fd, CDROM_SEND_PACKET, &cgc);
			if (ret < 0) {
				HAL_ERROR (("GPCMD_GET_EVENT_STATUS_NOTIFICATION failed, errno=%d", errno));
			} else {
				if ((buffer[4]&0x0f) == 0x01) {
					HAL_INFO (("Eject pressed on udi=%s!", 
						   hal_device_get_udi (d)));
					/* handle later */
					eject_pressed = TRUE;
					
				}
			}
		}

		/* Check if a disc is in the drive
		 *
		 * @todo Use API above if MMC-2 or higher drive
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
				g_signal_connect (child, "callouts_finished",
						  G_CALLBACK (volume_remove_from_gdl), NULL);
				hal_callout_device (child, FALSE);

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
			child = hal_device_store_match_key_value_string (
				hald_get_tdl (), "info.parent",
				hal_device_get_udi (d));
		}

		
		/* handle eject if we already got a disc */
		if (child != NULL && eject_pressed ) {
			device_send_signal_condition (
				child,
				"EjectPressed",
				DBUS_TYPE_STRING,
				hal_device_property_get_string (
					child,
					"block.device"),
				DBUS_TYPE_INVALID);
			

		}

		if (child == NULL) {
			int type;
			char udi[256];

			/* nope, add child */
			HAL_INFO (("Adding volume for optical device %s",
				   device_file));

			child = hal_device_new ();
			hal_device_store_add (hald_get_tdl (), child);
			g_object_unref (child);

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
			/* clear these otherwise we'll imposter the parent
			 * on hotplug remove
			 */
			hal_device_property_set_string (
				child, "linux.sysfs_path", "");
			hal_device_property_set_string (
				child, "linux.sysfs_path_device", "");

			/* set defaults */
			hal_device_property_set_string (
				child, "volume.label", "");
			hal_device_property_set_string (
				child, "volume.uuid", "");
			hal_device_property_set_string (
				child, "volume.fstype", "");
			hal_device_property_set_string (
				child, "volume.mount_point", "");
			hal_device_property_set_bool (
				child, "volume.is_mounted", FALSE);
			hal_device_property_set_bool (
				child, "volume.is_disc", TRUE);
			hal_device_property_set_string (
				child, "volume.disc.type", "unknown");
			hal_device_property_set_bool (
				child, "volume.disc.has_audio", FALSE);
			hal_device_property_set_bool (
				child, "volume.disc.has_data", FALSE);
			hal_device_property_set_bool (
				child, "volume.disc.is_blank", FALSE);
			hal_device_property_set_bool (
				child, "volume.disc.is_appendable", FALSE);
			hal_device_property_set_bool (
				child, "volume.disc.is_rewritable", FALSE);


			/* set UDI as appropriate */
			strncpy (udi,
				 hal_device_property_get_string (d, "info.udi"),
				 256);
			strncat (udi, "-disc", 256);
			hal_device_property_set_string (child, "info.udi", udi);
			hal_device_set_udi (child, udi);

			/* check for audio/data/blank */
			type = ioctl (fd, CDROM_DISC_STATUS, CDSL_CURRENT);
			switch (type) {
			case CDS_AUDIO:		/* audio CD */
				hal_device_property_set_bool (
					child, "volume.disc.has_audio", TRUE);
				break;
			case CDS_MIXED:		/* mixed mode CD */
				hal_device_property_set_bool (
					child, "volume.disc.has_audio", TRUE);
				hal_device_property_set_bool (
					child, "volume.disc.has_data", TRUE);
				break;
			case CDS_DATA_1:	/* data CD */
			case CDS_DATA_2:
			case CDS_XA_2_1:
			case CDS_XA_2_2:
				hal_device_property_set_bool (
					child, "volume.disc.has_data", TRUE);
				break;
			case CDS_NO_INFO:	/* blank or invalid CD */
				hal_device_property_set_bool (
					child, "volume.disc.is_blank", TRUE);
				break;

			default:		/* should never see this */
				hal_device_property_set_string (child,
						"volume.disc_type",
						"unknown");
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
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"cd_rom");
					break;
				case 0x09: /* CD-R */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"cd_r");
					break;
				case 0x0a: /* CD-RW */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"cd_rw");
					hal_device_property_set_bool (
						child, 
						"volume.disc.is_rewritable", 
						TRUE);
					break;
				case 0x10: /* DVD-ROM */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_rom");
					break;
				case 0x11: /* DVD-R Sequential */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_r");
					break;
				case 0x12: /* DVD-RAM */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_ram");
					hal_device_property_set_bool (
						child, 
						"volume.disc.is_rewritable", 
						TRUE);
					break;
				case 0x13: /* DVD-RW Restricted Overwrite */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_rw");
					hal_device_property_set_bool (
						child, 
						"volume.disc.is_rewritable", 
						TRUE);
					break;
				case 0x14: /* DVD-RW Sequential */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_rw");
					hal_device_property_set_bool (
						child, 
						"volume.disc.is_rewritable", 
						TRUE);
					break;
				case 0x1A: /* DVD+RW */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_plus_rw");
					hal_device_property_set_bool (
						child, 
						"volume.disc.is_rewritable", 
						TRUE);
					break;
				case 0x1B: /* DVD+R */
					hal_device_property_set_string (
						child, 
						"volume.disc.type",
						"dvd_plus_r");
					hal_device_property_set_bool (
						child, 
						"volume.disc.is_rewritable", 
						TRUE);
					break;
				default: 
					break;
				}
			}

			if (disc_is_appendable (fd)) {
				hal_device_property_set_bool (
					child, 
					"volume.disc.is_appendable", 
					TRUE);
			}

			close(fd);

			detect_fs (child);

			/* If we have a nice volume label, set it */
			if (hal_device_has_property (child, "volume.label")) {
				const char *label;

				label = hal_device_property_get_string (
					child, "volume.label");

				if (label != NULL && label[0] != '\0') {
					hal_device_property_set_string (
						child, "info.product", label);
				}
			}

			/* add new device */
			g_signal_connect (child, "callouts_finished",
					  G_CALLBACK (device_move_from_tdl_to_gdl), NULL);
			hal_callout_device (child, TRUE);

			/* GDL was modified */
			return TRUE;
		}

	}			/* if( is optical drive ) */

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
detect_fs (HalDevice *d)
{
	struct volume_id *vid;
	const char *device_file;
	int rc;

	device_file = hal_device_property_get_string (d, "block.device");
	vid = volume_id_open_node(device_file);
	if (vid == NULL)
		return;

	rc = volume_id_probe(vid, ALL);
	if (rc != 0) {
		volume_id_close(vid);
		return;
	}

	hal_device_property_set_string (d, "volume.fstype", vid->fs_name);
	if (vid->label_string[0] != '\0')
		hal_device_property_set_string (d, "volume.label", vid->label_string);
	if (vid->uuid_string[0] != '\0')
		hal_device_property_set_string (d, "volume.uuid", vid->uuid_string);

	volume_id_close(vid);

	return;
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
		hal_device_property_set_string (
			stordev, "storage.bus", "unknown");

		hal_device_property_set_bool (
			stordev, 
			"storage.media_check_enabled", 
			(hald_get_conf ())->storage_media_check_enabled);

		hal_device_property_set_bool (
			stordev, 
			"storage.automount_enabled", 
			(hald_get_conf ())->storage_automount_enabled);

		hal_device_property_set_string (stordev, 
						"storage.model", "");
		hal_device_property_set_string (stordev, 
						"storage.vendor", "");

		/* walk up the device chain to find the physical device, 
		 * start with our parent. On the way, optionally pick up
		 * the scsi_device if it exists */
		udi_it = parent->udi;

		while (udi_it != NULL) {
			HalDevice *d_it;
			const char *bus;

			/* Find device */
			d_it = hal_device_store_find (hald_get_gdl (), udi_it);
			assert (d_it != NULL);

			/* Check info.bus */
			bus = hal_device_property_get_string (d_it,"info.bus");

			if (strcmp (bus, "scsi_device") == 0) {
				scsidev = d_it;
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
		char *volume_name;

		/* We are a volume */
		find_and_set_physical_device (d);
		hal_device_property_set_bool (d, "info.virtual", TRUE);
		hal_device_add_capability (d, "volume");
		hal_device_property_set_string (d, "info.category", "volume");
		hal_device_property_set_string (d, "volume.label", "");
		hal_device_property_set_string (d, "volume.uuid", "");
		hal_device_property_set_bool (d, "volume.is_disc", FALSE);
		hal_device_property_set_bool (d, "volume.is_mounted", FALSE);

		/* block device that is a partition; e.g. a storage volume */

		/* Detect filesystem and volume label */
		detect_fs (d);

		volume_name = g_strdup (
			hal_device_property_get_string (d, "volume.label"));

		if (volume_name == NULL || volume_name[0] == '\0') {
			const char *fstype;

			g_free (volume_name);

			fstype = hal_device_property_get_string (
				d,
				"volume.fstype");

			if (fstype == NULL || fstype[0] == '\0')
				volume_name = g_strdup ("Volume");
			else {
				volume_name = g_strdup_printf ("Volume (%s)",
							       fstype);
			}
		}

		hal_device_property_set_string (d, "info.product",
						volume_name);

		g_free (volume_name);

		/* Not much to do for volumes; our parent already set the
		 * appropriate values for the storage device backing us */

		return;
	} 


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
			} else if (strcmp (media, "tape") == 0) {
				has_removable_media = TRUE;
			}
			
		}
		
	} else if (strcmp (hal_device_property_get_string (parent, 
							 "info.bus"),
			   "scsi_device") == 0) {
		const char *sysfs_path;
		char attr_path[SYSFS_PATH_MAX];
		struct sysfs_attribute *attr;
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
				has_removable_media = TRUE;
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
			parent, "scsi_device.host");
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

			/* Indeed no partitions */
			hal_device_property_set_bool (d, 
						      "block.no_partitions",
						      TRUE);

			/* My experiments with my USB LaCie Floppy disk
			 * drive is that polling indeed work (Yay!), so
			 * we don't set storage.media_check_enabled to 
			 * FALSE - for devices where this doesn't work,
			 * we can override it with .fdi files
			 */
		}

	} else {
		/** @todo block device on non-IDE and non-SCSI device;
		 *  how to find the name and the media-type? 
		 */
		
		/* guestimate product name */
		hal_device_property_set_string (d, "info.product", "Disk");
		
	}
	
	
	hal_device_property_set_bool (
		stordev, 
		"storage.removable", 
		has_removable_media);

	if (hal_device_has_property (stordev, "storage.drive_type") &&
	    strcmp (hal_device_property_get_string (stordev, 
						    "storage.drive_type"), 
		    "cdrom") == 0) {
		hal_device_property_set_bool (d, "block.no_partitions", TRUE);
		cdrom_check (stordev, device_file);
		hal_device_add_capability (stordev, "storage.cdrom");
	}

	hal_device_property_set_string (stordev, "info.category", "storage");
	hal_device_add_capability (stordev, "storage");

	hal_device_property_set_bool (stordev, "storage.hotpluggable",
				      is_hotpluggable);

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
		 * storage.lun%d.* properties */
		if (scsidev != NULL) {
			int lun;
			char propname[64];
		
			lun = hal_device_property_get_int (
				scsidev, "scsi_device.lun");
		
			/* See 6in1-card-reader.fdi for an example */
		       		
			snprintf (propname, sizeof (propname), 
				  "storage_lun%d.", lun);

			hal_device_merge_with_rewrite (stordev, physdev, 
						       "storage.", propname);
		}
	}

	/* check for media on the device */
	detect_media (d, FALSE);

	/* Check the mtab to see if the device is mounted */
	etc_mtab_process_all_block_devices (TRUE);
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
	
	if (!hal_device_property_get_bool (d, "block.no_partitions"))
		return TRUE;

	if (!hal_device_has_property (d, "storage.media_check_enabled"))
		return TRUE;

	if (hal_device_property_get_bool (d, "storage.media_check_enabled"))
		return TRUE;

	HAL_INFO (("FOOO Checking for %s", 
		   hal_device_property_get_string (d, "block.device")));

	/* Only handle storage devices where block.no_parition==TRUE and
	 * where we can't check for media. Typically this is only legacy
	 * floppy drives on x86 boxes.
	 *
	 * So, in this very specific situation we maintain a child volume
	 * exactly when media is mounted.
	 */

	major = hal_device_property_get_int (d, "block.major");
	minor = hal_device_property_get_int (d, "block.minor");

	child = hal_device_store_match_key_value_string (
		hald_get_gdl (), "info.parent",
		hal_device_get_udi (d));

	/* Search all mount points */
	found_mount_point = FALSE;
	for (i = 0; i < num_mount_points; i++) {
		mp = &mount_points[i];
			
		if (mp->major == major && mp->minor == minor) {

			if (child == NULL ) {
				char udi[256];

				/* is now mounted, and we didn't have a child,
				 * so add the child */
				HAL_INFO (("%s now mounted at %s, "
					   "major:minor=%d:%d, "
					   "fstype=%s, udi=%s", 
					   mp->device, mp->mount_point, 
					   mp->major, mp->minor, 
					   mp->fs_type, d->udi));

				child = hal_device_new ();
				hal_device_store_add (hald_get_tdl (), child);
				g_object_unref (child);

				/* copy from parent */
				hal_device_merge (child, d);

				/* modify some properties */
				hal_device_property_set_string (
					child, "info.parent", d->udi);
				hal_device_property_set_bool (
					child, "block.is_volume", TRUE);
				hal_device_property_set_string (
					child, "info.capabilities",
					"block volume");
				hal_device_property_set_string (
					child, "info.category","volume");
				hal_device_property_set_string (
					child, "info.product", "Volume");
				/* clear these otherwise we'll
				 * imposter the parent on hotplug
				 * remove
				 */
				hal_device_property_set_string (
					child, "linux.sysfs_path", "");
				hal_device_property_set_string (
					child, "linux.sysfs_path_device", "");

				/* set defaults */
				hal_device_property_set_string (
					child, "volume.label", "");
				hal_device_property_set_string (
					child, "volume.uuid", "");
				hal_device_property_set_string (
					child, "volume.fstype",
					mp->fs_type);
				hal_device_property_set_string (
					child, "volume.mount_point",
					mp->mount_point);
				hal_device_property_set_bool (
					child, "volume.is_mounted", TRUE);
				hal_device_property_set_bool (
					child, "volume.is_disc", FALSE);

				/* set UDI as appropriate */
				strncpy (udi,
					 hal_device_property_get_string (
						 d, 
						 "info.udi"), 256);
				strncat (udi, "-volume", 256);
				hal_device_property_set_string (
					child, "info.udi", udi);
				hal_device_set_udi (child, udi);
				
				detect_fs (child);
				
				/* If we have a nice volume label, set it */
				if (hal_device_has_property (child, 
							     "volume.label")) {
					const char *label;
					
					label = hal_device_property_get_string
						(child, "volume.label");
					
					if (label != NULL && 
					    label[0] != '\0') {
						hal_device_property_set_string
							(child, "info.product",
							 label);
					}
				}

				/* add new device */
				g_signal_connect (
					child, "callouts_finished",
					G_CALLBACK (device_move_from_tdl_to_gdl), NULL);
				hal_callout_device (child, TRUE);

				/* GDL was modified */
				return TRUE;

			}
		}
	}

	/* No mount point found */
	if (!found_mount_point) {
		if (child != NULL ) {
			/* We had a child, but is no longer mounted, go
			 * remove the child */
			HAL_INFO (("%s not mounted anymore at %s, "
				   "major:minor=%d:%d, "
				   "fstype=%s, udi=%s", 
				   mp->device, mp->mount_point, 
				   mp->major, mp->minor, 
				   mp->fs_type, d->udi));
			
			g_signal_connect (child, "callouts_finished",
					  G_CALLBACK (volume_remove_from_gdl), NULL);
			hal_callout_device (child, FALSE);
					
			/* GDL was modified */
			return TRUE;
		}

		
	}

	return TRUE;
}

static gboolean
mtab_handle_volume (HalDevice *d)
{
	int major, minor;
	dbus_bool_t found_mount_point;
	struct mount_point_s *mp;
	int i;

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
			hal_device_property_get_bool (d, "volume.is_mounted");

		hal_device_property_set_bool (d, "volume.is_mounted",
					      FALSE);
		hal_device_property_set_string (d, "volume.mount_point",
						"");

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

	if (!force)
		HAL_INFO (("/etc/mtab changed, processing all block devices"));
	else
		HAL_INFO (("processing /etc/mtab"));

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
	class_device_in_gdl,                /**< in GDL */
	"block",                            /**< sysfs class name */
	"block",                            /**< hal class name */
	TRUE,                               /**< require device file */
	FALSE                               /**< merge onto sysdevice */
};

/** @} */
