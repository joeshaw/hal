/***************************************************************************
 * CVSID: $Id$
 *
 * lshal.c : Show devices managed by HAL
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#include <glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>

#include <libhal/libhal.h>

/**
 * @defgroup HalLsHal  List HAL devices
 * @ingroup HalMisc
 *
 * @brief A commandline tool, lshal, for displaying and, optionally,
 *        monitor the devices managed by the HAL daemon. Uses libhal.
 *
 * @{
 */

/** Macro for terminating the program on an unrecoverable error */
#define DIE(expr) do {printf("*** [DIE] %s:%s():%d : ", __FILE__, __FUNCTION__, __LINE__); printf expr; printf("\n"); exit(1); } while(0)

static LibHalContext *hal_ctx;

/** Dump all devices to stdout
 *
 */
static void
dump_devices (void)
{
	int i;
	int num_devices;
	char **device_names;
	DBusError error;

	dbus_error_init (&error);

	device_names = libhal_get_all_devices (hal_ctx, &num_devices, &error);
	if (device_names == NULL)
		DIE (("Couldn't obtain list of devices\n"));

	printf ("\n"
		"Dumping %d device(s) from the Global Device List:\n"
		"-------------------------------------------------\n",
		num_devices);

	for (i = 0; i < num_devices; i++) {
		LibHalPropertySet *props;
		LibHalPropertySetIterator it;
		int type;

		props = libhal_device_get_all_properties (hal_ctx, device_names[i], &error);

		/* NOTE NOTE NOTE: This may be NULL if the device was removed
		 *                 in the daemon; this is because 
		 *                 hal_device_get_all_properties() is a in
		 *                 essence an IPC call and other stuff may 
		 *                 be happening..
		 */
		if (props == NULL)
			continue;

		printf ("udi = '%s'\n", device_names[i]);

		for (libhal_psi_init (&it, props); libhal_psi_has_more (&it); libhal_psi_next (&it)) {
			type = libhal_psi_get_type (&it);
			switch (type) {
			case LIBHAL_PROPERTY_TYPE_STRING:
				printf ("  %s = '%s'  (string)\n",
					libhal_psi_get_key (&it),
					libhal_psi_get_string (&it));
				break;

			case LIBHAL_PROPERTY_TYPE_INT32:
				printf ("  %s = %d  (0x%x)  (int)\n",
					libhal_psi_get_key (&it),
					libhal_psi_get_int (&it),
					libhal_psi_get_int (&it));
				break;

			case LIBHAL_PROPERTY_TYPE_UINT64:
				printf ("  %s = %lld  (0x%llx)  (uint64)\n",
					libhal_psi_get_key (&it),
					libhal_psi_get_uint64 (&it),
					libhal_psi_get_uint64 (&it));
				break;

			case LIBHAL_PROPERTY_TYPE_DOUBLE:
				printf ("  %s = %g  (double)\n",
					libhal_psi_get_key (&it),
					libhal_psi_get_double (&it));
				break;

			case LIBHAL_PROPERTY_TYPE_BOOLEAN:
				printf ("  %s = %s  (bool)\n",
					libhal_psi_get_key (&it),
					libhal_psi_get_bool (&it) ? "true" :
					"false");
				break;

			case LIBHAL_PROPERTY_TYPE_STRLIST:
			{
				unsigned int i;
				char **strlist;

				printf ("  %s = {", libhal_psi_get_key (&it));

				strlist = libhal_psi_get_strlist (&it);
				for (i = 0; strlist[i] != 0; i++) {
					printf ("'%s'", strlist[i]);
					if (strlist[i+1] != NULL)
						printf (", ");
				}
				printf ("} (string list)\n");
				break;
			}

			default:
				printf ("Unknown type %d=0x%02x\n", type, type);
				break;
			}
		}
		libhal_free_property_set (props);
		printf ("\n");
	}

	libhal_free_string_array (device_names);

	printf ("\n"
		"Dumped %d device(s) from the Global Device List:\n"
		"------------------------------------------------\n",
		num_devices);

	printf ("\n");
}

/** Invoked when a device is added to the Global Device List. Simply prints
 *  a message on stderr.
 *
 *  @param  udi                 Universal Device Id
 */
static void
device_added (LibHalContext *ctx,
	      const char *udi)
{
	fprintf (stderr, "*** lshal: device_added, udi='%s'\n", udi);
	/*dump_devices ();*/
}

/** Invoked when a device is removed from the Global Device List. Simply
 *  prints a message on stderr.
 *
 *  @param  udi                 Universal Device Id
 */
static void
device_removed (LibHalContext *ctx,
		const char *udi)
{
	fprintf (stderr, "*** lshal: device_removed, udi='%s'\n", udi);
	/*dump_devices ();*/
}

/** Invoked when device in the Global Device List acquires a new capability.
 *  Prints the name of the capability to stderr.
 *
 *  @param  udi                 Universal Device Id
 *  @param  capability          Name of capability
 */
static void
device_new_capability (LibHalContext *ctx,
		       const char *udi, 
		       const char *capability)
{
	fprintf (stderr, "*** lshal: new_capability, udi='%s'\n", udi);
	fprintf (stderr, "*** capability: %s\n", capability);
	/*dump_devices(); */
}

/** Invoked when device in the Global Device List loses a capability.
 *  Prints the name of the capability to stderr.
 *
 *  @param  udi                 Universal Device Id
 *  @param  capability          Name of capability
 */
static void
device_lost_capability (LibHalContext *ctx,
			const char *udi, 
			const char *capability)
{
	fprintf (stderr, "*** lshal: lost_capability, udi='%s'\n", udi);
	fprintf (stderr, "*** capability: %s\n", capability);
	/*dump_devices(); */
}

/** Acquires and prints the value of of a property to stderr.
 *
 *  @param  udi                 Universal Device Id
 *  @param  key                 Key of property
 */
static void
print_property (const char *udi, const char *key)
{
	int type;
	char *str;
	DBusError error;

	dbus_error_init (&error);

	type = libhal_device_get_property_type (hal_ctx, udi, key, &error);

	switch (type) {
	case LIBHAL_PROPERTY_TYPE_STRING:
		str = libhal_device_get_property_string (hal_ctx, udi, key, &error);
		fprintf (stderr, "*** new value: '%s'  (string)\n", str);
		libhal_free_string (str);
		break;
	case LIBHAL_PROPERTY_TYPE_INT32:
		{
			dbus_int32_t value = libhal_device_get_property_int (hal_ctx, udi, key, &error);
			fprintf (stderr,
				 "*** new value: %d (0x%x)  (int)\n",
				 value, value);
		}
		break;
	case LIBHAL_PROPERTY_TYPE_UINT64:
		{
			dbus_uint64_t value = libhal_device_get_property_uint64 (hal_ctx, udi, key, &error);
			fprintf (stderr,
				 "*** new value: %lld (0x%llx)  (uint64)\n",
				 value, value);
		}
		break;
	case LIBHAL_PROPERTY_TYPE_DOUBLE:
		fprintf (stderr, "*** new value: %g  (double)\n",
			 libhal_device_get_property_double (hal_ctx, udi, key, &error));
		break;
	case LIBHAL_PROPERTY_TYPE_BOOLEAN:
		fprintf (stderr, "*** new value: %s  (bool)\n",
			 libhal_device_get_property_bool (hal_ctx, udi, key, &error) ? "true" : "false");
		break;
	case LIBHAL_PROPERTY_TYPE_STRLIST:
	{
		unsigned int i;
		char **strlist;
		
		fprintf (stderr, "*** new value: {");

		strlist = libhal_device_get_property_strlist (hal_ctx, udi, key, &error);
		for (i = 0; strlist[i] != 0; i++) {
			fprintf (stderr, "'%s'", strlist[i]);
			if (strlist[i+1] != NULL)
				fprintf (stderr, ", ");
		}
		fprintf (stderr, "}  (string list)\n");
		libhal_free_string_array (strlist);
		break;
	}

	default:
		fprintf (stderr, "Unknown type %d='%c'\n", type, type);
		break;
	}
}

/** Invoked when a property of a device in the Global Device List is
 *  changed, and we have we have subscribed to changes for that device.
 *
 *  @param  udi                 Univerisal Device Id
 *  @param  key                 Key of property
 */
static void
property_modified (LibHalContext *ctx,
		   const char *udi, 
		   const char *key,
		   dbus_bool_t is_removed, 
		   dbus_bool_t is_added)
{
	fprintf (stderr, "*** lshal: property_modified, udi=%s, key=%s\n",
		 udi, key);
	fprintf (stderr, "           is_removed=%s, is_added=%s\n",
		 is_removed ? "true" : "false",
		 is_added ? "true" : "false");
	if (!is_removed)
		print_property (udi, key);
	fprintf (stderr, "\n");
	/*dump_devices(); */
}


/** Invoked when a property of a device in the Global Device List is
 *  changed, and we have we have subscribed to changes for that device.
 *
 *  @param  udi                 Univerisal Device Id
 *  @param  condition_name      Name of condition
 *  @param  message             D-BUS message with parameters
 */
static void
device_condition (LibHalContext *ctx,
		  const char *udi, 
		  const char *condition_name,
		  const char *condition_details)
{
	fprintf (stderr, "*** lshal: device_condition, udi=%s\n", udi);
	fprintf (stderr, "           condition_name=%s\n", condition_name);
	fprintf (stderr, "           condition_details=%s\n", condition_details);
	fprintf (stderr, "\n");
	/*dump_devices(); */
}



/** Print out program usage.
 *
 *  @param  argc                Number of arguments given to program
 *  @param  argv                Arguments given to program
 */
static void
usage (int argc, char *argv[])
{
	fprintf (stderr, "\n" "usage : %s --monitor [--help]\n", argv[0]);
	fprintf (stderr,
		 "\n"
		 "        --monitor        Monitor device list\n"
		 "        --help           Show this information and exit\n"
		 "\n"
		 "Shows all devices and their properties. If the --monitor option is given\n"
		 "then the device list and all devices are monitored for changes.\n"
		 "\n");
}

/** Entry point
 *
 *  @param  argc                Number of arguments given to program
 *  @param  argv                Arguments given to program
 *  @return                     Return code
 */
int
main (int argc, char *argv[])
{
	DBusError error;
	dbus_bool_t do_monitor = FALSE;
	GMainLoop *loop;
	DBusConnection *conn;

	fprintf (stderr, "lshal version " PACKAGE_VERSION "\n");

	while (1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
			{"monitor", 0, NULL, 0},
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
			} else if (strcmp (opt, "monitor") == 0) {
				do_monitor = TRUE;
			}
			break;

		default:
			usage (argc, argv);
			return 1;
			break;
		}
	}

	if (do_monitor)
		loop = g_main_loop_new (NULL, FALSE);
	else
		loop = NULL;

	dbus_error_init (&error);	
	conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		fprintf (stderr, "error: dbus_bus_get: %s: %s\n", error.name, error.message);
		return 1;
	}

	if (do_monitor)
		dbus_connection_setup_with_g_main (conn, NULL);

	if ((hal_ctx = libhal_ctx_new ()) == NULL) {
		fprintf (stderr, "error: libhal_ctx_new\n");
		return 1;
	}
	if (!libhal_ctx_set_dbus_connection (hal_ctx, conn)) {
		fprintf (stderr, "error: libhal_ctx_set_dbus_connection: %s: %s\n", error.name, error.message);
		return 1;
	}
	if (!libhal_ctx_init (hal_ctx, &error)) {
		fprintf (stderr, "error: libhal_ctx_init: %s: %s\n", error.name, error.message);
		return 1;
	}

	libhal_ctx_set_device_added (hal_ctx, device_added);
	libhal_ctx_set_device_removed (hal_ctx, device_removed);
	libhal_ctx_set_device_new_capability (hal_ctx, device_new_capability);
	libhal_ctx_set_device_lost_capability (hal_ctx, device_lost_capability);
	libhal_ctx_set_device_property_modified (hal_ctx, property_modified);
	libhal_ctx_set_device_condition (hal_ctx, device_condition);

	dump_devices ();

	/* run the main loop only if we should monitor */
	if (do_monitor && loop != NULL) {
		libhal_device_property_watch_all (hal_ctx, &error);
		g_main_loop_run (loop);
	}

	libhal_ctx_shutdown (hal_ctx, &error);
	libhal_ctx_free (hal_ctx);

	dbus_connection_disconnect (conn);
	dbus_connection_unref (conn);
	return 0;
}

/**
 * @}
 */
