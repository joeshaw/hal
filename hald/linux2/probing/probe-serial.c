/***************************************************************************
 * CVSID: $Id$
 *
 * probe-serial.c : Probe serial ports
 *
 * Copyright (C) 2005 Pierre Ossman <drzeus@drzeus.cx>
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
#include <linux/serial.h>

#include "libhal/libhal.h"

#include "shared.h"

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	struct serial_struct ss;

	fd = -1;

	/* assume failure */
	ret = 1;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_SERIAL_DEVICE")) == NULL)
		goto out;

	_set_debug ();
	
	dbg ("Checking if %s is actually present", device_file);
	
	/* Check that there actually is a drive at the other end */
	fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		dbg ("Could not open %s", device_file);
		goto out;
	}
	
	if (ioctl (fd, TIOCGSERIAL, &ss)) {
		dbg ("TIOCGSERIAL failed for %s", device_file);
		goto out;
	}
	
	if (ss.type == 0) {
		dbg ("serial port %s seems not to exist", device_file);
		goto out;
	}

	/* works */
	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	return ret;
}
