/***************************************************************************
 * CVSID: $Id$
 *
 * hald.c : main startup for HAL daemon
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
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "callout.h"
#include "logger.h"
#include "hald.h"
#include "device_store.h"
#include "device_info.h"
#include "osspec.h"
#include "hald_dbus.h"

/**
 * @defgroup HalDaemon HAL daemon
 * @brief The HAL daemon manages persistent device objects available through
 *        a D-BUS network API
 */

static HalDeviceStore *global_device_list = NULL;

static HalDeviceStore *temporary_device_list = NULL;

static void
gdl_store_changed (HalDeviceStore *store, HalDevice *device,
		   gboolean is_added, gpointer user_data)
{
	if (is_added)
		HAL_INFO(("Added device to GDL!"));
	else
		HAL_INFO(("Removed device from GDL!"));

	hal_device_print (device);

	if (is_added) {
		manager_send_signal_device_added (device);
		hal_callout_device (device, TRUE);
	} else {
		manager_send_signal_device_removed (device);
		hal_callout_device (device, FALSE);
	}
}

static void
gdl_property_changed (HalDeviceStore *store, HalDevice *device,
		      const char *key, gboolean added, gboolean removed,
		      gpointer user_data)
{
	device_send_signal_property_modified (device, key, removed, added);
}

static void
gdl_capability_added (HalDeviceStore *store, HalDevice *device,
		      const char *capability, gpointer user_data)
{
	manager_send_signal_new_capability (device, capability);
	hal_callout_capability (device, capability, TRUE);
}

HalDeviceStore *
hald_get_gdl (void)
{
	if (global_device_list == NULL) {
		global_device_list = hal_device_store_new ();
		
		g_signal_connect (global_device_list,
				  "store_changed",
				  G_CALLBACK (gdl_store_changed), NULL);
		g_signal_connect (global_device_list,
				  "device_property_changed",
				  G_CALLBACK (gdl_property_changed), NULL);
		g_signal_connect (global_device_list,
				  "device_capability_added",
				  G_CALLBACK (gdl_capability_added), NULL);
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

/**
 * @defgroup MainDaemon Basic functions
 * @ingroup HalDaemon
 * @brief Basic functions in the HAL daemon
 * @{
 */

/** Print out program usage.
 *
 */
static void
usage ()
{
	fprintf (stderr, "\n" "usage : hald [--daemon=yes|no] [--help]\n");
	fprintf (stderr,
		 "\n"
		 "        --daemon=yes|no    Become a daemon\n"
		 "        --help             Show this information and exit\n"
		 "\n"
		 "The HAL daemon detects devices present in the system and provides the\n"
		 "org.freedesktop.Hal service through D-BUS. The commandline options given\n"
		 "overrides the configuration given in "
		 PACKAGE_SYSCONF_DIR "/hald.conf\n" "\n"
		 "For more information visit http://freedesktop.org/Software/hal\n"
		 "\n");
}


/** If #TRUE, we will daemonize */
static dbus_bool_t opt_become_daemon = TRUE;

/** Run as specified username if not #NULL */
static char *opt_run_as = NULL;

/** Entry point for HAL daemon
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @return                     Exit code
 */
int
main (int argc, char *argv[])
{
	DBusConnection *dbus_connection;
	GMainLoop *loop;

	/* We require root to sniff mii registers */
	/*opt_run_as = HAL_USER; */

	while (1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
			{"daemon", 1, NULL, 0},
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
				usage ();
				return 0;
			} else if (strcmp (opt, "daemon") == 0) {
				if (strcmp ("yes", optarg) == 0) {
					opt_become_daemon = TRUE;
				} else if (strcmp ("no", optarg) == 0) {
					opt_become_daemon = FALSE;
				} else {
					usage ();
					return 1;
				}
			}
			break;

		default:
			usage ();
			return 1;
			break;
		}
	}

	logger_init ();
	HAL_INFO (("HAL daemon version " PACKAGE_VERSION " starting up"));

	HAL_DEBUG (("opt_become_daemon = %d", opt_become_daemon));

	if (opt_become_daemon) {
		int child_pid;
		int dev_null_fd;

		if (chdir ("/") < 0) {
			HAL_ERROR (("Could not chdir to /, errno=%d",
				    errno));
			return 1;
		}

		child_pid = fork ();
		switch (child_pid) {
		case -1:
			HAL_ERROR (("Cannot fork(), errno=%d", errno));
			break;

		case 0:
			/* child */

			dev_null_fd = open ("/dev/null", O_RDWR);
			/* ignore if we can't open /dev/null */
			if (dev_null_fd > 0) {
				/* attach /dev/null to stdout, stdin, stderr */
				dup2 (dev_null_fd, 0);
				dup2 (dev_null_fd, 1);
				dup2 (dev_null_fd, 2);
			}

			umask (022);

			/** @todo FIXME change logger to direct to syslog */

			break;

		default:
			/* parent */
			exit (0);
			break;
		}

		/* Create session */
		setsid ();
	}

	if (opt_run_as != NULL) {
		uid_t uid;
		gid_t gid;
		struct passwd *pw;


		if ((pw = getpwnam (opt_run_as)) == NULL) {
			HAL_ERROR (("Could not lookup user %s, errno=%d",
				    opt_run_as, errno));
			exit (1);
		}

		uid = pw->pw_uid;
		gid = pw->pw_gid;

		if (setgid (gid) < 0) {
			HAL_ERROR (("Failed to set GID to %d, errno=%d",
				    gid, errno));
			exit (1);
		}

		if (setuid (uid) < 0) {
			HAL_ERROR (("Failed to set UID to %d, errno=%d",
				    uid, errno));
			exit (1);
		}

	}

	g_type_init ();

	/* set up the dbus services */
	dbus_connection = hald_dbus_init ();

	loop = g_main_loop_new (NULL, FALSE);

	/* initialize operating system specific parts */
	osspec_init (dbus_connection);
	/* and detect devices */
	osspec_probe ();

	/* run the main loop and serve clients */
	g_main_loop_run (loop);

	return 0;
}

/** @} */
