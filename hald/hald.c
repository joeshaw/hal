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
#include <sys/prctl.h>
#include <grp.h>
#include <syslog.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

/*#include "master_slave.h"*/

#include "logger.h"
#include "hald.h"
#include "device_store.h"
#include "device_info.h"
#include "osspec.h"
#include "hald_dbus.h"
#include "util.h"

static void delete_pid(void)
{
	unlink(HALD_PID_FILE);
}

/**
 * @defgroup HalDaemon HAL daemon
 * @brief The HAL daemon manages persistent device objects available through
 *        a D-BUS network API
 */

static HalDeviceStore *global_device_list = NULL;

static HalDeviceStore *temporary_device_list = NULL;

static GSList *running_addons = NULL;

static void 
addon_terminated (HalDevice *d, gboolean timed_out, gint return_code, 
		  gpointer data1, gpointer data2, HalHelperData *helper_data)
{
	running_addons = g_slist_remove (running_addons, helper_data);
}

static void
gdl_store_changed (HalDeviceStore *store, HalDevice *device,
		   gboolean is_added, gpointer user_data)
{
	if (is_added) {
		GSList *addons;

		HAL_INFO (("Added device to GDL; udi=%s", hal_device_get_udi(device)));

		if ((addons = hal_device_property_get_strlist (device, "info.addons")) != NULL) {
			GSList *i;
			
			for (i = addons; i != NULL; i = g_slist_next (i)) {
				const gchar *command_line;
				HalHelperData *helper_data;
				gchar *extra_env[2] = {"HALD_ACTION=addon", NULL};

				command_line = (const gchar *) i->data;
				helper_data = hal_util_helper_invoke (command_line, extra_env, device,
								      NULL, NULL,
								      addon_terminated, 0 /* no timeout */);

				if (helper_data != NULL) {
					HAL_INFO (("Invoked addon %s with pid %d for udi %s", 
						   command_line, helper_data->pid, helper_data->d->udi));
					running_addons = g_slist_prepend (running_addons, helper_data);
				}
			}
		}
	} else {
		GSList *i;

		HAL_INFO (("Removed device from GDL; udi=%s", hal_device_get_udi(device)));

	start_from_beginning:

		/* may have several addons running */
		for (i = running_addons; i != NULL; i = g_slist_next (i)) {
			HalHelperData *helper_data;

			helper_data = (HalHelperData *) (i->data);
			if (helper_data->d == device) {
				HAL_INFO (("Terminating addon with pid %d for udi %s", 
					   helper_data->pid, helper_data->d->udi));
				/* will force a callback - the callback removes us from the list */
				hal_util_terminate_helper (helper_data);
				/* TODO: is it safe to remove an elem from a GSList and keep iterating? 
				 *       Better play it safe for now.
				 */
				goto start_from_beginning;
			}
		}
		
	}

	/*hal_device_print (device);*/

	if (is_added)
		manager_send_signal_device_added (device);
	else
		manager_send_signal_device_removed (device);
}

static void
gdl_property_changed (HalDeviceStore *store, HalDevice *device,
		      const char *key, gboolean added, gboolean removed,
		      gpointer user_data)
{
	device_send_signal_property_modified (device, key, removed, added);

	/* only execute the callouts if the property _changed_ */
	if (added == FALSE && removed == FALSE)
		/*hal_callout_property (device, key)*/;
}

static void
gdl_capability_added (HalDeviceStore *store, HalDevice *device,
		      const char *capability, gpointer user_data)
{
	manager_send_signal_new_capability (device, capability);
	/*hal_callout_capability (device, capability, TRUE)*/;
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
	fprintf (stderr, "\n" "usage : hald [--daemon=yes|no] [--verbose=yes|no] [--help]\n");
	fprintf (stderr,
		 "\n"
		 "        --daemon=yes|no      Become a daemon\n"
		 "        --verbose=yes|no     Print out debug (overrides HALD_VERBOSE)\n"
 		 "        --retain-privileges  Run as normal user instead of root (calling of\n"
 		 "                             external scripts to modify fstab etc. will not work\n" 
		 "                             run as root)\n"
		 "        --help               Show this information and exit\n"
		 "\n"
		 "The HAL daemon detects devices present in the system and provides the\n"
		 "org.freedesktop.Hal service through the system-wide message bus provided\n"
		 "by D-BUS.\n"
		 "\n"
		 "For more information visit http://freedesktop.org/Software/hal\n"
		 "\n");
}

/** If #TRUE, we will daemonize */
static dbus_bool_t opt_become_daemon = TRUE;

/** If #TRUE, we will spew out debug */
dbus_bool_t hald_is_verbose = FALSE;

static int sigterm_unix_signal_pipe_fds[2];
static GIOChannel *sigterm_iochn;

static void 
handle_sigterm (int value)
{
	static char marker[1] = {'S'};

	/* write a 'S' character to the other end to tell about
	 * the signal. Note that 'the other end' is a GIOChannel thingy
	 * that is only called from the mainloop - thus this is how we
	 * defer this since UNIX signal handlers are evil
	 *
	 * Oh, and write(2) is indeed reentrant */
	write (sigterm_unix_signal_pipe_fds[1], marker, 1);
}

static gboolean
sigterm_iochn_data (GIOChannel *source, 
		    GIOCondition condition, 
		    gpointer user_data)
{
	GError *err = NULL;
	gchar data[1];
	gsize bytes_read;
	unsigned int num_helpers;

	/* Empty the pipe */
	if (G_IO_STATUS_NORMAL != 
	    g_io_channel_read_chars (source, data, 1, &bytes_read, &err)) {
		HAL_ERROR (("Error emptying sigterm pipe: %s",
				   err->message));
		g_error_free (err);
		goto out;
	}

	HAL_INFO (("Caught SIGTERM, initiating shutdown"));
	num_helpers = hal_util_kill_all_helpers ();
	HAL_INFO (("Killed %d helpers; exiting"));
	exit (0);

out:
	return TRUE;
}


/** This is set to #TRUE if we are probing and #FALSE otherwise */
dbus_bool_t hald_is_initialising;

static int startup_daemonize_pipe[2];

/** Drop all but necessary privileges from hald when it runs as root.  Set the
 *  running user id to HAL_USER and group to HAL_GROUP
 */
static void
drop_privileges ()
{
    struct passwd *pw = NULL;
    struct group *gr = NULL;

    /* determine user id */
    pw = getpwnam (HAL_USER);
    if (!pw)  {
	HAL_ERROR (("drop_privileges: user " HAL_USER " does not exist"));
	exit (-1);
    }

    /* determine primary group id */
    gr = getgrnam (HAL_GROUP);
    if(!gr) {
	HAL_ERROR (("drop_privileges: group " HAL_GROUP " does not exist"));
	exit (-1);
    }

    if( initgroups (HAL_USER, gr->gr_gid)) {
	HAL_ERROR (("drop_privileges: could not initialize groups"));
	exit (-1);
    }

    if( setgid (gr->gr_gid) ) {
	HAL_ERROR (("drop_privileges: could not set group id"));
	exit (-1);
    }

    if( setuid (pw->pw_uid)) {
	HAL_ERROR (("drop_privileges: could not set user id"));
	exit (-1);
    }
}


/** Entry point for HAL daemon
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @return                     Exit code
 */
int
main (int argc, char *argv[])
{
	GMainLoop *loop;
	guint sigterm_iochn_listener_source_id;
	gboolean retain_privs;
	char *path;
	char newpath[512];

	retain_privs = FALSE;
  
	openlog ("hald", LOG_PID, LOG_DAEMON);

	g_type_init ();

	logger_init ();
	if (getenv ("HALD_VERBOSE"))
		hald_is_verbose = TRUE;
	else
		hald_is_verbose = FALSE;

	/* our helpers are installed into libexec, so adjust out $PATH to include this */
	path = getenv ("PATH");
	g_strlcpy (newpath, PACKAGE_LIBEXEC_DIR, sizeof (newpath));
	if (path != NULL) {
		g_strlcat (newpath, ":", sizeof (newpath));
		g_strlcat (newpath, path, sizeof (newpath));
	}
	setenv ("PATH", newpath, TRUE);

	while (1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
			{"daemon", 1, NULL, 0},
			{"verbose", 1, NULL, 0},
			{"help", 0, NULL, 0},
			{"retain-privileges", 0, NULL, 0},
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
			} else if (strcmp (opt, "verbose") == 0) {
				if (strcmp ("yes", optarg) == 0) {
					hald_is_verbose = TRUE;
				} else if (strcmp ("no", optarg) == 0) {
					hald_is_verbose = FALSE;
				} else {
					usage ();
					return 1;
				}
			} else if (strcmp (opt, "retain-privileges") == 0) {
				retain_privs = TRUE;
			}
			break;

		default:
			usage ();
			return 1;
			break;
		}
	}

	if (hald_is_verbose)
		logger_enable ();
	else
		logger_disable ();

	/* will fork into two; only the child will return here if we are successful */
	/*master_slave_setup ();*/

	if (!retain_privs)
		drop_privileges();

	loop = g_main_loop_new (NULL, FALSE);

	HAL_INFO ((PACKAGE_STRING));
	if (opt_become_daemon)
		HAL_INFO (("Will daemonize"));
	else
		HAL_INFO (("Will not daemonize"));

	if (opt_become_daemon) {
		int child_pid;
		int dev_null_fd;
		int pf;
		char pid[9];
		
		HAL_INFO (("Becoming a daemon"));

		if (pipe (startup_daemonize_pipe) != 0) {
			fprintf (stderr, "Could not setup pipe: %s\n", strerror(errno));
			exit (1);
		}


		if (chdir ("/") < 0) {
			fprintf (stderr, "Could not chdir to /: %s\n", strerror(errno));
			exit (1);
		}

		child_pid = fork ();
		switch (child_pid) {
		case -1:
			fprintf (stderr, "Cannot fork(): %s\n", strerror(errno));
			break;

		case 0:
			/* child */

			dev_null_fd = open ("/dev/null", O_RDWR);
			/* ignore if we can't open /dev/null */
			if (dev_null_fd >= 0) {
				/* attach /dev/null to stdout, stdin, stderr */
				dup2 (dev_null_fd, 0);
				dup2 (dev_null_fd, 1);
				dup2 (dev_null_fd, 2);
				close (dev_null_fd);
			}

			umask (022);
			break;

		default:
		        {
				/* parent, block until child writes */
				/* char buf[1];
				read (startup_daemonize_pipe[0], &buf, sizeof (buf));*/
				exit (0);
				break;
			}
		}

		/* Create session */
		setsid ();

		/* remove old pid file */
		unlink (HALD_PID_FILE);

		/* Make a new one */
		if ((pf= open (HALD_PID_FILE, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL, 0644)) > 0) {
			snprintf (pid, sizeof(pid), "%lu\n", (long unsigned) getpid ());
			write (pf, pid, strlen(pid));
			close (pf);
			atexit (delete_pid);
		}
	}


	/* we need to do stuff when we are expected to terminate, thus
	 * this involves looking for SIGTERM; UNIX signal handlers are
	 * evil though, so set up a pipe to transmit the signal.
	 */

	/* create pipe */
	if (pipe (sigterm_unix_signal_pipe_fds) != 0) {
		DIE (("Could not setup pipe, errno=%d", errno));
	}
	
	/* setup glib handler - 0 is for reading, 1 is for writing */
	sigterm_iochn = g_io_channel_unix_new (sigterm_unix_signal_pipe_fds[0]);
	if (sigterm_iochn == NULL)
		DIE (("Could not create GIOChannel"));
	
	/* get callback when there is data to read */
	sigterm_iochn_listener_source_id = g_io_add_watch (
		sigterm_iochn, G_IO_IN, sigterm_iochn_data, NULL);
	
	/* Finally, setup unix signal handler for TERM */
	signal (SIGTERM, handle_sigterm);

	/* set up the dbus services */
	if (!hald_dbus_init ())
		return 1;

	/* initialize operating system specific parts */
	osspec_init ();

	hald_is_initialising = TRUE;

	/* detect devices */
	osspec_probe ();

	/* run the main loop and serve clients */
	g_main_loop_run (loop);

	return 0;
}

gboolean
resolve_udiprop_path (const char *path, const char *source_udi,
		      char *udi_result, size_t udi_result_size, 
		      char *prop_result, size_t prop_result_size);

void 
osspec_probe_done (void)
{
	char buf[1] = {0};

	HAL_INFO (("Device probing completed"));

/*
	{
		char udi[256];
		char prop[256];

		resolve_udiprop_path ("info.udi", 
				      "/org/freedesktop/Hal/devices/computer", 
				      udi, sizeof (udi), 
				      prop, sizeof (prop));
		HAL_INFO (("----------------------------------------"));
		resolve_udiprop_path ("/org/freedesktop/Hal/devices/computer:kernel.name", 
				      "/org/freedesktop/Hal/devices/pci_8086_3341", 
				      udi, sizeof (udi), 
				      prop, sizeof (prop));
		HAL_INFO (("----------------------------------------"));
		resolve_udiprop_path ("@block.storage_device:@storage.physical_device:ide.channel", 
				      "/org/freedesktop/Hal/devices/block_3_3", 
				      udi, sizeof (udi), 
				      prop, sizeof (prop));
	}
*/

	/* tell parent to exit */
	write (startup_daemonize_pipe[1], buf, sizeof (buf));
	close (startup_daemonize_pipe[0]);
	close (startup_daemonize_pipe[1]);

	hald_is_initialising = FALSE;
}


/** @} */
