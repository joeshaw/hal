/***************************************************************************
 * CVSID: $Id$
 *
 * lshal.c : Show devices managed by HAL
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Pierre Ossman, <drzeus@drzeus.cx>
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
#include <time.h>
#include <sys/time.h>

#include <glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>
#include <libhal.h>

#ifdef __SUNPRO_C
#define __FUNCTION__ __func__
#endif

/* Macro for terminating the program on an unrecoverable error */
#define DIE(expr) do {printf("*** [DIE] %s:%s():%d : ", __FILE__, __FUNCTION__, __LINE__); printf expr; printf("\n"); exit(1); } while(0)

#define UDI_BASE "/org/freedesktop/Hal/devices/"

static LibHalContext *hal_ctx;
static dbus_bool_t long_list = FALSE;
static dbus_bool_t tree_view = FALSE;
static dbus_bool_t short_list = FALSE;
static char *show_device = NULL;

struct Device {
	char *name;
	char *parent;
};

/** 
 *  short_name:
 *  @udi:               Universal Device Id
 * 
 *  Returns:		short name of a device
 *
 *  Generate a short name for a device 
 */
static const char *
short_name (const char *udi)
{
	return &udi[sizeof(UDI_BASE) - 1];
}

static char *
get_time (void)
{
	struct timeval tnow;
	struct tm *tlocaltime;
	struct timezone tzone;
	static char tbuf[256];
	static char buf[256];
	GTimeVal t;

	gettimeofday (&tnow, &tzone);
	tlocaltime = localtime ((time_t *) &tnow.tv_sec);
	strftime (tbuf, sizeof (tbuf), "%H:%M:%S", tlocaltime);

	g_get_current_time (&t);
	g_snprintf (buf, sizeof(buf), "%s.%03d", tbuf, (int) (t.tv_usec / 1000));
	return buf;
}

/** 
 *  print_props:
 *  @udi:                Universal Device Id
 *
 *  Print all properties of a device 
 */
static void
print_props (const char *udi)
{
	DBusError error;
	LibHalPropertySet *props;
	LibHalPropertySetIterator it;
	int type;

	dbus_error_init (&error);

	props = libhal_device_get_all_properties (hal_ctx, udi, &error);

	/* NOTE : This may be NULL if the device was removed
	 *        in the daemon; this is because
	 *        hal_device_get_all_properties() is a in
	 *        essence an IPC call and other stuff may
	 *        be happening..
	 */
	if (props == NULL) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		return;
	}

	libhal_property_set_sort (props);

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
			printf ("  %s = %llu  (0x%llx)  (uint64)\n",
				libhal_psi_get_key (&it),
				(long long unsigned int) libhal_psi_get_uint64 (&it),
				(long long unsigned int) libhal_psi_get_uint64 (&it));
			break;

		case LIBHAL_PROPERTY_TYPE_DOUBLE:
			printf ("  %s = %.1f (%g) (double)\n",
				libhal_psi_get_key (&it),
				libhal_psi_get_double (&it),
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
			for (i = 0; strlist[i] != NULL; i++) {
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
}

/** 
 *  dump_device:
 *  @udi:                 Universal Device Id
 *
 *  Dumps information about a single device 
 */
static void
dump_device (const char *udi)
{
	DBusError error;

	dbus_error_init (&error);

	if (!libhal_device_exists (hal_ctx, udi, &error)) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		return;
	}

	if (long_list) {
		printf ("udi = '%s'\n", udi);

		print_props (udi);
		printf ("\n");
	}
	else
		printf ("%s\n", short_name (udi));
}

/** 
 *  dump_children:
 *  @udi:                 Universal Device Id of parent
 *  @num_devices:         Total number of devices in device list
 *  @devices:             List of devices
 *  @depth:               Current recursion depth
 *
 *  Dump all children of device 
 */
static void
dump_children (char *udi, int num_devices, struct Device *devices, int depth)
{
	int i;

	for (i = 0; i < num_devices; i++) {
		if (!udi) {
			if (devices[i].parent)
				continue;
		}
		else {
			if (!devices[i].parent)
				continue;
			if (strcmp (devices[i].parent, udi))
				continue;
		}

		if (long_list)
			printf ("udi = '%s'\n", devices[i].name);
		else {
			int j;
			if (tree_view) {
				for (j = 0;j < depth;j++)
					printf("  ");
			}
			printf ("%s\n", short_name (devices[i].name));
		}

		if (long_list) {
			print_props (devices[i].name);
			printf ("\n");
		}

		dump_children(devices[i].name, num_devices, devices, depth + 1);
	}
}

/** 
 *  dump_devices:
 *  
 *  Dump all devices to stdout
 */
static void
dump_devices (void)
{
	int i;
	int num_devices;
	char **device_names;
	struct Device *devices;
	DBusError error;

	dbus_error_init (&error);

	device_names = libhal_get_all_devices (hal_ctx, &num_devices, &error);
	if (device_names == NULL) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		DIE (("Couldn't obtain list of devices\n"));
	}

	devices = malloc (sizeof(struct Device) * num_devices);
	if (!devices) {
		libhal_free_string_array (device_names);
		return;
	}

	for (i = 0;i < num_devices;i++) {
		devices[i].name = device_names[i];
		devices[i].parent = libhal_device_get_property_string (hal_ctx,
				device_names[i], "info.parent", &error);

		if (dbus_error_is_set (&error)) {
			/* Free the error (which include a dbus_error_init())
			   This should prevent errors if a call above fails */
			dbus_error_free (&error);
		}
	}

	if (long_list) {
		printf ("\n"
			"Dumping %d device(s) from the Global Device List:\n"
			"-------------------------------------------------\n",
			num_devices);
	}

	dump_children(NULL, num_devices, devices, 0);

	for (i = 0;i < num_devices;i++) {
		if (devices[i].parent)
			libhal_free_string (devices[i].parent);
	}

	free (devices);
	libhal_free_string_array (device_names);

	if (long_list) {
		printf ("\n"
			"Dumped %d device(s) from the Global Device List.\n"
			"------------------------------------------------\n",
			num_devices);

		printf ("\n");
	}
}

/** 
 *  device_added:
 *  @ctx:		The HAL Context
 *  @udi:                Universal Device Id
 *
 *  Invoked when a device is added to the Global Device List. Simply prints
 *  a message on stdout. 
 */
static void
device_added (LibHalContext *ctx,
	      const char *udi)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list) {
		printf ("*** %s: lshal: device_added, udi='%s'\n", get_time (), udi);
		print_props (udi);
	} else
		printf ("%s: %s added\n", get_time (), short_name (udi));
}

/** 
 *  device_removed:
 *  @ctx:		The HAL Context
 *  @udi:               Universal Device Id
 *
 *  Invoked when a device is removed from the Global Device List. Simply
 *  prints a message on stdout. 
 */
static void
device_removed (LibHalContext *ctx,
		const char *udi)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list)
		printf ("*** %s: lshal: device_removed, udi='%s'\n", get_time (), udi);
	else
		printf ("%s: %s removed\n", get_time (), short_name (udi));
}

/** 
 *  device_new_capability:
 *  @ctx:		The HAL Context
 *  @udi:               Universal Device Id
 *  @capability:        Name of capability
 *
 *  Invoked when device in the Global Device List acquires a new capability.
 *  Prints the name of the capability to stdout. 
 */
static void
device_new_capability (LibHalContext *ctx,
		       const char *udi,
		       const char *capability)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list) {
		printf ("*** %s: lshal: new_capability, udi='%s'\n", get_time (), udi);
		printf ("*** capability: %s\n", capability);
	} else
		printf ("%s: %s capability %s added\n", get_time (), short_name (udi),
			capability);
}

/** 
 *  device_lost_capability:
 *  @ctx:		The HAL Context
 *  @udi:               Universal Device Id
 *  @capability:        Name of capability
 * 
 *  Invoked when device in the Global Device List loses a capability.
 *  Prints the name of the capability to stdout. 
 */
static void
device_lost_capability (LibHalContext *ctx,
			const char *udi,
			const char *capability)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list) {
		printf ("*** %s: lshal: lost_capability, udi='%s'\n", get_time (), udi);
		printf ("*** capability: %s\n", capability);
	} else
		printf ("%s: %s capability %s lost\n", get_time (), short_name (udi),
			capability);
}

/** 
 *  print_property:
 *  @udi:                 Universal Device Id
 *  @key:                 Key of property
 *
 *  Acquires and prints the value of of a property to stdout. 
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
		printf (long_list?"*** new value: '%s'  (string)\n":"'%s'", str);
		libhal_free_string (str);
		break;
	case LIBHAL_PROPERTY_TYPE_INT32:
		{
			dbus_int32_t value = libhal_device_get_property_int (hal_ctx, udi, key, &error);
			printf (long_list?"*** new value: %d (0x%x)  (int)\n":"%d (0x%x)",
				 value, value);
		}
		break;
	case LIBHAL_PROPERTY_TYPE_UINT64:
		{
			dbus_uint64_t value = libhal_device_get_property_uint64 (hal_ctx, udi, key, &error);
			printf (long_list?"*** new value: %llu (0x%llx)  (uint64)\n":"%llu (0x%llx)",
				(long long unsigned int) value, (long long unsigned int) value);
		}
		break;
	case LIBHAL_PROPERTY_TYPE_DOUBLE:
		printf (long_list?"*** new value: %g  (double)\n":"%g",
			libhal_device_get_property_double (hal_ctx, udi, key, &error));
		break;
	case LIBHAL_PROPERTY_TYPE_BOOLEAN:
		printf (long_list?"*** new value: %s  (bool)\n":"%s",
			libhal_device_get_property_bool (hal_ctx, udi, key, &error) ? "true" : "false");
		break;
	case LIBHAL_PROPERTY_TYPE_STRLIST:
	{
		unsigned int i;
		char **strlist;

		if (long_list)
			printf ("*** new value: {");
		else
			printf ("{");

		strlist = libhal_device_get_property_strlist (hal_ctx, udi, key, &error);
                /* may be NULL because property may have been removed */
                if (strlist != NULL) {
                        for (i = 0; strlist[i] != NULL; i++) {
                                printf ("'%s'", strlist[i]);
                                if (strlist[i+1] != NULL)
                                        printf (", ");
                        }
                        if (long_list)
                                printf ("}  (string list)\n");
                        else
                                printf ("}");
                        libhal_free_string_array (strlist);
                }
		break;
	}

	default:
		fprintf (stderr, "Unknown type %d='%c'\n", type, type);
		break;
	}

	if (dbus_error_is_set (&error))
		dbus_error_free (&error);
}

/** 
 *  property_modified:
 *  @ctx:		The HAL Context
 *  @udi:               Univerisal Device Id
 *  @key:               Key of property
 *  @is_removed:        if the property was removed
 *  @is_added:          if the property was added
 * 
 *  Invoked when a property of a device in the Global Device List is
 *  changed, and we have we have subscribed to changes for that device. 
 */
static void
property_modified (LibHalContext *ctx,
		   const char *udi,
		   const char *key,
		   dbus_bool_t is_removed,
		   dbus_bool_t is_added)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list) {
		printf ("*** %s: lshal: property_modified, udi=%s, key=%s\n",
			get_time (), udi, key);
		printf ("           is_removed=%s, is_added=%s\n",
			is_removed ? "true" : "false",
			is_added ? "true" : "false");
		if (!is_removed)
			print_property (udi, key);
		printf ("\n");
	} else {
		printf ("%s: %s property %s ", get_time (), short_name (udi), key);
		if (is_removed)
			printf ("removed");
		else {
			printf ("= ");
			print_property (udi, key);

			if (is_added)
				printf (" (new)");
		}
		printf ("\n");
	}
}


/** 
 *  device_condition:
 *  @ctx:                 The HAL Context
 *  @udi:                 Univerisal Device Id
 *  @condition_name:      Name of condition
 *  @message:             D-BUS message with parameters
 *
 *  Invoked when a property of a device in the Global Device List is
 *  changed, and we have we have subscribed to changes for that device. 
 */
static void
device_condition (LibHalContext *ctx,
		  const char *udi,
		  const char *condition_name,
		  const char *condition_details)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list) {
		printf ("*** %s: lshal: device_condition, udi=%s\n", get_time (), udi);
		printf ("           condition_name=%s\n", condition_name);
		printf ("           condition_details=%s\n", condition_details);
		printf ("\n");
	} else {
		printf ("%s: %s condition %s = %s\n", get_time (), short_name (udi),
			condition_name, condition_details);
	}
}

static void
do_interface_lock (LibHalContext *ctx,
                   dbus_bool_t acquired,
                   dbus_bool_t global,
                   const char *udi,
                   const char *interface_name,
                   const char *lock_owner,
                   int num_locks)
{
	if (show_device && strcmp(show_device, udi))
		return;

	if (long_list) {
                if (global)
                        printf ("*** %s: lshal: global_interface_lock_%s\n", get_time (), acquired ? "acquired" : "released");
                else
                        printf ("*** %s: lshal: interface_lock_%s, udi=%s\n", get_time (), acquired ? "acquired" : "released", udi);
		printf ("           interface_name=%s\n", interface_name);
		printf ("           lock_owner=%s\n", lock_owner);
		printf ("           num_locks=%d\n", num_locks);
		printf ("\n");
	} else {
                if (global)
                        printf ("%s: global_interface_lock_%s %s by %s (%d lockers)\n", get_time (),
                                acquired ? "acquired" : "released",
                                interface_name, lock_owner, num_locks);
                else
                        printf ("%s: %s interface_lock_%s %s by %s (%d lockers)\n", get_time (), short_name (udi),
                                acquired ? "acquired" : "released",
                                interface_name, lock_owner, num_locks);
	}
}

static void
global_interface_lock_acquired (LibHalContext *ctx,
                                const char *interface_name,
                                const char *lock_owner,
                                int num_locks)
{
        do_interface_lock (ctx, TRUE, TRUE, NULL, interface_name, lock_owner, num_locks);
}

static void
global_interface_lock_released (LibHalContext *ctx,
                                const char *interface_name,
                                const char *lock_owner,
                                int num_locks)
{
        do_interface_lock (ctx, FALSE, TRUE, NULL, interface_name, lock_owner, num_locks);
}

static void
interface_lock_acquired (LibHalContext *ctx,
                         const char *udi,
                         const char *interface_name,
                         const char *lock_owner,
                         int num_locks)
{
        do_interface_lock (ctx, TRUE, FALSE, udi, interface_name, lock_owner, num_locks);
}

static void
interface_lock_released (LibHalContext *ctx,
                         const char *udi,
                         const char *interface_name,
                         const char *lock_owner,
                         int num_locks)
{
        do_interface_lock (ctx, FALSE, FALSE, udi, interface_name, lock_owner, num_locks);
}


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
	fprintf (stderr, "lshal version " PACKAGE_VERSION "\n");

	fprintf (stderr, "\n" "usage : %s [options]\n", argv[0]);
	fprintf (stderr,
		 "\n"
		 "Options:\n"
		 "    -m, --monitor        Monitor device list\n"
		 "    -s, --short          short output (print only nonstatic part of udi)\n"
		 "    -l, --long           Long output\n"
		 "    -t, --tree           Tree view\n"
		 "    -u, --show <udi>     Show only the specified device\n"
		 "\n"
		 "    -h, --help           Show this information and exit\n"
		 "    -V, --version        Print version number\n"
		 "\n"
		 "Without any given options lshal will start with option --long."
		 "\n"
		 "Shows all devices and their properties. If the --monitor option is given\n"
		 "then the device list and all devices are monitored for changes.\n"
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
	DBusError error;
	dbus_bool_t do_monitor = FALSE;
	GMainLoop *loop;
	DBusConnection *conn;

	if (argc == 1) {
		/* This is the default case lshal without any options */
		long_list = TRUE;
	}
	else {
		static const struct option long_options[] = {
			{"monitor", no_argument, NULL, 'm'},
			{"long", no_argument, NULL, 'l'},
			{"short", no_argument, NULL, 's'},
			{"tree", no_argument, NULL, 't'},
			{"show", required_argument, NULL, 'u'},
			{"help", no_argument, NULL, 'h'},
			{"usage", no_argument, NULL, 'U'},
			{"version", no_argument, NULL, 'V'},
			{NULL, 0, NULL, 0}
		};

		while (1) {
			int c;
			
			c = getopt_long (argc, argv, "mlstu:hUV", long_options, NULL);

			if (c == -1) {
				/* this should happen e.g. if 'lshal -' and this is incorrect/incomplete option */
				if (!do_monitor && !long_list && !short_list && !tree_view && !show_device) {
					usage (argc, argv);
					return 1;
				}
				
				break;
			}

			switch (c) {
			case 'm': 
				do_monitor = TRUE;
				break;

			case 'l':
				long_list = TRUE;
				break;

			case 's':
				short_list = TRUE;
				long_list = FALSE;
				break;
			
			case 't':
				tree_view = TRUE;
				break;
				
			case 'u':
				if (strchr(optarg, '/') != NULL)
					show_device = strdup(optarg);
				else {
					show_device = malloc(strlen(UDI_BASE) + strlen(optarg) + 1);
					memcpy(show_device, UDI_BASE, strlen(UDI_BASE));
					memcpy(show_device + strlen(UDI_BASE), optarg, strlen(optarg) + 1);
				}
				
				break;
			
			case 'h':
			case 'U':
				usage (argc, argv);
				return 0;
			
			case 'V':
				printf ("lshal version " PACKAGE_VERSION "\n");
				return 0;

			default:
				usage (argc, argv);
				return 1;
			}
		}
	}
	
	if (do_monitor)
		loop = g_main_loop_new (NULL, FALSE);
	else
		loop = NULL;

	dbus_error_init (&error);
	conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		fprintf (stderr, "error: dbus_bus_get: %s: %s\n",
			 error.name, error.message);
		LIBHAL_FREE_DBUS_ERROR (&error);
		return 1;
	}

	if (do_monitor)
		dbus_connection_setup_with_g_main (conn, NULL);

	if ((hal_ctx = libhal_ctx_new ()) == NULL) {
		fprintf (stderr, "error: libhal_ctx_new\n");
		return 1;
	}
	if (!libhal_ctx_set_dbus_connection (hal_ctx, conn)) {
		fprintf (stderr, "error: libhal_ctx_set_dbus_connection: %s: %s\n",
			 error.name, error.message);
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

	libhal_ctx_set_device_added (hal_ctx, device_added);
	libhal_ctx_set_device_removed (hal_ctx, device_removed);
	libhal_ctx_set_device_new_capability (hal_ctx, device_new_capability);
	libhal_ctx_set_device_lost_capability (hal_ctx, device_lost_capability);
	libhal_ctx_set_device_property_modified (hal_ctx, property_modified);
	libhal_ctx_set_device_condition (hal_ctx, device_condition);
	libhal_ctx_set_global_interface_lock_acquired (hal_ctx, global_interface_lock_acquired);
	libhal_ctx_set_global_interface_lock_released (hal_ctx, global_interface_lock_released);
	libhal_ctx_set_interface_lock_acquired (hal_ctx, interface_lock_acquired);
	libhal_ctx_set_interface_lock_released (hal_ctx, interface_lock_released);

	if (show_device) {
		long_list = TRUE;
		dump_device (show_device);
	}
	else if (!do_monitor)
		dump_devices ();

	/* run the main loop only if we should monitor */
	if (do_monitor && loop != NULL) {
		if( long_list || short_list || tree_view )
			dump_devices ();
		
		if ( libhal_device_property_watch_all (hal_ctx, &error) == FALSE) {
			fprintf (stderr, "error: monitoring devicelist - libhal_device_property_watch_all: %s: %s\n",
				 error.name, error.message);
			LIBHAL_FREE_DBUS_ERROR (&error);
			return 1;
		}
		printf ("\nStart monitoring devicelist:\n"
			"-------------------------------------------------\n");
		g_main_loop_run (loop);
	}

	if ( libhal_ctx_shutdown (hal_ctx, &error) == FALSE)
		LIBHAL_FREE_DBUS_ERROR (&error);
	libhal_ctx_free (hal_ctx);

	dbus_connection_unref (conn);

	if (show_device)
		free(show_device);

	return 0;
}

