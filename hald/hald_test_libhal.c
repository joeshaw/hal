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

static void
send_tests_done (DBusConnection *conn, dbus_bool_t passed)
{
	int i;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Tests",
						"org.freedesktop.Hal.Tests",
						"TestsDone");
	if (message == NULL) {
		fprintf (stderr, "%s %d : Couldn't allocate D-BUS message\n", __FILE__, __LINE__);
		goto out;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_boolean (&iter, passed);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (conn, message, -1, &error);
	if (dbus_error_is_set (&error)) {
		dbus_message_unref (message);
		fprintf (stderr, "%s %d : Error sending message: %s: %s\n", 
			 __FILE__, __LINE__, error.name, error.message);
		return;
	}

	if (reply == NULL) {
		dbus_message_unref (message);
		fprintf (stderr, "%s %d : No reply!\n", __FILE__, __LINE__);
		return;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);
out:
	;
}



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
		dbus_bool_t passed;

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
		libhal_device_print (ctx, "/org/freedesktop/Hal/devices/testobj1", &error);

		passed = FALSE;

		{
			char *val;
			val = libhal_device_get_property_string (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.string", &error);
			if (val == NULL || strcmp (val, "fooooobar22") != 0 || dbus_error_is_set (&error)) {
				libhal_free_string (val);
				printf ("FAILED100\n");			
				goto fail;
			}
			libhal_free_string (val);
		}

		{
			char *val;
			val = libhal_device_get_property_string (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.string2", &error);
			if (val == NULL || strcmp (val, "fooøةמ") != 0 || dbus_error_is_set (&error)) {
				libhal_free_string (val);
				printf ("FAILED100\n");			
				goto fail;
			}
			libhal_free_string (val);
		}


		if (libhal_device_get_property_bool (
			    ctx, "/org/freedesktop/Hal/devices/testobj1", "test.bool", &error) != TRUE ||
		    dbus_error_is_set (&error)) {
			printf ("FAILED102\n");			
			goto fail;
		}

		{
			double val;
			double expected_val = 0.53434343;

			val = libhal_device_get_property_double (ctx, "/org/freedesktop/Hal/devices/testobj1", 
								 "test.double", &error);
			if ( memcmp (&val, &expected_val, sizeof (double)) != 0 || dbus_error_is_set (&error)) {
				printf ("FAILED103\n");
				goto fail;
			}
		}

		if (libhal_device_get_property_uint64 (
			    ctx, "/org/freedesktop/Hal/devices/testobj1", "test.uint64", &error) != 
		    ((((dbus_uint64_t)1)<<35) + 5) ||
		    dbus_error_is_set (&error)) {
			printf ("FAILED104\n");			
			goto fail;
		}

		{
			char **val;
			val = libhal_device_get_property_strlist (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.strlist", &error);
			if (val == NULL ||  dbus_error_is_set (&error)) {
				libhal_free_string_array (val);
				printf ("FAILED105\n");			
				goto fail;
			}

			if (libhal_string_array_length (val) != 2) {
				libhal_free_string_array (val);
				printf ("FAILED106\n");			
				goto fail;
			}

			if (strcmp (val[0], "foostrlist2") != 0 ||
			    strcmp (val[1], "foostrlist3") != 0) {
				libhal_free_string_array (val);
				printf ("FAILED107\n");			
				goto fail;
			}

			libhal_free_string_array (val);
		}

		if (libhal_device_get_property_int (
			    ctx, "/org/freedesktop/Hal/devices/testobj1", "test.int", &error) != 42 ||
		    dbus_error_is_set (&error)) {
			printf ("FAILED108\n");			
			goto fail;
		}

		printf ("Passed all libhal tests\n");
		passed = TRUE;

	fail:

		send_tests_done (conn, passed);
		exit (1);

	} else {
		printf ("child pid=%d\n", child_pid);
	}
	return TRUE;
}
