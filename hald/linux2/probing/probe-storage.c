/***************************************************************************
 * CVSID: $Id$
 *
 * probe-storage.c : Probe storage devices
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
#include <asm/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>

#include "libhal/libhal.h"

#include "drive_id/drive_id.h"
#include "volume_id/volume_id.h"

#include "linux_dvd_rw_utils.h"

#include "shared.h"

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	char *bus;
	char *drive_type;

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
	printf ("Doing probe-storage for %s (bus %s) (drive_type %s)\n", device_file, bus, drive_type);
	printf ("**************************************************\n");
	printf ("**************************************************\n");

	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		printf ("Cannot open %s: %s\n", device_file, strerror (errno));
		goto out;
	}

	/* Only do drive_id on IDE and real SCSI disks - specifically
	 * not on USB which uses emulated SCSI since an INQUIRY on
	 * most USB devices may crash the storage device if the
	 * transfer length isn't exactly 36 bytes. See Red Hat bug
	 * #145256 for details.
	 */
	if (strcmp (bus, "ide") == 0 ||
	    strcmp (bus, "scsi") == 0) {
		struct drive_id *did;

		did = drive_id_open_fd (fd);
		if (drive_id_probe_all (did) == 0) {
			if (did->serial[0] != '\0')
				if (!libhal_device_set_property_string (ctx, udi, "storage.serial", 
									did->serial, &error))
					goto out;

			if (did->firmware[0] != '\0')
				if (!libhal_device_set_property_string (ctx, udi, "storage.firmware_version", 
									did->firmware, &error))
					goto out;
		}
		drive_id_close (did);		
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

		if( ioctl (fd, CDROM_SET_OPTIONS, CDO_USE_FFLAGS) < 0 ) {
			HAL_ERROR (("CDROM_SET_OPTIONS failed: %s\n", strerror(errno)));
			goto out;
		}
		
		capabilities = ioctl (fd, CDROM_GET_CAPABILITY, 0);
		if (capabilities < 0)
			goto out;
		
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.cdr", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.cdrw", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvd", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdr", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdrw", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdram", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusr", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdplusrw", FALSE, &error);

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
			}
		}
		if (capabilities & CDC_DVD_R) {
			libhal_device_set_property_bool (ctx, udi, "storage.cdrom.dvdr", TRUE, &error);
		}
		if (capabilities & CDC_DVD_RAM) {
			libhal_device_set_property_bool (ctx, udi, "storage.dvdram", TRUE, &error);
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
	}

	/* TODO: see if we got a file system on the main block device */



	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
