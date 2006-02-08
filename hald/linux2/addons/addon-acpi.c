/***************************************************************************
 * CVSID: $Id$
 *
 * addon-acpi.c : Listen to ACPI events and modify hal device objects
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Ryan Lortie <desrt@desrt.ca>
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

#ifdef ACPI_PROC
static FILE *
acpi_get_event_fp_kernel (void)
{
	FILE *fp = NULL;

	fp = fopen ("/proc/acpi/event", "r");

	if (fp == NULL)
		dbg ("Cannot open /proc/acpi/event: %s", strerror (errno));

	return fp;
}
#endif

#ifdef ACPI_ACPID
static FILE *
acpi_get_event_fp_acpid (void)
{
	FILE *fp = NULL;

	struct sockaddr_un addr;
	int fd;

	if( (fd = socket (AF_UNIX, SOCK_STREAM, 0)) < 0 ) {
		dbg ("Cannot create socket: %s", strerror (errno));
		return NULL;
	}

	memset (&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy (addr.sun_path, "/var/run/acpid.socket", sizeof addr.sun_path);

	if (connect (fd, (struct sockaddr *) &addr, sizeof addr) < 0) {
		dbg ("Cannot connect to acpid socket: %s", strerror (errno));
		close (fd);
	} else {
		fp = fdopen (fd, "r");

		if (fp == NULL)
		{
			dbg ("fdopen failed: %s", strerror (errno));
			close (fd);
		}
	}

	return fp;
}
#endif


static void
main_loop (LibHalContext *ctx, FILE *eventfp)
{
	unsigned int acpi_num1;
	unsigned int acpi_num2;
	char acpi_path[256];
	char acpi_name[256];
	DBusError error;
	char event[256];

	dbus_error_init (&error);

	while (fgets (event, sizeof event, eventfp))
	{
		dbg ("event is '%s'", event);

		if (sscanf (event, "%s %s %x %x", acpi_path, acpi_name, &acpi_num1, &acpi_num2) == 4) {
			char udi[256];

			snprintf (udi, sizeof (udi), "/org/freedesktop/Hal/devices/acpi_%s", acpi_name);

			if (strncmp (acpi_path, "button", sizeof ("button") - 1) == 0) {
				char *type;

				dbg ("button event");

				/* TODO: only rescan if button got state */
				libhal_device_rescan (ctx, udi, &error);

				type = libhal_device_get_property_string(ctx, udi, 
									 "button.type",
									 &error);
				if (type != NULL) {
					libhal_device_emit_condition (ctx, udi, "ButtonPressed",
								      type, &error);
					libhal_free_string(type);
				} else {
					libhal_device_emit_condition (ctx, udi, "ButtonPressed", "", &error);
				}
			} else if (strncmp (acpi_path, "ac_adapter", sizeof ("ac_adapter") - 1) == 0) {
				dbg ("ac_adapter event");
				libhal_device_rescan (ctx, udi, &error);
			} else if (strncmp (acpi_path, "battery", sizeof ("battery") - 1) == 0) {
				dbg ("battery event");
				libhal_device_rescan (ctx, udi, &error);
			}

		} else {
			dbg ("cannot parse event");
		}
		
	}

	dbus_error_free (&error);
	fclose (eventfp);
}

int
main (int argc, char **argv)
{
	LibHalContext *ctx = NULL;
	DBusError error;
	FILE *eventfp;

	if (getenv ("HALD_VERBOSE") != NULL)
		is_verbose = TRUE;

	dbus_error_init (&error);

	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		dbg ("Unable to initialise libhal context: %s", error.message);
		return 1;
	}

	while (1)
	{
#ifdef ACPI_PROC
		/* If we can connect directly to the kernel then do so. */
		if ((eventfp = acpi_get_event_fp_kernel ())) {
			main_loop (ctx, eventfp);
			dbg ("Lost connection to kernel acpi event source - retry connect");
		}
#endif
#ifdef ACPI_ACPID
		/* Else, try to use acpid. */
		if ((eventfp = acpi_get_event_fp_acpid ())) {
			main_loop (ctx, eventfp);
			dbg ("Cannot connect to acpid event socket - retry connect");
		}
#endif
		
		/* If main_loop exits or we failed a reconnect attempt then
		 * sleep for 5s and try to reconnect (again). */
		sleep (5);
	}

	return 1;
}

/* vim:set sw=8 noet: */
