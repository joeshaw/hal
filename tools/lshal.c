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
dump_devices ()
{
	int i;
	int num_devices;
	char **device_names;

	device_names = hal_get_all_devices (hal_ctx, &num_devices);

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

		props = hal_device_get_all_properties (hal_ctx, 
						       device_names[i]);

		/* NOTE NOTE NOTE: This may be NULL if the device was removed
		 *                 in the daemon; this is because 
		 *                 hal_device_get_all_properties() is a in
		 *                 essence an IPC call and other stuff may 
		 *                 be happening..
		 */
		if (props == NULL)
			continue;

		printf ("udi = '%s'\n", device_names[i]);

		for (hal_psi_init (&it, props); hal_psi_has_more (&it);
		     hal_psi_next (&it)) {
			type = hal_psi_get_type (&it);
			switch (type) {
			case DBUS_TYPE_STRING:
				printf ("  %s = '%s'  (string)\n",
					hal_psi_get_key (&it),
					hal_psi_get_string (&it));
				break;

			case DBUS_TYPE_INT32:
				printf ("  %s = %d  (0x%x)  (int)\n",
					hal_psi_get_key (&it),
					hal_psi_get_int (&it),
					hal_psi_get_int (&it));
				break;

			case DBUS_TYPE_DOUBLE:
				printf ("  %s = %g  (double)\n",
					hal_psi_get_key (&it),
					hal_psi_get_double (&it));
				break;

			case DBUS_TYPE_BOOLEAN:
				printf ("  %s = %s  (bool)\n",
					hal_psi_get_key (&it),
					hal_psi_get_bool (&it) ? "true" :
					"false");
				break;
			}
		}
		hal_free_property_set (props);
		printf ("\n");
	}

	hal_free_string_array (device_names);

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
	dump_devices ();
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
	dump_devices ();
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

	type = hal_device_get_property_type (hal_ctx, udi, key);

	switch (type) {
	case DBUS_TYPE_STRING:
		str = hal_device_get_property_string (hal_ctx, udi, key);
		fprintf (stderr, "*** new value: '%s'  (string)\n", str);
		hal_free_string (str);
		break;
	case DBUS_TYPE_INT32:
		{
			dbus_int32_t value =
			    hal_device_get_property_int (hal_ctx, udi, key);
			fprintf (stderr,
				 "*** new value: %d (0x%x)  (int)\n",
				 value, value);
		}
		break;
	case DBUS_TYPE_DOUBLE:
		fprintf (stderr, "*** new value: %g  (double)\n",
			 hal_device_get_property_double (hal_ctx, udi, key));
		break;
	case DBUS_TYPE_BOOLEAN:
		fprintf (stderr, "*** new value: %s  (bool)\n",
			 hal_device_get_property_bool (hal_ctx, udi,
						       key) ? "true" :
			 "false");
		break;

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
		  DBusMessage * message)
{
	fprintf (stderr, "*** lshal: device_condition, udi=%s\n", udi);
	fprintf (stderr, "           condition_name=%s\n", condition_name);
	/** @todo FIXME print out message */
	fprintf (stderr, "\n");
	/*dump_devices(); */
}


/** Invoked by libhal for integration with our mainloop. We take the
 *  easy route and use link with glib for painless integrate.
 *
 *  @param  dbus_connection     D-BUS connection to integrate
 */
static void
mainloop_integration (LibHalContext *ctx, DBusConnection * dbus_connection)
{
	dbus_connection_setup_with_g_main (dbus_connection, NULL);
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
	dbus_bool_t do_monitor = FALSE;
	GMainLoop *loop;
	LibHalFunctions hal_functions = { mainloop_integration,
		device_added,
		device_removed,
		device_new_capability,
		device_lost_capability,
		property_modified,
		device_condition
	};

	fprintf (stderr, "lshal version " PACKAGE_VERSION "\n");

	loop = g_main_loop_new (NULL, FALSE);

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


	if ((hal_ctx = hal_initialize (&hal_functions, FALSE)) == NULL) {
		fprintf (stderr, "error: hal_initialize failed\n");
		exit (1);
	}

	dump_devices ();

	/* run the main loop only if we should monitor */
	if (do_monitor) {
		hal_device_property_watch_all (hal_ctx);
		g_main_loop_run (loop);
	}

	hal_shutdown (hal_ctx);
	return 0;
}


/**
 * @}
 */
