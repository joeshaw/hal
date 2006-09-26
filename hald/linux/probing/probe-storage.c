/***************************************************************************
 * CVSID: $Id$
 *
 * probe-storage.c : Probe storage devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Danny Kukawka, <danny.kukawka@web.de>
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib.h>
#include <libvolume_id.h>

#include "libhal/libhal.h"
#include "partutil/partutil.h"
#include "linux_dvd_rw_utils.h"

#include "../../logger.h"

static void vid_log(int priority, const char *file, int line, const char *format, ...)
{
	char log_str[1024];
	va_list args;

	va_start(args, format);
	vsnprintf(log_str, sizeof(log_str), format, args);
	logger_forward_debug("%s:%i %s\n", file, line, log_str);
	va_end(args);
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
	char *sysfs_path;
	dbus_bool_t only_check_for_fs;
	LibHalChangeSet *cs;

	cs = NULL;

	fd = -1;

	/* hook in our debug into libvolume_id */
	volume_id_log_fn = vid_log;

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
	if ((sysfs_path = getenv ("HAL_PROP_LINUX_SYSFS_PATH")) == NULL)
		goto out;

	setup_logger ();

	if (argc == 2 && strcmp (argv[1], "--only-check-for-media") == 0)
		only_check_for_fs = TRUE;
	else
		only_check_for_fs = FALSE;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	cs = libhal_device_new_changeset (udi);
	if (cs == NULL) {
		HAL_DEBUG(("Cannot initialize changeset"));
		goto out;
	}

	HAL_DEBUG (("Doing probe-storage for %s (bus %s) (drive_type %s) (udi=%s) (--only-check-for-fs==%d)", 
	     device_file, bus, drive_type, udi, only_check_for_fs));

	if (!only_check_for_fs) {
		/* Get properties for CD-ROM drive */
		if (strcmp (drive_type, "cdrom") == 0) {
			int capabilities;
			int read_speed, write_speed;
			char *write_speeds;
			
			HAL_DEBUG (("Doing open (\"%s\", O_RDONLY | O_NONBLOCK)", device_file));
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
			if (fd < 0) {
				HAL_ERROR (("Cannot open %s: %s", device_file, strerror (errno)));
				goto out;
			}
			HAL_DEBUG (("Returned from open(2)"));

			if (ioctl (fd, CDROM_SET_OPTIONS, CDO_USE_FFLAGS) < 0) {
				HAL_ERROR (("Error: CDROM_SET_OPTIONS failed: %s\n", strerror(errno)));
				close (fd);
				goto out;
			}
			
			capabilities = ioctl (fd, CDROM_GET_CAPABILITY, 0);
			if (capabilities < 0) {
				close (fd);
				goto out;
			}
			HAL_DEBUG (("CDROM_GET_CAPABILITY returned: 0x%08x", capabilities));
			
			libhal_changeset_set_property_bool (cs, "storage.cdrom.cdr", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.cdrw", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvd", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdr", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdrw", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdram", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusr", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusrw", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusrwdl", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusrdl", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.bd", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.bdr", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.bdre", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.hddvd", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.hddvdr", FALSE);
			libhal_changeset_set_property_bool (cs, "storage.cdrom.hddvdrw", FALSE);
			
			if (capabilities & CDC_CD_R) {
				libhal_changeset_set_property_bool (cs, "storage.cdrom.cdr", TRUE);
			}
			
			if (capabilities & CDC_CD_RW) {
				libhal_changeset_set_property_bool (cs, "storage.cdrom.cdrw", TRUE);
			}
			if (capabilities & CDC_DVD) {
				int profile;
				
				libhal_changeset_set_property_bool (cs, "storage.cdrom.dvd", TRUE);

				profile = get_dvd_r_rw_profile (fd);
				HAL_DEBUG (("get_dvd_r_rw_profile returned: %d", profile));

				if (profile & DRIVE_CDROM_CAPS_DVDRW)
					libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdrw", TRUE);
				if (profile & DRIVE_CDROM_CAPS_DVDPLUSR)
					libhal_changeset_set_property_bool(cs, "storage.cdrom.dvdplusr", TRUE);
				if (profile & DRIVE_CDROM_CAPS_DVDPLUSRW)
					libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusrw", TRUE);
				if (profile & DRIVE_CDROM_CAPS_DVDPLUSRWDL)
					libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusrwdl", TRUE);
				if (profile & DRIVE_CDROM_CAPS_DVDPLUSRDL)
                                        libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdplusrdl", TRUE);
			}
			if (capabilities & CDC_DVD_R) {
				libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdr", TRUE);
			}
			if (capabilities & CDC_DVD_RAM) {
				libhal_changeset_set_property_bool (cs, "storage.cdrom.dvdram", TRUE);
			}
			
			/* while we're at it, check if we support media changed */
			if (capabilities & CDC_MEDIA_CHANGED) {
				libhal_changeset_set_property_bool (cs, "storage.cdrom.support_media_changed", TRUE);
			} else {
				libhal_changeset_set_property_bool (cs, "storage.cdrom.support_media_changed", FALSE);
			}
			
			if (get_read_write_speed(fd, &read_speed, &write_speed, &write_speeds) >= 0) {
				libhal_changeset_set_property_int (cs, "storage.cdrom.read_speed", read_speed);
				if (write_speed > 0) {
					libhal_changeset_set_property_int (
						cs, "storage.cdrom.write_speed", write_speed);
					
					if (write_speeds != NULL)
					{
						gchar **wspeeds;
 						wspeeds = g_strsplit_set (write_speeds, ",", -1);
						libhal_changeset_set_property_strlist (cs, 
										       "storage.cdrom.write_speeds", 
										       (const char **) wspeeds);
						g_strfreev (wspeeds);
						free (write_speeds);
					}
				} else {
					gchar *wspeeds[1] = {NULL};
					libhal_changeset_set_property_int (cs, "storage.cdrom.write_speed", 0);
					libhal_changeset_set_property_strlist (cs, "storage.cdrom.write_speeds", 
									       (const char **) wspeeds);
				}
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

		HAL_DEBUG (("Checking for optical disc on %s", device_file));

		support_media_changed_str = getenv ("HAL_PROP_STORAGE_CDROM_SUPPORT_MEDIA_CHANGED");
		if (support_media_changed_str != NULL && strcmp (support_media_changed_str, "true") == 0)
			support_media_changed = TRUE;
		else
			support_media_changed = FALSE;

		HAL_DEBUG (("Doing open (\"%s\", O_RDONLY | O_NONBLOCK | O_EXCL)", device_file));
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

			HAL_DEBUG (("Doing open (\"%s\", O_RDONLY | O_NONBLOCK)", device_file));
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
		}

		if (fd < 0) {
			HAL_DEBUG (("open failed for %s: %s", device_file, strerror (errno)));
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
			HAL_ERROR (("Error: CDROM_DRIVE_STATUS failed: %s\n", strerror(errno)));
			break;
			
		default:
			break;
		}

		if (got_media) {
			uint64_t size;
			ret = 2;
			libhal_changeset_set_property_bool (cs, "storage.removable.media_available", TRUE);
			if (ioctl (fd, BLKGETSIZE64, &size) == 0) {
				HAL_DEBUG (("media size = %llu", size));
				libhal_changeset_set_property_uint64 (cs, "storage.removable.media_size", size);
			}
		} else {
			libhal_changeset_set_property_bool (cs, "storage.removable.media_available", FALSE);
		}

		close (fd);
	} else {
		struct volume_id *vid;
		GDir *dir;
		const gchar *partition;
		const gchar *main_device;
		size_t main_device_len;
		uint64_t size;

		HAL_DEBUG (("Checking for file system on %s", device_file));

		/* See if we got a file system on the main block device - which 
		 * means doing a data (non O_NONBLOCK) open - this might fail, 
		 * especially if we don't have any media...
		 */
		HAL_DEBUG (("Doing open (\"%s\", O_RDONLY)", device_file));
		fd = open (device_file, O_RDONLY);
		if (fd < 0) {
			HAL_DEBUG (("Cannot open %s: %s", device_file, strerror (errno)));
			/* no media */
			libhal_changeset_set_property_bool (cs, "storage.removable.media_available", FALSE);
			goto out;
		}
		HAL_DEBUG (("Returned from open(2)"));

		/* if we get to here, we have media */
		libhal_changeset_set_property_bool (cs, "storage.removable.media_available", TRUE);

		if (ioctl (fd, BLKGETSIZE64, &size) != 0)
			size = 0;
		
		libhal_changeset_set_property_uint64 (cs, "storage.removable.media_size", size);

		/* if the kernel has created partitions, we don't look for a filesystem */
		main_device = strrchr (sysfs_path, '/');
		if (main_device == NULL)
			goto out;
		main_device = &main_device[1];
		main_device_len = strlen (main_device);
		HAL_DEBUG (("look for existing partitions for %s", main_device));
		if ((dir = g_dir_open (sysfs_path, 0, NULL)) == NULL) {
			HAL_DEBUG (("failed to open sysfs dir"));
			goto out;
		}
		while ((partition = g_dir_read_name (dir)) != NULL) {
			if (strncmp (main_device, partition, main_device_len) == 0 &&
			    isdigit (partition[main_device_len])) {
				PartitionTable *p;

				HAL_DEBUG (("partition %s found, skip probing for filesystem", partition));
				g_dir_close (dir);

				/* probe for partition table type */
				p = part_table_load_from_disk (device_file);
				if (p != NULL) {

					libhal_changeset_set_property_string (
						cs, 
						"storage.partitioning_scheme", 
						part_get_scheme_name (part_table_get_scheme (p)));

					part_table_free (p);
				}

				goto out;
			}
		}
		g_dir_close (dir);

		libhal_changeset_set_property_string (cs, "storage.partitioning_scheme", "none");

		/* probe for file system */
		vid = volume_id_open_fd (fd);
		if (vid != NULL) {
			if (volume_id_probe_all (vid, 0, size) == 0) {
				/* signal to hald that we've found something and a fakevolume
				 * should be added - see hald/linux/blockdev.c:add_blockdev_probing_helper_done()
				 * and hald/linux/blockdev.c:block_rescan_storage_done().
				 */
				if (vid->usage_id == VOLUME_ID_FILESYSTEM ||
				    vid->usage_id == VOLUME_ID_RAID ||
				    vid->usage_id == VOLUME_ID_OTHER ||
				    vid->usage_id == VOLUME_ID_CRYPTO)
					ret = 2;
			} else {
				;
			}
			volume_id_close(vid);
		}
		close (fd);
	}
	
out:
	if (cs != NULL) {
		libhal_device_commit_changeset (ctx, cs, &error);
		libhal_device_free_changeset (cs);
	}

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
