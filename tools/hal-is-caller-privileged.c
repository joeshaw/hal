/***************************************************************************
 * CVSID: $Id$
 *
 * hal-is-caller-privileged.c : Determine if a caller is privileged
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
#include <glib.h>
#include <stdlib.h>

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
                 "usage : hal-is-caller-privileged --udi <udi> --action <action>\n"
                 "                                 --caller <caller-name>\n"
                 "                                 [--help] [--version]\n");
	fprintf (stderr,
                 "\n"
                 "        --udi            Unique Device Id\n"
                 "        --action         PolicyKit action to check for\n"
                 "        --caller         The name of the caller\n"
                 "        --version        Show version and exit\n"
                 "        --help           Show this information and exit\n"
                 "\n"
                 "This program determines if a given process on the system bus is\n"
                 "privileged for a given PolicyKit action for a given device. If an error\n"
                 "occurs this program exits with a non-zero exit code. Otherwise\n"
                 "the textual reply will be printed on stdout and this program will\n"
                 "exit with exit code 0. Note that only the super user (root)\n"
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
	char *action = NULL;
	char *caller = NULL;
        dbus_bool_t is_version = FALSE;
        char *polkit_result;
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
			{"action", 1, NULL, 0},
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
			} else if (strcmp (opt, "action") == 0) {
				action = strdup (optarg);
			}
			break;

		default:
			usage (argc, argv);
			return 1;
			break;
		}
	}

	if (is_version) {
		printf ("hal-is-caller-privileged " PACKAGE_VERSION "\n");
		return 0;
	}

	if (udi == NULL || caller == NULL || action == NULL) {
		usage (argc, argv);
		return 1;
	}

	dbus_error_init (&error);	
        if (getenv ("HALD_DIRECT_ADDR") != NULL) {
                if ((hal_ctx = libhal_ctx_init_direct (&error)) == NULL) {
                        fprintf (stderr, "Cannot connect to hald\n");
                        if (dbus_error_is_set (&error)) {
                                dbus_error_free (&error);
                        }
                        return 1;
                }
        } else {
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
        }

        polkit_result = libhal_device_is_caller_privileged (hal_ctx,
                                                            udi,
                                                            action,
                                                            caller,
                                                            &error);
        if (dbus_error_is_set (&error)) {
		fprintf (stderr, "error: %s: %s\n", error.name, error.message);
		dbus_error_free (&error);
		return 1;
        }

        printf ("%s\n", polkit_result);
        return 0;
}
