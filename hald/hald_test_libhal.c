/***************************************************************************
 * CVSID: $Id$
 *
 * hald_test_libhal.c : Unit tests for libhal
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <grp.h>
#include <syslog.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"


/* TODO: All this needs work */

gboolean
check_libhal (const char *server_addr)
{
	pid_t child_pid;

	child_pid = fork ();
	if (child_pid == -1) {
		printf ("Cannot fork\n");
		exit (1);
	} else if (child_pid == 0) {
		DBusError error;
		DBusConnection *conn;
		LibHalContext *ctx;

		printf ("server address='%s'\n", server_addr);

		dbus_error_init (&error);
		if ((conn = dbus_connection_open (server_addr, &error)) == NULL) {
			printf ("Error connecting to server: %s\n", error.message);
			/* TODO: handle */
		}

		dbus_connection_setup_with_g_main (conn, NULL);

		if ((ctx = libhal_ctx_new ()) == NULL) {
			printf ("Error getting libhal context\n");
			/* TODO: handle */
		}

		libhal_ctx_set_dbus_connection (ctx, conn);
		libhal_ctx_init (ctx, &error);
		printf ("got %s\n", libhal_device_get_property_string (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.string", &error));
		libhal_device_print (ctx, "/org/freedesktop/Hal/devices/testobj1", &error);

	} else {
		printf ("child pid=%d\n", child_pid);
	}
	return TRUE;
}
