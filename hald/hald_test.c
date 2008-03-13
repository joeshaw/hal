/***************************************************************************
 * CVSID: $Id$
 *
 * hald_test.c : Unit tests for hald
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

#include "logger.h"
#include "hald.h"
#include "device_store.h"
#include "device_info.h"

static HalDeviceStore *global_device_list = NULL;

static HalDeviceStore *temporary_device_list = NULL;

/** This is set to #TRUE if we are probing and #FALSE otherwise */
dbus_bool_t hald_is_initialising;

/** This is set to #TRUE if we are shutting down and #FALSE otherwise */
dbus_bool_t hald_is_shutting_down;

/** If #TRUE, we will spew out debug */
dbus_bool_t hald_is_verbose = FALSE;

HalDeviceStore *
hald_get_gdl (void)
{
	if (global_device_list == NULL) {
		global_device_list = hal_device_store_new ();
#if 0		
		g_signal_connect (global_device_list,
				  "store_changed",
				  G_CALLBACK (gdl_store_changed), NULL);
		g_signal_connect (global_device_list,
				  "device_property_changed",
				  G_CALLBACK (gdl_property_changed), NULL);
		g_signal_connect (global_device_list,
				  "device_capability_added",
				  G_CALLBACK (gdl_capability_added), NULL);
#endif
	}

	return global_device_list;
}

HalDeviceStore *
hald_get_tdl (void)
{
	if (temporary_device_list == NULL) {
		temporary_device_list = hal_device_store_new ();
		
	}

	return temporary_device_list;
}

/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
check_properties (void)
{
	HalDevice *d;
	GSList *s;

	printf ("Checking HalDevice properties\n");

	d = hal_device_new ();
	if (d == NULL) {
		printf ("Cannot allocate HalDevice object\n");
		goto out;
	}

	printf ("int: ");
	hal_device_property_set_int (d, "test.int", 42);
	if (hal_device_property_get_int (d, "test.int") != 42) {
		printf ("FAILED\n");
		goto out;
	}
	printf ("PASSED\n");

	printf ("uint64: ");
	hal_device_property_set_uint64 (d, "test.uint64", ((((dbus_uint64_t)1)<<35) + 5));
	if (hal_device_property_get_uint64 (d, "test.uint64") != ((((dbus_uint64_t)1)<<35) + 5)) {
		printf ("FAILED\n");
		goto out;
	}
	printf ("PASSED\n");

	{
		double val = 0.53434343;
		double val2;

		printf ("double: ");
		hal_device_property_set_double (d, "test.double", val);
		val2 = hal_device_property_get_double (d, "test.double");
		if (memcmp (&val, &val2, sizeof (double)) != 0) {
			printf ("FAILED\n");
			goto out;
		}
		printf ("PASSED\n");
	}

	printf ("string (ASCII): ");
	hal_device_property_set_string (d, "test.string", "fooooobar22");
	if (strcmp (hal_device_property_get_string (d, "test.string"), "fooooobar22") != 0) {
		printf ("FAILED\n");
		goto out;
	}
	printf ("PASSED\n");

	printf ("string (UTF-8): ");
	hal_device_property_set_string (d, "test.string2", "fooøةמ");
	if (strcmp (hal_device_property_get_string (d, "test.string2"), "fooøةמ") != 0) {
		printf ("FAILED\n");
		goto out;
	}
	printf ("PASSED\n");

	printf ("bool: ");
	hal_device_property_set_bool (d, "test.bool", TRUE);
	if (hal_device_property_get_bool (d, "test.bool") != TRUE) {
		printf ("FAILED\n");
		goto out;
	}
	printf ("PASSED\n");

	printf ("strlist: ");
	if (!hal_device_property_strlist_append (d, "test.strlist", "foostrlist0", FALSE)) {
		printf ("FAILED00\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED01\n");
		goto out;
	}
	if (g_slist_length (s) != 1) {
		printf ("FAILED02\n");
		goto out;
	}
	if (strcmp (s->data, "foostrlist0") != 0) {
		printf ("FAILED03\n");
		goto out;
	}
	
	if (!hal_device_property_strlist_append (d, "test.strlist", "foostrlist1", FALSE)) {
		printf ("FAILED10\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED11\n");
		goto out;
	}
	if (g_slist_length (s) != 2) {
		printf ("FAILED12\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist0") != 0) {
		printf ("FAILED13\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist1") != 0) {
		printf ("FAILED14\n");
		goto out;
	}
	
	if (!hal_device_property_strlist_prepend (d, "test.strlist", "foostrlist2")) {
		printf ("FAILED20\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED21\n");
		goto out;
	}
	if (g_slist_length (s) != 3) {
		printf ("FAILED22\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist2") != 0) {
		printf ("FAILED23\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist0") != 0) {
		printf ("FAILED24\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist1") != 0) {
		printf ("FAILED25\n");
		goto out;
	}


	if (!hal_device_property_strlist_remove_elem (d, "test.strlist", 1)) {
		printf ("FAILED30\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED31\n");
		goto out;
	}
	if (g_slist_length (s) != 2) {
		printf ("FAILED32\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist2") != 0) {
		printf ("FAILED33\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist1") != 0) {
		printf ("FAILED34\n");
		goto out;
	}

	/* this add should fail because it shouldn't change the list */
	if (hal_device_property_strlist_add (d, "test.strlist", "foostrlist2")) {
		printf ("FAILED40\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED31\n");
		goto out;
	}
	if (g_slist_length (s) != 2) {
		printf ("FAILED42\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist2") != 0) {
		printf ("FAILED43\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist1") != 0) {
		printf ("FAILED44\n");
		goto out;
	}

	/* this add will succeed and it should change the list */
	if (!hal_device_property_strlist_add (d, "test.strlist", "foostrlist3")) {
		printf ("FAILED50\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED51\n");
		goto out;
	}
	if (g_slist_length (s) != 3) {
		printf ("FAILED52\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist2") != 0) {
		printf ("FAILED53\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist1") != 0) {
		printf ("FAILED54\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist3") != 0) {
		printf ("FAILED55\n");
		goto out;
	}



	/* this remove will succeed and it should change the list */
	if (!hal_device_property_strlist_remove (d, "test.strlist", "foostrlist1")) {
		printf ("FAILED60\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED61\n");
		goto out;
	}
	if (g_slist_length (s) != 2) {
		printf ("FAILED62\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist2") != 0) {
		printf ("FAILED63\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist3") != 0) {
		printf ("FAILED65\n");
		goto out;
	}

	/* this remove will succeed but it shouldn't change the list */
	if (!hal_device_property_strlist_remove (d, "test.strlist", "foostrlist1")) {
		printf ("FAILED70\n");
		goto out;
	}
	if ((s = hal_device_property_get_strlist (d, "test.strlist")) == NULL) {
		printf ("FAILED71\n");
		goto out;
	}
	if (g_slist_length (s) != 2) {
		printf ("FAILED72\n");
		goto out;
	}
	if (s == NULL || strcmp (s->data, "foostrlist2") != 0) {
		printf ("FAILED73\n");
		goto out;
	}
	s = s->next;
	if (s == NULL || strcmp (s->data, "foostrlist3") != 0) {
		printf ("FAILED75\n");
		goto out;
	}
	printf ("PASSED\n");

	printf ("property existence: ");
	if (!hal_device_has_property (d, "test.int") ||
	    !hal_device_has_property (d, "test.uint64") ||
	    !hal_device_has_property (d, "test.double") ||
	    !hal_device_has_property (d, "test.bool") ||
	    !hal_device_has_property (d, "test.string") ||
	    !hal_device_has_property (d, "test.string2") ||
	    !hal_device_has_property (d, "test.strlist")) {
		printf ("FAILED80\n");
		goto out;
	}
	if (hal_device_has_property (d, "moe") ||
	    hal_device_has_property (d, "joe") ||
	    hal_device_has_property (d, "woo")) {
		printf ("FAILED81\n");
		goto out;
	}
	printf ("PASSED\n");

	hal_device_set_udi (d, "/org/freedesktop/Hal/devices/testobj1");

	/* add this to the global device store */
	hal_device_store_add (hald_get_gdl (), d);

	return TRUE;
out:
	return FALSE;
}

static gboolean check_libhal (const char *server_addr);



static DBusHandlerResult
server_filter_function (DBusConnection * connection,
			DBusMessage * message, void *user_data)
{
    printf ("server_filter_function: obj_path=%s interface=%s method=%s destination=%s", 
	    dbus_message_get_path (message), 
	    dbus_message_get_interface (message),
	    dbus_message_get_member (message),
	    dbus_message_get_destination (message));
}

static int num_tests_done;
static gboolean num_tests_done_last_result;

static DBusHandlerResult 
server_message_handler (DBusConnection *connection, 
			DBusMessage *message, 
			void *user_data)
{
/*
	printf ("destination=%s obj_path=%s interface=%s method=%s\n", 
		dbus_message_get_destination (message), 
		dbus_message_get_path (message), 
		dbus_message_get_interface (message),
		dbus_message_get_member (message));
*/
	/* handle our TestsDone Method */
	if (dbus_message_is_method_call (message, "org.freedesktop.Hal.Tests", "TestsDone")) {
		DBusError error;
		dbus_bool_t passed;
		DBusMessage *reply;

		dbus_error_init (&error);
		if (!dbus_message_get_args (message, &error,
					    DBUS_TYPE_BOOLEAN, &passed,
					    DBUS_TYPE_INVALID)) {

			reply = dbus_message_new_error (message, "org.freedesktop.Hal.SyntaxError", "Syntax Error");
			dbus_connection_send (connection, reply, NULL);
			dbus_message_unref (reply);
			passed = FALSE;			
		} else {

			reply = dbus_message_new_method_return (message);
			dbus_connection_send (connection, reply, NULL);
			dbus_message_unref (reply);
		}

		num_tests_done++;
		num_tests_done_last_result = passed;

		return DBUS_HANDLER_RESULT_HANDLED;
       
	} else if (dbus_message_is_method_call (message, "org.freedesktop.DBus", "AddMatch")) {
		DBusMessage *reply;

                /* cheat, and handle AddMatch since libhal will try that */
		
		reply = dbus_message_new_error (message, "org.freedesktop.Hal.Error",
						"Not handled in HAL testing mode");
		if (reply == NULL)
			DIE (("No memory"));
		if (!dbus_connection_send (connection, reply, NULL))
			DIE (("No memory"));
		dbus_message_unref (reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return hald_dbus_filter_function (connection, message, user_data);
}

DBusHandlerResult 
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void
server_unregister_handler (DBusConnection *connection, void *user_data)
{
	printf ("unregistered\n");
}

static void
server_handle_connection (DBusServer *server,
			  DBusConnection *new_connection,
			  void *data)
{
	DBusObjectPathVTable vtable = { &server_unregister_handler, 
					&server_message_handler, 
					NULL, NULL, NULL, NULL};

	printf ("%d: Got a connection\n", getpid ());
	printf ("dbus_connection_get_is_connected = %d\n", dbus_connection_get_is_connected (new_connection));

	/*dbus_connection_add_filter (new_connection, server_filter_function, NULL, NULL);*/

	dbus_connection_register_fallback (new_connection, 
					   "/org/freedesktop",
					   &vtable,
					   NULL);
	dbus_connection_ref (new_connection);
	dbus_connection_setup_with_g_main (new_connection, NULL);
}


static gboolean
wait_for_external_test (void)
{
	int num_tests_done_pre;

	/* sure, this is pretty ugly. Patches welcome */

	num_tests_done_pre = num_tests_done;
	while (num_tests_done_pre == num_tests_done) {
		g_main_context_iteration (NULL, TRUE);
		if (!g_main_context_pending (NULL))
			usleep (100*1000);
	}

	return num_tests_done_last_result;
}

int
main (int argc, char *argv[])
{
	int num_tests_failed;
	DBusServer *server;
	DBusError error;
	GMainLoop *loop;

	num_tests_failed = 0;

	g_type_init ();

	loop = g_main_loop_new (NULL, FALSE);

	printf ("=============================\n");

	/* setup a server listening on a socket so we can do point to point connections 
	 * for testing libhal
	 */
	dbus_error_init (&error);
	if ((server = dbus_server_listen ("unix:tmpdir=hald-test", &error)) == NULL) { 
		printf ("Cannot create D-BUS server\n");
		num_tests_failed++;
		goto out;
	}
	printf ("server is listening at %s\n", dbus_server_get_address (server));
	dbus_server_setup_with_g_main (server, NULL);

	dbus_server_set_new_connection_function (server, server_handle_connection, NULL, NULL);

	/* this creates the /org/freedesktop/Hal/devices/testobj1 object  */
	if (!check_properties ())
		num_tests_failed++;

	/* tests of libhal against /org/freedesktop/Hal/devices/testobj1 for getting  */
/*
	if (!check_libhal (dbus_server_get_address (server)))
		num_tests_failed++;
	if (!wait_for_external_test ())
		num_tests_failed++;
*/

	printf ("=============================\n");

	printf ("Total number of tests failed: %d\n", num_tests_failed);

out:
	return num_tests_failed;
}

gboolean osspec_device_rescan (HalDevice *d)
{
  return FALSE;
}

gboolean osspec_device_reprobe (HalDevice *d)
{
  return FALSE;
}
