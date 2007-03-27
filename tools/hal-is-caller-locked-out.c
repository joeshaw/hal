/***************************************************************************
 * CVSID: $Id$
 *
 * hal-is-caller-locked-out.c : Determine if a caller is locked out
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include <libhal.h>

/** 
 *  usage:
 *  @argc:                Number of arguments given to program
 *  @argv:                Arguments given to program
 *
 *  Print out program usage. 
 */
static void
usage (int argc, char *argv[])
{
	fprintf (stderr,
                 "\n"
                 "usage : hal-is-caller-locked-out --udi <udi> --interface <interface>\n"
                 "                                 --caller <caller-name>\n"
                 "                                 [--help] [--version]\n");
	fprintf (stderr,
                 "\n"
                 "        --udi            Unique Device Id\n"
                 "        --interface      Interface\n"
                 "        --caller         The name of the caller\n"
                 "        --version        Show version and exit\n"
                 "        --help           Show this information and exit\n"
                 "\n"
                 "This program determines if a given process on the system bus is\n"
                 "locked out of a D-Bus interface. If the process is locked out\n"
                 "or an error occurs this program exits with exit code 1. Otherwise\n"
                 "it exits with exit code 0. Note that only the super user (root)\n"
                 "or other privileged users can use this tool.\n"
                 "\n");
}

/** 
 *  main:
 *  @argc:                Number of arguments given to program
 *  @argv:                Arguments given to program
 *
 *  Returns:              Return code
 *
 *  Main entry point 
 */
int
main (int argc, char *argv[])
{
	char *udi = NULL;
	char *interface = NULL;
	char *caller = NULL;
        dbus_bool_t is_version = FALSE;
        dbus_bool_t locked_out;
	DBusError error;
        LibHalContext *hal_ctx;


	if (argc <= 1) {
		usage (argc, argv);
		return 1;
	}

	while (1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
			{"udi", 1, NULL, 0},
			{"interface", 1, NULL, 0},
			{"caller", 1, NULL, 0},
			{"version", 0, NULL, 0},
			{"help", 0, NULL, 0},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long (argc, argv, "",
				 long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			opt = long_options[option_index].name;

			if (strcmp (opt, "help") == 0) {
				usage (argc, argv);
				return 0;
			} else if (strcmp (opt, "version") == 0) {
				is_version = TRUE;
			} else if (strcmp (opt, "udi") == 0) {
				udi = strdup (optarg);
			} else if (strcmp (opt, "caller") == 0) {
				caller = strdup (optarg);
			} else if (strcmp (opt, "interface") == 0) {
				interface = strdup (optarg);
			}
			break;

		default:
			usage (argc, argv);
			return 1;
			break;
		}
	}

	if (is_version) {
		printf ("hal-is-caller-locked-out " PACKAGE_VERSION "\n");
		return 0;
	}

	if (udi == NULL || caller == NULL || interface == NULL) {
		usage (argc, argv);
		return 1;
	}

	dbus_error_init (&error);	
	if ((hal_ctx = libhal_ctx_new ()) == NULL) {
		fprintf (stderr, "error: libhal_ctx_new\n");
		return 1;
	}
	if (!libhal_ctx_set_dbus_connection (hal_ctx, dbus_bus_get (DBUS_BUS_SYSTEM, &error))) {
		fprintf (stderr, "error: libhal_ctx_set_dbus_connection: %s: %s\n", error.name, error.message);
		LIBHAL_FREE_DBUS_ERROR (&error);
		return 1;
	}
	if (!libhal_ctx_init (hal_ctx, &error)) {
		if (dbus_error_is_set(&error)) {
			fprintf (stderr, "error: libhal_ctx_init: %s: %s\n", error.name, error.message);
			dbus_error_free (&error);
		}
		fprintf (stderr, "Could not initialise connection to hald.\n"
				 "Normally this means the HAL daemon (hald) is not running or not ready.\n");
		return 1;
	}

        locked_out = libhal_device_is_caller_locked_out (hal_ctx,
                                                         udi,
                                                         interface,
                                                         caller,
                                                         &error);
        if (dbus_error_is_set (&error)) {
		fprintf (stderr, "error: %s: %s\n", error.name, error.message);
		dbus_error_free (&error);
		return 1;
        }

        if (locked_out)
                return 1;
        else
                return 0;
}
