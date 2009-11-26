/***************************************************************************
 * CVSID: $Id$
 *
 * hal-disable-polling.c : Disable polling on a drive
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

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <glib.h>
#include <libhal.h>
#include "../hald/util.h"

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
                 "usage : hal-disable-polling [--udi <udi> | --device <device-file>]\n"
                 "                            [--enable-polling]\n"
                 "                            [--help] [--version]\n");
	fprintf (stderr,
                 "\n"
                 "        --udi            Unique Device Id\n"
                 "        --device         Device file\n"
                 "        --enable-polling Enable polling instead of disabling it\n"
                 "        --version        Show version and exit\n"
                 "        --help           Show this information and exit\n"
                 "\n"
                 "This program is provided to make HAL stop polling a drive. Please read.\n"
                 "the entire manual page before using this program.\n"
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
	char *device = NULL;
        dbus_bool_t is_version = FALSE;
        dbus_bool_t enable_polling = FALSE;
	DBusError error;
        LibHalContext *hal_ctx;
        FILE *f;
        char *filename;
	char *basename;

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
			{"device", 1, NULL, 0},
                        {"enable-polling", 0, NULL, 0},
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
			} else if (strcmp (opt, "device") == 0) {
				device = strdup (optarg);
			} else if (strcmp (opt, "enable-polling") == 0) {
				enable_polling = TRUE;
			}
			break;

		default:
			usage (argc, argv);
			return 1;
			break;
		}
	}

	if (is_version) {
		printf ("hal-disable-polling " PACKAGE_VERSION "\n");
		return 0;
	}

	if (udi == NULL && device == NULL) {
		usage (argc, argv);
		return 1;
	}

	if (udi != NULL && device != NULL) {
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

        if (getuid () != 0) {
		fprintf (stderr, "This program requires super user (root) privileges.\n");
                return 1;
        }

        if (device != NULL) {
                char **devices;
                int num_devices;
                int n;

                devices = libhal_manager_find_device_string_match (hal_ctx, "block.device", device, &num_devices, NULL);
		if (devices == NULL || devices[0] == NULL) {
			char real_device[HAL_PATH_MAX];

			if (realpath(device, real_device) == NULL) {
                                fprintf (stderr, "Cannot find device %s.\n", device);
                                return 1;
                        }

                        devices = libhal_manager_find_device_string_match (hal_ctx, "block.device", real_device,
                                &num_devices, NULL);

                        if (devices == NULL) {
                                fprintf (stderr, "Cannot find symlinked device %s -> %s.\n", device, real_device);
                                return 1;
                        }

                        fprintf (stderr, "Following symlink from %s to %s.\n", device, real_device);
                }

                for (n = 0; devices[n] != NULL; n++) {
                        if (libhal_device_query_capability (hal_ctx, devices[n], "storage", NULL)) {
                                udi = devices[n];
                                break;
                        }
                }

                if (udi == NULL) {
                        fprintf (stderr, "Cannot find storage device %s.\n", device);
                        return 1;
                }

                /* mmmkay, we don't care about leaking the variable devices... mmkay? mmkay! */
        } else {
                if (!libhal_device_exists (hal_ctx, udi, &error)) {
                        fprintf (stderr, "Cannot find device with udi %s.\n", udi);
                        return 1;
                }
                if (!libhal_device_query_capability (hal_ctx, udi, "storage", NULL)) {
                        fprintf (stderr, "Device with udi %s is not a storage device.\n", udi);
                        return 1;
                }
                device = libhal_device_get_property_string (hal_ctx, udi, "block.device", NULL);
                if (device == NULL) {
                        fprintf (stderr, "Device with udi %s does not have block.device set.\n", udi);
                        return 1;
                }
        }

        if (!libhal_device_get_property_bool (hal_ctx, udi, "storage.removable", NULL)) {
                fprintf (stderr, "The given drive don't use removable media so it's not polled anyway.\n");
                return 1;
        }


	basename = g_path_get_basename (udi); 
        filename = g_strdup_printf (PACKAGE_SYSCONF_DIR "/hal/fdi/information/media-check-disable-%s.fdi",
                                    basename);
	g_free (basename);

        if (enable_polling) {
                if (libhal_device_get_property_bool (hal_ctx, udi, "storage.media_check_enabled", NULL)) {
                        fprintf (stderr, "Polling is already enabled on the given drive.\n");
                        return 1;
                }

                if (!g_file_test (filename, G_FILE_TEST_EXISTS)) {
                        fprintf (stderr, "Cannot find fdi file %s. Perhaps polling wasn't disabled using this tool?\n", filename);
                        return 1;
                }
                if (unlink (filename) != 0) {
                        fprintf (stderr, "Cannot delete fdi file %s.\n", filename);
                        return 1;
                }
        } else {
                if (!libhal_device_get_property_bool (hal_ctx, udi, "storage.media_check_enabled", NULL)) {
                        fprintf (stderr, "Polling is already disabled on the given drive.\n");
                        return 1;
                }

                if (g_file_test (filename, G_FILE_TEST_EXISTS)) {
                        fprintf (stderr, "The fdi file %s already exist. Cowardly refusing to overwrite it.\n", filename);
                        return 1;
                }

                f = fopen (filename, "w");
                if (f == NULL) {
                        fprintf (stderr, "Cannot open %s for writing.\n", filename);
                        return 1;
                }
                fprintf (f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "\n"
                         "<deviceinfo version=\"0.2\">\n"
                         "  <device>\n"
                         "    <match key=\"info.udi\" string=\"%s\">\n"
                         "      <merge key=\"storage.media_check_enabled\" type=\"bool\">false</merge>\n"
                         "    </match>\n"
                         "  </device>\n"
                         "</deviceinfo>\n"
                         "\n", udi);
                fclose (f);
        }

        libhal_device_reprobe (hal_ctx, udi, &error);

        if (enable_polling)
                printf ("Polling for drive %s have been enabled. The fdi file deleted was\n"
                        "  %s\n", device, filename);
        else
                printf ("Polling for drive %s have been disabled. The fdi file written was\n"
                        "  %s\n", device, filename);

        return 0;
}
