/***************************************************************************
 * CVSID: $Id$
 *
 * addon-storage.c : Poll storage devices for media changes
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>

#include "libhal/libhal.h"

#include "../probing/shared.h"

static void 
force_unmount (const char *device_file)
{
	pid_t pid;

	switch (pid = fork ()) {
	case -1:
		break;
	case 0:
		execl ("/bin/umount", "-l", device_file);
		break;
	default:
		waitpid (pid, NULL, 0);
		break;
	}
}

static void 
unmount_childs(LibHalContext *ctx, const char *udi)
{
	int num_volumes;
	char **volumes;
	DBusError error;

	/* need to force unmount all partitions */
	dbus_error_init (&error);
	if ((volumes = libhal_manager_find_device_string_match (
		     ctx, "block.storage_device", udi, &num_volumes, &error)) != NULL) {
		int i;
		
		for (i = 0; i < num_volumes; i++) {
			char *vol_udi;
			
			vol_udi = volumes[i];
			dbus_error_init (&error);
			if (libhal_device_get_property_bool (ctx, vol_udi, "block.is_volume", &error)) {
				dbus_error_init (&error);
				if (libhal_device_get_property_bool (ctx, vol_udi, "volume.is_mounted", &error)) {
					char *vol_device_file;

					dbus_error_init (&error);
					vol_device_file = libhal_device_get_property_string (ctx, vol_udi, 
											     "block.device", &error);
					if (vol_device_file != NULL) {
						dbg ("Forcing unmount for %s", vol_device_file);

						/* TODO: emit DeviceCondition */
						force_unmount (vol_device_file);
						libhal_free_string (vol_device_file);
					}
				}
			}
		}
		libhal_free_string_array (volumes);
	} 
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


enum {
	MEDIA_STATUS_UNKNOWN = 0,
	MEDIA_STATUS_GOT_MEDIA = 1,
	MEDIA_STATUS_NO_MEDIA = 2
};

int 
main (int argc, char *argv[])
{
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	char *bus;
	char *drive_type;
	int is_cdrom;
	int media_status;
	int storage_policy_should_mount;
	char *storage_policy_should_mount_str;
	char *support_media_changed_str;
	int support_media_changed;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
	if ((bus = getenv ("HAL_PROP_STORAGE_BUS")) == NULL)
		goto out;
	if ((drive_type = getenv ("HAL_PROP_STORAGE_DRIVE_TYPE")) == NULL)
		goto out;
	storage_policy_should_mount_str = getenv ("HAL_PROP_STORAGE_POLICY_SHOULD_MOUNT");

	support_media_changed_str = getenv ("HAL_PROP_STORAGE_CDROM_SUPPORT_MEDIA_CHANGED");
	if (support_media_changed_str != NULL && strcmp (support_media_changed_str, "true") == 0)
		support_media_changed = TRUE;
	else
		support_media_changed = FALSE;

	dbus_error_init (&error);
	if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
		goto out;

	if ((ctx = libhal_ctx_new ()) == NULL)
		goto out;
	if (!libhal_ctx_set_dbus_connection (ctx, conn))
		goto out;
	if (!libhal_ctx_init (ctx, &error))
		goto out;

	printf ("**************************************************\n");
	printf ("**************************************************\n");
	printf ("Doing addon-storage for %s (bus %s) (drive_type %s) (udi %s)\n", device_file, bus, drive_type, udi);
	printf ("**************************************************\n");
	printf ("**************************************************\n");

	if (strcmp (drive_type, "cdrom") == 0)
		is_cdrom = 1;
	else
		is_cdrom = 0;

	if (storage_policy_should_mount_str != NULL && strcmp (storage_policy_should_mount_str, "true") == 0)
		storage_policy_should_mount = 1;
	else
		storage_policy_should_mount = 0;

	media_status = MEDIA_STATUS_UNKNOWN;

	while (TRUE) {
		int fd;
		int got_media;

		got_media = FALSE;

		if (is_cdrom) {
			int drive;

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
					goto skip_check;

				fd = open (device_file, O_RDONLY | O_NONBLOCK);
			}

			if (fd < 0) {
				dbg ("open failed for %s: %s", device_file, strerror (errno)); 
				goto skip_check;
			}


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
				dbg ("CDROM_DRIVE_STATUS failed: %s\n", strerror(errno));
				break;
				
			default:
				break;
			}
			close (fd);
		} else {

			fd = open (device_file, O_RDONLY);
			if (fd < 0 && errno == ENOMEDIUM) {
				got_media = FALSE;
				close (fd);
			} else if (fd >= 0) {
				got_media = TRUE;
				close (fd);
			} else {
				dbg ("open failed for %s: %s", device_file, strerror (errno)); 
				close (fd);
				goto skip_check;
			}
		}

		switch (media_status)
		{
		case MEDIA_STATUS_GOT_MEDIA:
			if (!got_media) {
				DBusError error;
				
				dbg ("Media removal detected on %s", device_file);
				
				/* have to unmount all childs, but only if we're doing policy on the device */
				if (storage_policy_should_mount)
					unmount_childs (ctx, udi);
				
				/* could have a fs on the main block device; do a rescan to remove it */
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);
				
				/* have to this to trigger appropriate hotplug events */
				fd = open (device_file, O_RDONLY | O_NONBLOCK);
				if (fd >= 0)
					ioctl (fd, BLKRRPART);
				close (fd);
			}
			break;

		case MEDIA_STATUS_NO_MEDIA:
			if (got_media) {
				DBusError error;
				
				dbg ("Media insertion detected on %s", device_file);
				/* our probe will trigger the appropriate hotplug events */
				
				/* could have a fs on the main block device; do a rescan to add it */
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);
				
			}
			break;
			
		default:
		case MEDIA_STATUS_UNKNOWN:
			break;
		}
		
		/* update our current status */
		if (got_media)
			media_status = MEDIA_STATUS_GOT_MEDIA;
		else
			media_status = MEDIA_STATUS_NO_MEDIA;
		
		
		dbg ("polling %s; got media=%d", device_file, got_media);
		
	skip_check:

		sleep (2);

	}

out:
	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return 0;
}
