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
#include <asm/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>

#include "libhal/libhal.h"

#include "../probing/shared.h"

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
	printf ("Doing addon-storage for %s (bus %s) (drive_type %s)\n", device_file, bus, drive_type);
	printf ("**************************************************\n");
	printf ("**************************************************\n");

	if (strcmp (drive_type, "cdrom") == 0)
		is_cdrom = 1;
	else
		is_cdrom = 0;


	media_status = MEDIA_STATUS_UNKNOWN;

	while (1) {
		int fd;

		if (is_cdrom) {
			
			/* TODO */
		} else {
			int got_media;

			fd = open (device_file, O_RDONLY);
			if (fd < 0 && errno == ENOMEDIUM) {
				got_media = FALSE;
			} else {
				got_media = TRUE;
			}
			close (fd);

			switch (media_status)
			{
			case MEDIA_STATUS_GOT_MEDIA:
				if (!got_media) {
					/* signal that parent should force unmount all partitions */
					HAL_INFO (("Media removal detected on %s", device_file));
				}
				break;

			case MEDIA_STATUS_NO_MEDIA:
				if (got_media) {
					HAL_INFO (("Media insertion detected on %s", device_file));
					/* will trigger appropriate hotplug events */
					fd = open (device_file, O_RDONLY | O_NONBLOCK);
					if (fd >= 0)
						ioctl (fd, BLKRRPART);
					close (fd);
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


			HAL_INFO (("polling %s; got media=%d", device_file, got_media));

		}

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
