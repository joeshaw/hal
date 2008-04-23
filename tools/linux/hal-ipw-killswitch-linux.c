/***************************************************************************
 * CVSID: $Id$
 *
 * Copyright (C) 2007 Adel Gadllah <adel.gadllah@gmail.com>
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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <stdlib.h>

#include <libhal.h>

static LibHalContext *hal_ctx;

int main(int argc,char** argv) {

	DBusError error;

	char *udi;
	char *parent;
	char *iface;
	int i, kill_status;
	char **udis;
	int num_udis;
	FILE *fd;
	char *path;
	int ret = -1;

	if (argc == 1) 
		return -1;	

	dbus_error_init (&error);
	
	if ((udi = getenv ("HAL_PROP_INFO_UDI")) == NULL) return -1;

	if ((hal_ctx = libhal_ctx_new ()) == NULL) {
		syslog (LOG_INFO, "error: libhal_ctx_new\n");
		return -1;
	}

	if (!libhal_ctx_set_dbus_connection (hal_ctx, dbus_bus_get (DBUS_BUS_SYSTEM, &error))) {
		syslog (LOG_INFO, "error: libhal_ctx_set_dbus_connection: %s: %s\n", error.name, error.message);
		LIBHAL_FREE_DBUS_ERROR (&error);
		return -1;
	}

	if (!libhal_ctx_init (hal_ctx, &error)) {
		syslog (LOG_INFO, "error: libhal_ctx_init: %s: %s\n", error.name, error.message);
		LIBHAL_FREE_DBUS_ERROR (&error);
		return -1;
	}


	parent = libhal_device_get_property_string (hal_ctx, udi, "info.parent", &error);
	udis = libhal_manager_find_device_string_match (hal_ctx, "info.parent", parent, &num_udis, &error);
	
	if( argc==2 && strcmp("getrfkill",argv[1])==0) {

		for (i = 0; i < num_udis; i++) {
			char buf[64];

			if (strcmp (udis[i], udi) == 0) 
				continue;

			iface = libhal_device_get_property_string (hal_ctx, udis[i], "net.interface", &error);
			if (iface != NULL) {
				path = g_strdup_printf ("/sys/class/net/%s/device/rf_kill", iface);

				if ((fd = fopen (path, "r")) == NULL) {
					return -1;
				}
				if (fgets (buf, sizeof (buf), fd) == NULL) {
					return -1;
				}

				errno = 0;
				kill_status = strtol (buf, NULL, 10);
				if (errno == 0) {
					/* syslog (LOG_INFO, "'%s' returned %d", path, kill_status); */
					
					switch(kill_status) {
						case 0:
							ret = 0;
							break;
						case 1:
						case 2:
						case 3:
							ret = 1;
							break;
						default:
							break;
					}
				}

				fclose (fd);
				g_free (path);
				libhal_free_string (iface);
			}
		}
	}

	if (argc == 3 && strcmp ("setrfkill", argv[1]) == 0 && (atoi (argv[2]) == 0 || atoi(argv[2]) == 1)) {

		for (i = 0; i < num_udis; i++) {
			if (strcmp (udis[i], udi) == 0) 
				continue;

			iface = libhal_device_get_property_string (hal_ctx, udis[i], "net.interface", &error);
			if (iface != NULL) {
				path = g_strdup_printf ("/sys/class/net/%s/device/rf_kill", iface);
				
				if ((fd = fopen (path, "w")) == NULL) {
					return -1;
				}

				fputc (argv[2][0], fd);
				fclose (fd);

				g_free (path);
				libhal_free_string (iface);
			}
		}
	
		ret = 0;
	}
	
	libhal_free_string (parent);
	libhal_free_string_array (udis);
	libhal_ctx_free (hal_ctx);

	if (dbus_error_is_set (&error)) {
		syslog (LOG_INFO, "error: %s: %s\n", error.name, error.message);
		dbus_error_free (&error);
		return -1;
	}

	return ret;
}
