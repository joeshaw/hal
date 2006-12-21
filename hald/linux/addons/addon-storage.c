/***************************************************************************
 * CVSID: $Id$
 *
 * addon-storage.c : Poll storage devices for media changes
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
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>
#include <scsi/sg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "libhal/libhal.h"

#include "../../logger.h"
#include "../../util_helper.h"

static void 
force_unmount (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	DBusMessage *msg = NULL;
	DBusMessage *reply = NULL;
	char **options = NULL;
	unsigned int num_options = 0;
	DBusConnection *dbus_connection;
	char *device_file;

	dbus_connection = libhal_ctx_get_dbus_connection (ctx);

	msg = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
					    "org.freedesktop.Hal.Device.Volume",
					    "Unmount");
	if (msg == NULL) {
		HAL_ERROR (("Could not create dbus message for %s", udi));
		goto out;
	}


	options = calloc (1, sizeof (char *));
	if (options == NULL) {
		HAL_ERROR (("Could not allocate options array"));
		goto out;
	}

	options[0] = "lazy";
	num_options = 1;

	device_file = libhal_device_get_property_string (ctx, udi, "block.device", NULL);
	if (device_file != NULL) {
		HAL_INFO(("forcibly attempting to lazy unmount %s as media was removed", device_file));
		libhal_free_string (device_file);
	}

	if (!dbus_message_append_args (msg, 
				       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &options, num_options,
				       DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Could not append args to dbus message for %s", udi));
		goto out;
	}
	
	dbus_error_init (&error);
	if (!(reply = dbus_connection_send_with_reply_and_block (dbus_connection, msg, -1, &error))) {
		HAL_ERROR (("Unmount failed for %s: %s : %s\n", udi, error.name, error.message));
		dbus_error_free (&error);
		goto out;
	}

	if (dbus_error_is_set (&error)) {
		HAL_ERROR (("Unmount failed for %s\n%s : %s\n", udi, error.name, error.message));
		dbus_error_free (&error);
		goto out;
	}

	HAL_DEBUG (("Succesfully unmounted udi '%s'", udi));

out:
	if (options != NULL)
		free (options);
	if (msg != NULL)
		dbus_message_unref (msg);
	if (reply != NULL)
		dbus_message_unref (reply);
}

static dbus_bool_t
unmount_cleartext_devices (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	char **clear_devices;
	int num_clear_devices;
	dbus_bool_t ret;

	ret = FALSE;

	/* check if the volume we back is mounted.. if it is.. unmount it */
	dbus_error_init (&error);
	clear_devices = libhal_manager_find_device_string_match (ctx,
								 "volume.crypto_luks.clear.backing_volume",
								 udi,
								 &num_clear_devices,
								 &error);

	if (clear_devices != NULL && num_clear_devices > 0) {
		int i;

		ret = TRUE;

		for (i = 0; i < num_clear_devices; i++) {
			char *clear_udi;
			clear_udi = clear_devices[i];
			dbus_error_init (&error);
			if (libhal_device_get_property_bool (ctx, clear_udi, "volume.is_mounted", &error)) {
				HAL_DEBUG (("Forcing unmount of child '%s' (crypto)", clear_udi));
				force_unmount (ctx, clear_udi);
			}
		}
		libhal_free_string_array (clear_devices);
	}

	return ret;
}

static void 
unmount_childs (LibHalContext *ctx, const char *udi)
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
				dbus_bool_t is_crypto;

				/* unmount all cleartext devices associated with us */
				is_crypto = unmount_cleartext_devices (ctx, vol_udi);

				dbus_error_init (&error);
				if (libhal_device_get_property_bool (ctx, vol_udi, "volume.is_mounted", &error)) {
					HAL_DEBUG (("Forcing unmount of child '%s'", vol_udi));
					force_unmount (ctx, vol_udi);
				}

				/* teardown crypto */
				if (is_crypto) {
					DBusMessage *msg = NULL;
					DBusMessage *reply = NULL;

					/* tear down mapping */
					HAL_DEBUG (("Teardown crypto for '%s'", vol_udi));

					msg = dbus_message_new_method_call ("org.freedesktop.Hal", vol_udi,
									    "org.freedesktop.Hal.Device.Volume.Crypto",
									    "Teardown");
					if (msg == NULL) {
						HAL_ERROR (("Could not create dbus message for %s", vol_udi));
						goto teardown_failed;
					}

					dbus_error_init (&error);
					if (!(reply = dbus_connection_send_with_reply_and_block (
						      libhal_ctx_get_dbus_connection (ctx), msg, -1, &error)) || 
					    dbus_error_is_set (&error)) {
						HAL_DEBUG (("Teardown failed for %s: %s : %s\n", 
						     udi, error.name, error.message));
						dbus_error_free (&error);
					}

				teardown_failed:
					if (msg != NULL)
						dbus_message_unref (msg);
					if (reply != NULL)
						dbus_message_unref (reply);
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
	char *bus;
	char *drive_type;
	int is_cdrom;
	int media_status;
	char *support_media_changed_str;
	int support_media_changed;

	hal_set_proc_title_init (argc, argv);

	/* We could drop privs if we know that the haldaemon user is
	 * to be able to access block devices...
	 */
        /*drop_privileges (1);*/

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
	if ((bus = getenv ("HAL_PROP_STORAGE_BUS")) == NULL)
		goto out;
	if ((drive_type = getenv ("HAL_PROP_STORAGE_DRIVE_TYPE")) == NULL)
		goto out;

	setup_logger ();

	support_media_changed_str = getenv ("HAL_PROP_STORAGE_CDROM_SUPPORT_MEDIA_CHANGED");
	if (support_media_changed_str != NULL && strcmp (support_media_changed_str, "true") == 0)
		support_media_changed = TRUE;
	else
		support_media_changed = FALSE;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	dbus_error_init (&error);
	if (!libhal_device_addon_is_ready (ctx, udi, &error)) {
		goto out;
	}

	HAL_DEBUG (("**************************************************"));
	HAL_DEBUG (("Doing addon-storage for %s (bus %s) (drive_type %s) (udi %s)", device_file, bus, drive_type, udi));
	HAL_DEBUG (("**************************************************"));

	hal_set_proc_title ("hald-addon-storage: polling %s", device_file);

	if (strcmp (drive_type, "cdrom") == 0)
		is_cdrom = 1;
	else
		is_cdrom = 0;

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
				HAL_ERROR (("open failed for %s: %s", device_file, strerror (errno))); 
				goto skip_check;
			}


			/* Check if a disc is in the drive
			 *
			 * @todo Use MMC-2 API if applicable
			 */
			drive = ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
			switch (drive) {
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
				HAL_ERROR (("CDROM_DRIVE_STATUS failed: %s\n", strerror(errno)));
				break;

			default:
				break;
			}

			/* check if eject button was pressed */
			if (got_media) {
				unsigned char cdb[10] = { 0x4a, 1, 0, 0, 16, 0, 0, 0, 8, 0};
				unsigned char buffer[8];
				struct sg_io_hdr sg_h;
				int retval;

				memset(buffer, 0, sizeof(buffer));
				memset(&sg_h, 0, sizeof(struct sg_io_hdr));
				sg_h.interface_id = 'S';
				sg_h.cmd_len = sizeof(cdb);
				sg_h.dxfer_direction = SG_DXFER_FROM_DEV;
				sg_h.dxfer_len = sizeof(buffer);
				sg_h.dxferp = buffer;
				sg_h.cmdp = cdb;
				sg_h.timeout = 5000;
				retval = ioctl(fd, SG_IO, &sg_h);
				if (retval == 0 && sg_h.status == 0 && (buffer[4] & 0x0f) == 0x01) {
					DBusError error;

					/* emit signal from drive device object */
					dbus_error_init (&error);
					libhal_device_emit_condition (ctx, udi, "EjectPressed", "", &error);
				}
			}
			close (fd);
		} else {
			fd = open (device_file, O_RDONLY);
			if (fd < 0 && errno == ENOMEDIUM) {
				got_media = FALSE;
			} else if (fd >= 0) {
				got_media = TRUE;
				close (fd);
			} else {
				HAL_ERROR (("open failed for %s: %s", device_file, strerror (errno))); 
				goto skip_check;
			}
		}

		switch (media_status) {
		case MEDIA_STATUS_GOT_MEDIA:
			if (!got_media) {
				DBusError error;
				
				HAL_DEBUG (("Media removal detected on %s", device_file));
				libhal_device_set_property_bool (ctx, udi, "storage.removable.media_available", FALSE, NULL);
				libhal_device_set_property_string (ctx, udi, "storage.partitioning_scheme", "", NULL);

				
				/* attempt to unmount all childs */
				unmount_childs (ctx, udi);
				
				/* could have a fs on the main block device; do a rescan to remove it */
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);
				
				/* have to this to trigger appropriate hotplug events */
				fd = open (device_file, O_RDONLY | O_NONBLOCK);
				if (fd >= 0) {
					ioctl (fd, BLKRRPART);
					close (fd);
				}
			}
			break;

		case MEDIA_STATUS_NO_MEDIA:
			if (got_media) {
				DBusError error;

				HAL_DEBUG (("Media insertion detected on %s", device_file));

				/* our probe will trigger the appropriate hotplug events */
				libhal_device_set_property_bool (
					ctx, udi, "storage.removable.media_available", TRUE, NULL);

				/* could have a fs on the main block device; do a rescan to add it */
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);
				
			}
			break;

		case MEDIA_STATUS_UNKNOWN:
		default:
			break;
		}

		/* update our current status */
		if (got_media)
			media_status = MEDIA_STATUS_GOT_MEDIA;
		else
			media_status = MEDIA_STATUS_NO_MEDIA;

		/*HAL_DEBUG (("polling %s; got media=%d", device_file, got_media));*/

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
