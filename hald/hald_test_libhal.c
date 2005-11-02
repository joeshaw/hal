/***************************************************************************
 * CVSID: $Id$
 *
 * hald_test_libhal.c : Unit tests for libhal
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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
#include <pwd.h>
#include <stdint.h>
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

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &passed);

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

		if (dbus_error_is_set (&error)) {
			printf ("FAILED98: %s\n", error.message);			
			goto fail;
		}
		printf ("SUCCESS98\n");

		libhal_device_print (ctx, "/org/freedesktop/Hal/devices/testobj1", &error);


		if (dbus_error_is_set (&error)) {
			printf ("FAILED99: %s\n", error.message);			
			goto fail;
		}
		printf ("SUCCESS99\n");

		passed = FALSE;

		{
			char *val;
			
			val = libhal_device_get_property_string (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.string", &error);

			if (val == NULL || strcmp (val, "fooooobar22") != 0 || dbus_error_is_set (&error)) {
				libhal_free_string (val);
				printf ("FAILED100\n");			
				goto fail;
			}
			printf ("SUCCESS100\n");
			libhal_free_string (val);
		}

		{
			char *val;
			val = libhal_device_get_property_string (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.string2", &error);
			if (val == NULL || strcmp (val, "fooøةמ") != 0 || dbus_error_is_set (&error)) {
				libhal_free_string (val);
				printf ("FAILED101: %s\n", error.message);			
				goto fail;
			}
			libhal_free_string (val);
			printf ("SUCCESS101\n");
		}

		{
			dbus_bool_t b;
			b = libhal_device_get_property_bool (
			    ctx, "/org/freedesktop/Hal/devices/testobj1", 
			    "test.bool", &error);
			    
			if (!b || dbus_error_is_set (&error)) {
				printf ("FAILED102: %s, %i\n", error.message, b);			
				goto fail;
			}
			printf ("SUCCESS102\n");
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
			printf ("SUCCESS103\n");
		}

		if (libhal_device_get_property_uint64 (
			    ctx, "/org/freedesktop/Hal/devices/testobj1", "test.uint64", &error) != 
		    ((((dbus_uint64_t)1)<<35) + 5) ||
		    dbus_error_is_set (&error)) {
			printf ("FAILED104: %s\n", error.message);			
			goto fail;
		}
		printf ("SUCCESS104\n");
		
		{
			char **val;
			val = libhal_device_get_property_strlist (ctx, "/org/freedesktop/Hal/devices/testobj1", "test.strlist", &error);
			if (val == NULL ||  dbus_error_is_set (&error)) {
				libhal_free_string_array (val);
				printf ("FAILED105: %s\n", error.message);			
				goto fail;
			}
			printf ("SUCCESS105\n");
			
			if (libhal_string_array_length (val) != 2) {
				libhal_free_string_array (val);
				printf ("FAILED106\n");			
				goto fail;
			}
			printf ("SUCCESS106\n");

			if (strcmp (val[0], "foostrlist2") != 0 ||
			    strcmp (val[1], "foostrlist3") != 0) {
				libhal_free_string_array (val);
				printf ("FAILED107\n");			
				goto fail;
			}
			printf ("SUCCESS107\n");

			libhal_free_string_array (val);
		}

		if (libhal_device_get_property_int (
			    ctx, "/org/freedesktop/Hal/devices/testobj1", "test.int", &error) != 42 ||
		    dbus_error_is_set (&error)) {
			printf ("FAILED108\n");			
			goto fail;
		}
		printf ("SUCCESS108\n");
	
		/* tests for libhal_psi */
		{
			int type;
			char *key;
			LibHalPropertySet *pset;
			LibHalPropertySetIterator it;
			unsigned int psi_num_passed;
			unsigned int psi_num_elems;

			if ((pset = libhal_device_get_all_properties (ctx, "/org/freedesktop/Hal/devices/testobj1", 
								      &error)) == NULL)
				return FALSE;
			printf ("SUCCESS110\n");

			psi_num_passed = 0;
			psi_num_elems = libhal_property_set_get_num_elems (pset);

			for (libhal_psi_init (&it, pset); libhal_psi_has_more (&it); libhal_psi_next (&it)) {
				type = libhal_psi_get_type (&it);
				key = libhal_psi_get_key (&it);

				switch (type) {
				case LIBHAL_PROPERTY_TYPE_STRING:
					if (strcmp (key, "info.udi") == 0) {
						if (strcmp (libhal_psi_get_string (&it), 
							    "/org/freedesktop/Hal/devices/testobj1") == 0) {
							psi_num_passed++;
						} else {
							printf ("fail on %s\n", key);
						}
					} else if (strcmp (key, "test.string") == 0) {
						if (strcmp (libhal_psi_get_string (&it), 
							    "fooooobar22") == 0) {
							psi_num_passed++;
						} else {
							printf ("fail on %s\n", key);
						}
					} else if (strcmp (key, "test.string2") == 0) {
						if (strcmp (libhal_psi_get_string (&it), 
							    "fooøةמ") == 0) {
							psi_num_passed++;
						} else {
							printf ("fail on %s\n", key);
						}
					}
					break;

				case LIBHAL_PROPERTY_TYPE_INT32:
					if (strcmp (key, "test.int") == 0) {
						if (libhal_psi_get_int (&it) == 42)
							psi_num_passed++;
						else
							printf ("fail on %s\n", key);
					}
					break;

				case LIBHAL_PROPERTY_TYPE_UINT64:
					if (strcmp (key, "test.uint64") == 0) {
						if (libhal_psi_get_uint64 (&it) == ((((dbus_uint64_t)1)<<35) + 5))
							psi_num_passed++;
						else
							printf ("fail on %s\n", key);
					}
					break;

				case LIBHAL_PROPERTY_TYPE_BOOLEAN:
					if (strcmp (key, "test.bool") == 0) {
						if (libhal_psi_get_bool (&it) == TRUE)
							psi_num_passed++;
						else
							printf ("fail on %s\n", key);
					}
					break;

				case LIBHAL_PROPERTY_TYPE_DOUBLE:
					if (strcmp (key, "test.double") == 0) {
						double val = 0.53434343;
						double val2;

						val2 = libhal_psi_get_double (&it);
						if (memcmp (&val, &val2, sizeof (double)) == 0)
							psi_num_passed++;
						else
							printf ("fail on %s\n", key);
					}
					break;

				case LIBHAL_PROPERTY_TYPE_STRLIST:
					if (strcmp (key, "test.strlist") == 0) {
						char **val;
					
						val = libhal_psi_get_strlist (&it);
						if (libhal_string_array_length (val) == 2 &&
						    strcmp (val[0], "foostrlist2") == 0 &&
						    strcmp (val[1], "foostrlist3") == 0 &&
						    val[2] == NULL)
							psi_num_passed++;
						else
							printf ("fail on %s\n", key);
					}
					break;

				default:
					printf ("    *** unknown type for key %s\n", key);
					break;
				}
			}
			libhal_free_property_set (pset);

			if (psi_num_passed != psi_num_elems) {
				printf ("FAILED111\n");			
				goto fail;
			}
			printf ("SUCCESS111\n");			


		} /* end libhal_test_psi */

	
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
