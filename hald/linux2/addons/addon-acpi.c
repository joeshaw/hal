/***************************************************************************
 * CVSID: $Id$
 *
 * addon-acpi.c : Listen to ACPI events and modify hal device objects
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include "libhal/libhal.h"

#include "../probing/shared.h"

static char *
read_line (int fd)
{
	unsigned int i;
	unsigned int r;
	static char buf[256];
	char *res;
	dbus_bool_t searching;

	i = 0;
	res = NULL;
	searching = TRUE;

	while (searching) {
		while (i < sizeof (buf)) {
			r = read(fd, buf + i, 1);
			if (r < 0 && errno != EINTR) {
				/* we should do something with the data */
				dbg ("ERR read(): %s\n", strerror(errno));
				goto out;
			} else if (r == 0) {
				/* signal this in an almost standard way */
				errno = EPIPE;
				return NULL;
			} else if (r == 1) {
				/* scan for a newline */
				if (buf[i] == '\n') {
					searching = FALSE;
					buf[i] = '\0';
					res = buf;
					goto out;
				}
				i++;
			}
		}

		if (i >= sizeof (buf)) {
			dbg ("ERR: buffer size of %d is too small\n", sizeof (buf));
			goto out;
		}

	}

out:	
	return res;
}

int
main (int argc, char *argv[])
{
	int fd;
	struct sockaddr_un addr;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	char acpi_path[256];
	char acpi_name[256];
	unsigned int acpi_num1;
	unsigned int acpi_num2;

	fd = -1;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	/* TODO: get mountpoint of proc from... /proc/mounts.. :-) */
	fd = open ("/proc/acpi/event", O_RDONLY);
	if (fd < 0) {
		dbg ("Cannot open /proc/acpi/event: %s - trying /var/run/acpid.socket", strerror (errno));
		
		/* TODO: make /var/run/acpid.socket a configure option */

		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			return fd;
		}
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		snprintf (addr.sun_path, sizeof (addr.sun_path), "%s", "/var/run/acpid.socket");
		if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
			dbg ("Cannot open /var/run/acpid.socket - bailing out");
			goto out;
		}
	}
		
	/* main loop */
	while (1) {
		char *event;

		/* read and handle an event */
		event = read_line (fd);
		if (event) {
			dbg ("ACPI event %s\n", event);
		} else if (errno == EPIPE) {
			dbg ("connection closed\n");
			break;
		}

		dbg ("event is '%s'", event);

		if (sscanf (event, "%s %s %x %x", acpi_path, acpi_name, &acpi_num1, &acpi_num2) == 4) {
			char udi[256];

			snprintf (udi, sizeof (udi), "/org/freedesktop/Hal/devices/acpi_%s", acpi_name);

			if (strncmp (acpi_path, "button", sizeof ("button") - 1) == 0) {
				dbg ("button event");

				/* TODO: only rescan if button got state */
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);

				dbus_error_init (&error);
				libhal_device_emit_condition (ctx, udi, "ButtonPressed", "", &error);

			} else if (strncmp (acpi_path, "ac_adapter", sizeof ("ac_adapter") - 1) == 0) {
				dbg ("ac_adapter event");
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);
			} else if (strncmp (acpi_path, "battery", sizeof ("battery") - 1) == 0) {
				dbg ("battery event");
				dbus_error_init (&error);
				libhal_device_rescan (ctx, udi, &error);
			}

		} else {
			dbg ("cannot parse event");
		}
		
	}
	
	
out:
	if (fd >= 0)
		close (fd);

	return 0;
}
