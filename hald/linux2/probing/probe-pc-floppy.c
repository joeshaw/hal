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
#include <linux/fd.h>

#include "libhal/libhal.h"

#include "shared.h"

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	char name[256];
	struct floppy_drive_struct ds;

	fd = -1;

	/* assume failure */
	ret = 1;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;
	
	
	dbg ("Checking if %s is actually present", device_file);
	
	/* Check that there actually is a drive at the other end */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		dbg ("Could not open %s", device_file);
		goto out;
	}
	
	/* @todo Could use the name here */
	ioctl (fd, FDRESET, NULL);
	if (ioctl (fd, FDGETDRVTYP, name) != 0) {
		dbg ("FDGETDRVTYP failed for %s", device_file);
		goto out;
	}
	dbg ("floppy drive name is '%s'", name);

	if (ioctl (fd, FDPOLLDRVSTAT, &ds)) {
		dbg ("FDPOLLDRVSTAT failed for %s", device_file);
		goto out;
	}
	
	if (ds.track < 0) {
		dbg ("floppy drive %s seems not to exist", device_file);
		goto out;
	}

	/* works */
	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	return ret;
}
