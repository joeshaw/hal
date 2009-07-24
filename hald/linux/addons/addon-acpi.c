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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "libhal/libhal.h"

#include "../../logger.h"
#include "../../util_helper.h"

#ifdef ACPI_PROC
static FILE *
acpi_get_event_fp_kernel (void)
{
	FILE *fp = NULL;
	struct stat sbuf;

	if (stat("/usr/sbin/acpid", &sbuf) == 0)  {
		HAL_DEBUG (("acpid installed, not using the kernel acpi event interface"));
		return NULL;
	}

	fp = fopen ("/proc/acpi/event", "r");
	if (fp == NULL)
		HAL_ERROR (("Cannot open /proc/acpi/event: %s", strerror (errno)));

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
		HAL_ERROR (("Cannot create socket: %s", strerror (errno)));
		return NULL;
	}

	memset (&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy (addr.sun_path, "/var/run/acpid.socket", sizeof addr.sun_path);

	if (connect (fd, (struct sockaddr *) &addr, sizeof addr) < 0) {
		HAL_ERROR (("Cannot connect to acpid socket: %s", strerror (errno)));
		close (fd);
	} else {
		fp = fdopen (fd, "r");

		if (fp == NULL) {
			HAL_ERROR (("fdopen failed: %s", strerror (errno)));
			close (fd);
		}
	}

	return fp;
}
#endif

#ifdef BUILD_ACPI_IBM
static void
handle_ibm_acpi_events (LibHalContext *ctx, int type, int event) 
{
	DBusError error;
	char udi[256];
	char *button;
	
	dbus_error_init (&error);
	button = NULL;
	snprintf (udi, sizeof (udi), "/org/freedesktop/Hal/devices/computer");
	
	if (type == 128) {
		switch (event) {
			case 4097: /* Fn+F1 */
				button = "Fn+F1";
				break;
			case 4098:
				button = "Fn+F2";
				break;
			case 4099: /* dpms off */
				button = "display_off";
				break;
			case 4100: /* sleep button */
				button = "sleep";
				break;
			case 4101: /* wireless */
				button = "wifi-power";
				break;
			case 4102:
				button = "Fn+F6";
				break;
			case 4103: /* switch display */
				button = "display_switch";
				break;
			case 4104:
				button = "Fn+F8";
				break;
			case 4105: /* undock */
				button = "undock";
				break;
			case 4106:
				button = "Fn+F10";
				break;
			case 4107:
				button = "Fn+F11";
				break;
			case 4108: /* Fn+F12 , hibernate/s2disk */ 
				button = "hibernate";
				break;
			case 4109: /* Fn+Backspace*/
				button = "Fn+Backspace";
				break;
			case 4110: /* Fn+Insert*/ 
				button = "Fn+Insert";
				break;
			case 4111: /* Fn+Delete*/ 
				button = "Fn+Delete";
				break;
			case 4116: /* Fn+Space */
				button = "zoom";
				break;
			case 4120: /* ThinkPad */
				button = "ThinkPad";
				break;
			case 20489: /* Tablet rotated */
				button = "tabletpc_rotate_180";
				break;
			case 20490: /* Tablet rotated back*/
				button = "tabletpc_rotate_normal";
				break;
			case 28672: /* killswitch */
				button = "killswitch";
				break;
			default:
				break;
			
		}
		
		if (button) {
			libhal_device_emit_condition (ctx, udi, "ButtonPressed",
						      button, &error);
			if (dbus_error_is_set (&error)) 
				dbus_error_free (&error);
		}
	}
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
		HAL_DEBUG (("event is '%s'", event));

		if (sscanf (event, "%s %s %x %x", acpi_path, acpi_name, &acpi_num1, &acpi_num2) == 4) {
			char udi[256];

			snprintf (udi, sizeof (udi), "/org/freedesktop/Hal/devices/acpi_%s", acpi_name);

#ifdef BUILD_ACPI_IBM
			if (strncmp (acpi_path, "ibm/hotkey", sizeof ("ibm/hotkey") -1) == 0) {
				/* handle ibm ACPI hotkey events*/
				handle_ibm_acpi_events(ctx, acpi_num1, acpi_num2);
			} else 
#endif
			
			if (libhal_device_exists(ctx, udi, &error)) {

				if (strncmp (acpi_path, "button", sizeof ("button") - 1) == 0) {
					char *type;

					HAL_DEBUG (("button event"));

					/* TODO: only rescan if button got state */
					if (libhal_device_rescan (ctx, udi, &error)) {
						type = libhal_device_get_property_string(ctx, udi, 
											 "button.type",
											 &error);
						LIBHAL_FREE_DBUS_ERROR(&error);

						if (type != NULL) {
							libhal_device_emit_condition (ctx, udi, "ButtonPressed",
										      type, &error);
							libhal_free_string(type);
							LIBHAL_FREE_DBUS_ERROR(&error);
						} else {
							libhal_device_emit_condition (ctx, udi, "ButtonPressed", "", &error);
						}
					}
				} else if (strncmp (acpi_path, "ac_adapter", sizeof ("ac_adapter") - 1) == 0) {
					HAL_DEBUG (("ac_adapter event"));
					libhal_device_rescan (ctx, udi, &error);
				} else if (strncmp (acpi_path, "battery", sizeof ("battery") - 1) == 0) {
					HAL_DEBUG (("battery event"));
					libhal_device_rescan (ctx, udi, &error);
				}
			} 

		} else {
			HAL_DEBUG (("cannot parse event"));
		}

		LIBHAL_FREE_DBUS_ERROR(&error);
	}

	fclose (eventfp);
}

int
main (int argc, char **argv)
{
	LibHalContext *ctx = NULL;
	DBusError error;
	FILE *eventfp;

	hal_set_proc_title_init (argc, argv);

	/* If we don't even consider the /proc ACPI interface, drop privileges
	 * right away */
#ifndef ACPI_PROC
	drop_privileges (0);
#endif

	setup_logger ();

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		HAL_ERROR (("Unable to initialise libhal context: %s", error.message));
		goto out;
	}

	if (!libhal_device_addon_is_ready (ctx, getenv ("UDI"), &error)) {
		goto out;
	}

#ifdef ACPI_PROC
	/* If we can connect directly to the kernel then do so. */
	eventfp = acpi_get_event_fp_kernel ();
	drop_privileges (0);

	if (eventfp) {
		hal_set_proc_title ("hald-addon-acpi: listening on acpi kernel interface /proc/acpi/event");
		main_loop (ctx, eventfp);
		HAL_ERROR (("Lost connection to kernel acpi event source - exiting"));
		goto out;
	}
#endif

	while (1)
	{
#ifdef ACPI_ACPID
		/* Else, try to use acpid. */
		if ((eventfp = acpi_get_event_fp_acpid ())) {
			hal_set_proc_title ("hald-addon-acpi: listening on acpid socket /var/run/acpid.socket");
			main_loop (ctx, eventfp);
			HAL_DEBUG (("Cannot connect to acpid event socket - retry connect"));
		}
#endif
		
		/* If main_loop exits or we failed a reconnect attempt then
		 * sleep for 5s and try to reconnect (again). */
		sleep (5);
	}

out:

        HAL_DEBUG (("An error occured, exiting cleanly"));

        LIBHAL_FREE_DBUS_ERROR (&error);

        if (ctx != NULL) {
                libhal_ctx_shutdown (ctx, &error);
                LIBHAL_FREE_DBUS_ERROR (&error);
                libhal_ctx_free (ctx);
        }

	return 1;
}

/* vim:set sw=8 noet: */
