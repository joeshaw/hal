/***************************************************************************
 * CVSID: $Id$
 *
 * probe-storage.c : Probe storage devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>

#include "libhal/libhal.h"

#include "drive_id/drive_id.h"
#include "volume_id/volume_id.h"
#include "linux_dvd_rw_utils.h"

#include "shared.h"

void 
volume_id_log (const char *format, ...)
{
	va_list args;
	va_start (args, format);
	_do_dbg (format, args);
}

void 
drive_id_log (const char *format, ...)
{
	va_list args;
	va_start (args, format);
	_do_dbg (format, args);
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

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	char *bus;
	char *drive_type;
	struct volume_id *vid;
	dbus_bool_t only_check_for_fs;

	fd = -1;

	/* assume failure */
	ret = 1;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
	if ((bus = getenv ("HAL_PROP_STORAGE_BUS")) == NULL)
		goto out;
	if ((drive_type = getenv ("HAL_PROP_STORAGE_DRIVE_TYPE")) == NULL)
		goto out;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;

	if (argc == 2 && strcmp (argv[1], "--only-check-for-media") == 0)
		only_check_for_fs = TRUE;
	else
		only_check_for_fs = FALSE;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	dbg ("Doing probe-storage for %s (bus %s) (drive_type %s) (udi=%s) (--only-check-for-fs==%d)", 
	     device_file, bus, drive_type, udi, only_check_for_fs);

	if (!only_check_for_fs) {
		/* Only do drive_id on IDE and real SCSI disks - specifically
		 * not on USB which uses emulated SCSI since an INQUIRY on
		 * most USB devices may crash the storage device if the
		 * transfer length isn't exactly 36 bytes. See Red Hat bug
		 * #145256 for details.
		 */
		if (strcmp (bus, "ide") == 0 ||
		    strcmp (bus, "scsi") == 0) {
			struct drive_id *did;
			
			dbg ("Doing open (\"%s\", O_RDONLY | O_NONBLOCK)", device_file);
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
			if (fd < 0) {
				dbg ("Cannot open %s: %s", device_file, strerror (errno));
				goto out;
			}
			dbg ("Returned from open(2)");

			did = drive_id_open_fd (fd);
			if (drive_id_probe_all (did) == 0) {
				dbg ("serial = '%s', firmware = '%s'", did->serial, did->firmware);
				if (did->serial[0] != '\0') {
					dbus_error_init (&error);
					if (!libhal_device_set_property_string (ctx, udi, "storage.serial", 
										(char *) did->serial, &error)) {
						dbg ("Error setting storage.serial");
					}
				}
				
				if (did->firmware[0] != '\0') {
					dbus_error_init (&error);
					if (!libhal_device_set_property_string (ctx, udi, "storage.firmware_version", 
										(char *) did->firmware, &error)) {
						dbg ("Error setting storage.firmware_version");
					}
				}

				dbus_error_init (&error);

			}
			drive_id_close (did);

			close (fd);
		}

#if 0
		/* TODO: test for SATA drives once that gets exported to user space */
		{
			int fd;
			unsigned char unused;
			
			if ((fd = open (device_file, O_RDONLY|O_NDELAY)) != -1) {
				if (ioctl (fd, ATA_IOC_GET_IO32, &unused) >= 0) {
					hal_device_property_set_string (stordev, "storage.bus", "sata");
				}
				close (fd);
		}
		}
#endif

		/* Get properties for CD-ROM drive */
		if (strcmp (drive_type, "cdrom") == 0) {
			int capabilities;
			int read_speed, write_speed;
			
			dbg ("Doing open (\"%s\", O_RDONLY | O_NONBLOCK)", device_file);
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
			if (fd < 0) {
				dbg ("Cannot open %s: %s", device_file, strerror (errno));
				goto out;
			}
			dbg ("Returned from open(2)");

			if (ioctl (fd, CDROM_SET_OPTIONS, CDO_USE_FFLAGS) < 0) {
				dbg ("Error: CDROM_SET_OPTIONS failed: %s\n", strerror(errno));
				close (fd);
				goto out;
			}
			
			capabilities = ioctl (fd, CDROM_GET_CAPABILITY, 0);
			if (capabilities < 0) {
				close (fd);
				goto out;
			}
			
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.cdr", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.cdrw", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvd", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdr", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdrw", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdram", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusr", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrw", FALSE, &error);
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrdl", FALSE, &error);
			
			if (capabilities & CDC_CD_R) {
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.cdr", TRUE, &error);
			}
			
			if (capabilities & CDC_CD_RW) {
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.cdrw", TRUE, &error);
			}
			if (capabilities & CDC_DVD) {
				int profile;
				
				/** @todo FIXME BUG XXX: need to check for dvdrw (prolly need to rewrite much of 
				 *  the linux_dvdrw_utils.c file)
				 */
				
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvd", TRUE, &error);
				
				profile = get_dvd_r_rw_profile (fd);
				if (profile == 2) {
					libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusr", TRUE, &error);
					libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrw", TRUE, &error);
				} else if (profile == 0) {
					libhal_device_set_property_bool(ctx, udi, "storage.cdrom.dvdplusr", TRUE, &error);
				} else if (profile == 1) {
					libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrw", TRUE, &error);
				} else if (profile == 3) {
					libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusr", TRUE, &error);
					libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrdl", TRUE, &error);
				} else if (profile == 4) {
					libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusr", TRUE, &error);
                                	libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrw", TRUE, &error);
				        libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrdl", TRUE, &error);
				}
			}
			if (capabilities & CDC_DVD_R) {
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdr", TRUE, &error);
			}
			if (capabilities & CDC_DVD_RAM) {
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdram", TRUE, &error);
			}
			
			/* while we're at it, check if we support media changed */
			if (capabilities & CDC_MEDIA_CHANGED) {
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.support_media_changed", TRUE, &error);
			} else {
				libhal_device_set_property_bool (ctx, udi, "storage.cdrom.support_media_changed", FALSE, &error);
			}
			
			if (get_read_write_speed(fd, &read_speed, &write_speed) >= 0) {
				libhal_device_set_property_int (ctx, udi, "storage.cdrom.read_speed", read_speed, &error);
				if (write_speed > 0)
					libhal_device_set_property_int (ctx, udi, "storage.cdrom.write_speed", write_speed, &error);
				else
					libhal_device_set_property_int (ctx, udi, "storage.cdrom.write_speed", 0, &error);
			}

			close (fd);
		}
		
	} /* !only_check_for_fs */

	ret = 0;

	/* Also return 2 if we're a cdrom and we got a disc */
	if (strcmp (drive_type, "cdrom") == 0) {
		char *support_media_changed_str;
		int support_media_changed;
		int got_media;
		int drive;

		dbg ("Checking for optical disc on %s", device_file);

		support_media_changed_str = getenv ("HAL_PROP_STORAGE_CDROM_SUPPORT_MEDIA_CHANGED");
		if (support_media_changed_str != NULL && strcmp (support_media_changed_str, "true") == 0)
			support_media_changed = TRUE;
		else
			support_media_changed = FALSE;

		dbg ("Doing open (\"%s\", O_RDONLY | O_NONBLOCK | O_EXCL)", device_file);
		fd = open (device_file, O_RDONLY | O_NONBLOCK | O_EXCL);

		if (fd < 0 && errno == EBUSY) {
			/* this means the disc is mounted or some other app,
			 * like a cd burner, has already opened O_EXCL */
				
			/* HOWEVER, when starting hald, a disc may be
			 * mounted; so check /etc/mtab to see if it
			 * actually is mounted. If it is we retry to open
			 * without O_EXCL
			 */
			if (!is_mounted (device_file))
				goto out;

			dbg ("Doing open (\"%s\", O_RDONLY | O_NONBLOCK)", device_file);
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
		}

		if (fd < 0) {
			dbg ("open failed for %s: %s", device_file, strerror (errno));
			goto out;
		}

		got_media = FALSE;

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
			/* some CD-ROMs report CDS_DISK_OK even with an open
			 * tray; if media check has the same value two times in
			 * a row then this seems to be the case and we must not
			 * report that there is a media in it. */
			if (support_media_changed &&
			    ioctl (fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT) && 
			    ioctl (fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT)) {
			} else {
				got_media = TRUE;
			}
			break;
			
		case -1:
			dbg ("Error: CDROM_DRIVE_STATUS failed: %s\n", strerror(errno));
			break;
			
		default:
			break;
		}

		if (got_media)
			ret = 2;

		close (fd);
	} else {

		dbg ("Checking for file system on %s", device_file);

		/* See if we got a file system on the main block device - which 
		 * means doing a data (non O_NONBLOCK) open - this might fail, 
		 * especially if we don't have any media...
		 */
		dbg ("Doing open (\"%s\", O_RDONLY)", device_file);
		fd = open (device_file, O_RDONLY);
		if (fd < 0) {
			dbg ("Cannot open %s: %s", device_file, strerror (errno));
			goto out;
		}
		dbg ("Returned from open(2)");
		
		/* probe for file system */
		vid = volume_id_open_fd (fd);
		if (vid != NULL) {
			if (volume_id_probe_all (vid, 0, 0 /* size */) == 0) {
				/* signal to hald that we've found a file system and a fakevolume
				 * should be added - see hald/linux2/blockdev.c:add_blockdev_probing_helper_done()
				 * and hald/linux2/blockdev.c:block_rescan_storage_done().
				 */
				if (vid->usage_id == VOLUME_ID_FILESYSTEM)
					ret = 2;
			} else {
				;
			}
			volume_id_close(vid);
		}
		close (fd);
	}
	
out:

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
