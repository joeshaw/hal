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

#include <fcntl.h>
#include <linux/fd.h>
#include <linux/kdev_t.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "libhal/libhal.h"

#include "../../logger.h"

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *device_file;
	char name[256];
	struct floppy_drive_struct ds;

	fd = -1;

	/* assume failure */
	ret = 1;

	if (getenv ("UDI") == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;

	setup_logger ();
	
	HAL_DEBUG (("Checking if %s is actually present", device_file));
	
	/* Check that there actually is a drive at the other end */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		HAL_ERROR (("Could not open %s", device_file));
		goto out;
	}
	
	/* @todo Could use the name here */
	ioctl (fd, FDRESET, NULL);
	if (ioctl (fd, FDGETDRVTYP, name) != 0) {
		HAL_ERROR (("FDGETDRVTYP failed for %s", device_file));
		goto out;
	}
	HAL_DEBUG (("floppy drive name is '%s'", name));

	if (ioctl (fd, FDPOLLDRVSTAT, &ds)) {
		HAL_ERROR (("FDPOLLDRVSTAT failed for %s", device_file));
		goto out;
	}
	
	if (ds.track < 0) {
		HAL_ERROR (("floppy drive %s seems not to exist", device_file));
		goto out;
	}

	/* works */
	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	return ret;
}
