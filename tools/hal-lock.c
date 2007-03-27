/***************************************************************************
 * CVSID: $Id$
 *
 * hal-lock.c : Lock an interface
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <glib.h>

#include "libhal.h"

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
                 "usage : hal-lock --interface <interface>\n"
                 "                 --run <program-and-args>\n"
                 "                 [--udi <udi>]\n"
                 "                 [--exclusive]\n"
                 "                 [--help] [--version]\n");
	fprintf (stderr,
                 "\n"
                 "        --interface      Interface to lock\n"
                 "        --run            Program to run if the lock was acquired\n"
                 "        --udi            Unique Device Id of device to lock. If\n"
                 "                         ommitted the global lock will be tried\n"
                 "        --exclusive      Whether the lock can be held by others\n"
                 "        --version        Show version and exit\n"
                 "        --help           Show this information and exit\n"
                 "\n"
                 "This program will attempt to grab a lock on a given interface.\n"
                 "Unless, a specific UDI is given, the global lock will be tried.\n"
                 "If the lock was succesfully acquired the program specified by\n"
                 "the option --run will be run and upon termination this program\n"
                 "will exit with exit code 0. If the lock wasn't acquired or an\n"
                 "error occured while taking the lock, this program will exit with a\n"
                 "non-zero exit code and the given program will not be run.\n"
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
        char *run = NULL;
        dbus_bool_t is_version = FALSE;
        dbus_bool_t exclusive = FALSE;
        dbus_bool_t got_lock = FALSE;
	DBusError error;
        LibHalContext *hal_ctx;
        int ret;
        GError *g_error = NULL;

        ret = 1;

	if (argc <= 1) {
		usage (argc, argv);
                goto out;
	}

	while (1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
			{"udi", 1, NULL, 0},
			{"interface", 1, NULL, 0},
			{"run", 1, NULL, 0},
			{"exclusive", 0, NULL, 0},
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
			} else if (strcmp (opt, "run") == 0) {
				run = strdup (optarg);
			} else if (strcmp (opt, "exclusive") == 0) {
                                exclusive = TRUE;
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
		printf ("hal-lock " PACKAGE_VERSION "\n");
                ret = 0;
                goto out;
	}

	if (interface == NULL || run == NULL) {
		usage (argc, argv);
                goto out;
	}


        dbus_error_init (&error);	
        if ((hal_ctx = libhal_ctx_new ()) == NULL) {
                fprintf (stderr, "error: libhal_ctx_new\n");
                goto out;
        }
        if (!libhal_ctx_set_dbus_connection (hal_ctx, dbus_bus_get (DBUS_BUS_SYSTEM, &error))) {
                fprintf (stderr, "error: libhal_ctx_set_dbus_connection: %s: %s\n", error.name, error.message);
                LIBHAL_FREE_DBUS_ERROR (&error);
                goto out;
        }
        if (!libhal_ctx_init (hal_ctx, &error)) {
                if (dbus_error_is_set(&error)) {
                        fprintf (stderr, "error: libhal_ctx_init: %s: %s\n", error.name, error.message);
                        dbus_error_free (&error);
                }
                fprintf (stderr, "Could not initialise connection to hald.\n"
                         "Normally this means the HAL daemon (hald) is not running or not ready.\n");
                goto out;
        }
        
        if (udi != NULL) {
                got_lock = libhal_device_acquire_interface_lock (hal_ctx,
                                                                 udi,
                                                                 interface,
                                                                 exclusive,
                                                                 &error);
        } else {
                got_lock = libhal_acquire_global_interface_lock (hal_ctx,
                                                                 interface,
                                                                 exclusive,
                                                                 &error);
        }
                
        if (dbus_error_is_set(&error)) {
                fprintf (stderr, 
                         "error: %s: %s: %s\n", 
                         udi != NULL ? "libhal_device_acquire_interface_lock" : 
                         "libhal_acquire_global_interface_lock",
                         error.name, 
                         error.message);
                dbus_error_free (&error);
                goto out;
        }
        
        if (!got_lock) {
                goto out;
        }

        /* now run the program while holding the lock */
        if (!g_spawn_command_line_sync (run,
                                        NULL,
                                        NULL,
                                        NULL,
                                        &g_error)) {
                
                fprintf (stderr, "error: g_spawn_command_line_sync: %s\n", g_error->message);
                g_error_free (g_error);
                goto out;
        }

        ret = 0;

out:
        return ret;
}
