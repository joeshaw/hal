/***************************************************************************
 * CVSID: $Id$
 *
 * callout.c : Call out to helper programs when devices are added/removed.
 *
 * Copyright (C) 2004 Novell, Inc.
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

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "callout.h"
#include "logger.h"

#define DEVICE_CALLOUT_DIR     PACKAGE_SYSCONF_DIR "/hal/device.d"
#define CAPABILITY_CALLOUT_DIR PACKAGE_SYSCONF_DIR "/hal/capability.d"
#define PROPERTY_CALLOUT_DIR   PACKAGE_SYSCONF_DIR "/hal/property.d"

enum {
	CALLOUT_ADD,	/* device or capability is being added */
	CALLOUT_REMOVE,	/* device or capability is being removed */
	CALLOUT_MODIFY,	/* property is being modified */
};

typedef struct {
	const char *working_dir;
	char *filename;
	int action;
	HalDevice *device;
	char **envp;
	int envp_index;
	int pid;
	gboolean last_of_device;
} Callout;

static void process_callouts (void);

/* Callouts that still needing to be processed
 *
 * Key: HalDevice  Value: pointer to GSList of Callouts */
static GHashTable *pending_callouts = NULL;

/* List of Callouts currently being processed */
static GSList *active_callouts = NULL;

static gboolean processing_callouts = FALSE;

static void
add_pending_callout (HalDevice *device, Callout *callout)
{
	GSList **clist = NULL;

	if (pending_callouts == NULL)
		pending_callouts = g_hash_table_new (NULL, NULL);
	else
		clist = g_hash_table_lookup (pending_callouts, device);

	if (clist == NULL) {
		clist = g_new0 (GSList *, 1);
		g_hash_table_insert (pending_callouts, device, clist);
	}

	*clist = g_slist_append (*clist, callout);
}

static void
get_device (gpointer key, gpointer value, gpointer user_data)
{
	HalDevice **device = user_data;

	if (*device == NULL)
		*device = (HalDevice *) key;
}

static Callout *
pop_pending_callout (gboolean *last_of_device)
{
	GSList **clist;
	HalDevice *device = NULL;
	Callout *callout;

	if (pending_callouts == NULL)
		return NULL;

	/* Hmm, not sure of a better way to do this... */
	g_hash_table_foreach (pending_callouts, get_device, &device);

	clist = g_hash_table_lookup (pending_callouts, device);

	if (clist == NULL)
		return NULL;

	callout = (Callout *) (*clist)->data;
	*clist = g_slist_remove (*clist, callout);

	if (*clist == NULL) {
		g_hash_table_remove (pending_callouts, device);
		g_free (clist);
		*last_of_device = TRUE;
	} else
		*last_of_device = FALSE;

	return callout;
}

static gboolean
add_property_to_env (HalDevice *device, HalProperty *property, 
		     gpointer user_data)
{
	Callout *callout = user_data;
	char *prop_upper, *value;
	char *c;

	prop_upper = g_ascii_strup (hal_property_get_key (property), -1);

	/* periods aren't valid in the environment, so replace them with
	 * underscores. */
	for (c = prop_upper; *c; c++) {
		if (*c == '.')
			*c = '_';
	}

	value = hal_property_to_string (property);

	callout->envp[callout->envp_index] =
		g_strdup_printf ("HAL_PROP_%s=%s",
				 prop_upper,
				 value);

	g_free (value);
	g_free (prop_upper);

	callout->envp_index++;

	return TRUE;
}

static gboolean have_installed_sigchild_handler = FALSE;
static int unix_signal_pipe_fds[2];
static GIOChannel *iochn;

static void 
handle_sigchld (int value)
{
	static char marker[1] = {'D'};

	/* write a 'D' character to the other end to tell that a child has
	 * terminated. Note that 'the other end' is a GIOChannel thingy
	 * that is only called from the mainloop - thus this is how we
	 * defer this since UNIX signal handlers are evil
	 *
	 * Oh, and write(2) is indeed reentrant */
	write (unix_signal_pipe_fds[1], marker, 1);
}

static gboolean
iochn_data (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
	gsize bytes_read;
	gchar data[1];
	GError *err = NULL;
	pid_t child_pid;
	Callout *callout;
	GSList *it;


	/* Empty the pipe; one character per dead child */
	if (G_IO_STATUS_NORMAL != 
	    g_io_channel_read_chars (source, data, 1, &bytes_read, &err)) {
		HAL_ERROR (("Error emptying callout notify pipe: %s",
				   err->message));
		g_error_free (err);
		goto out;
	}

	/* signals are not queued, so loop until no zombies are left */
	while (TRUE) {
		/* wait for any child - this is sane because the reason we got
		 * invoked is the fact that a child has exited */
		child_pid = waitpid (-1, NULL, WNOHANG);

		if (child_pid == 0) {
			/* no more childs */
			goto out;
		} else if (child_pid == -1) {
			/* this can happen indeed since we loop */
			goto out;
		}
	
		/* Now find the corresponding Callout object */
		callout = NULL;
		for (it=g_slist_nth(active_callouts, 0); 
		     it != NULL; 
		     it = g_slist_next(it)) {
			Callout *callout_it = (Callout *) it->data;
			if (callout_it->pid == child_pid) {
				callout = callout_it;
				break;
			}
		}

		if (callout == NULL) {
			/* this should never happen */
			HAL_ERROR (("Cannot find callout for terminated "
				    "child with pid %d", child_pid));
			goto out;
		}


		/* remove element from active_callouts list */
		active_callouts = g_slist_delete_link (active_callouts,
							       it);

		if (callout->last_of_device)
			hal_device_callouts_finished (callout->device);
		
		g_free (callout->filename);
		g_strfreev (callout->envp);
		g_object_unref (callout->device);
		g_free (callout);
		
		process_callouts ();
	}
	
out:
	return TRUE;
}

static void
process_callouts (void)
{
	Callout *callout;
	gboolean last_of_device;
	char *argv[3];
	GError *err = NULL;
	int num_props;

	if (!have_installed_sigchild_handler) {
		guint iochn_listener_source_id;

		/* create pipe */
		if (pipe (unix_signal_pipe_fds) != 0) {
			DIE (("Could not setup pipe, errno=%d", errno));
		}

		/* setup glib handler - 0 is for reading, 1 is for writing */
		iochn = g_io_channel_unix_new (unix_signal_pipe_fds[0]);
		if (iochn == NULL)
			DIE (("Could not create GIOChannel"));

		/* get callback when there is data to read */
		iochn_listener_source_id = g_io_add_watch (
			iochn, G_IO_IN, iochn_data, NULL);

		/* setup unix signal handler */
		signal(SIGCHLD, handle_sigchld);
		have_installed_sigchild_handler = TRUE;
	}

	if (pending_callouts == NULL ||
	    g_hash_table_size (pending_callouts) == 0) {
		processing_callouts = FALSE;
		return;
	}

	processing_callouts = TRUE;

	callout = pop_pending_callout (&last_of_device);
	callout->last_of_device = last_of_device;

	argv[0] = callout->filename;

	switch (callout->action) {
	case CALLOUT_ADD:
		argv[1] = "add";
		break;
	case CALLOUT_REMOVE:
		argv[1] = "remove";
		break;
	case CALLOUT_MODIFY:
		argv[1] = "modify";
		break;
	}

	argv[2] = NULL;

	num_props = hal_device_num_properties (callout->device);

	/*
	 * All the properties, plus any special env vars set up for this
	 * type of callout, plus one for a trailing NULL.
	 */
	callout->envp = g_renew (char *, callout->envp,
				 num_props + callout->envp_index + 1);

	hal_device_property_foreach (callout->device, add_property_to_env,
				     callout);

	/*
	 * envp_index is incremented in the foreach, so afterward we're
	 * pointing at the end of the array.
	 */
	callout->envp[callout->envp_index] = NULL;

	active_callouts = g_slist_append (active_callouts, callout);

	if (!g_spawn_async (callout->working_dir, argv, callout->envp,
			    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
			    &callout->pid, &err)) {
		HAL_WARNING (("Couldn't invoke %s: %s", argv[0],
			      err->message));
		g_error_free (err);
	}
}

static gboolean
process_callouts_idle (gpointer user_data)
{
	process_callouts ();
	return FALSE;
}

void
hal_callout_device (HalDevice *device, gboolean added)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;
	gboolean any_callouts = FALSE;

	/* Directory doesn't exist.  This isn't an error, just exit
	 * quietly. */
	if (!g_file_test (DEVICE_CALLOUT_DIR, G_FILE_TEST_EXISTS))
		goto finish;

	dir = g_dir_open (DEVICE_CALLOUT_DIR, 0, &err);

	if (dir == NULL) {
		HAL_WARNING (("Unable to open device callout directory: %s",
			      err->message));
		g_error_free (err);
		goto finish;
	}

	while ((filename = g_dir_read_name (dir)) != NULL) {
		char *full_filename;
		Callout *callout;
		int num_props;

		full_filename = g_build_filename (DEVICE_CALLOUT_DIR,
						  filename, NULL);

		if (!g_file_test (full_filename, G_FILE_TEST_IS_EXECUTABLE)) {
			g_free (full_filename);
			continue;
		}

		g_free (full_filename);

		callout = g_new0 (Callout, 1);

		callout->working_dir = DEVICE_CALLOUT_DIR;
		callout->filename = g_strdup (filename);
		callout->action = added ? CALLOUT_ADD : CALLOUT_REMOVE;
		callout->device = g_object_ref (device);

		num_props = hal_device_num_properties (device);

		callout->envp_index = 1;
		callout->envp = g_new (char *, callout->envp_index);

		callout->envp[0] = g_strdup_printf ("UDI=%s",
						    hal_device_get_udi (device));
		add_pending_callout (callout->device, callout);

		any_callouts = TRUE;
	}

	g_dir_close (dir);

	if (!processing_callouts)
		g_idle_add (process_callouts_idle, NULL);

finish:
	/*
	 * If we're not executing any callouts for this device, go ahead
	 * and emit the "callouts_finished" signal.
	 */
	if (!any_callouts)
		hal_device_callouts_finished (device);
		
}

void
hal_callout_capability (HalDevice *device, const char *capability, gboolean added)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;

	/* Directory doesn't exist.  This isn't an error, just exit
	 * quietly. */
	if (!g_file_test (CAPABILITY_CALLOUT_DIR, G_FILE_TEST_EXISTS))
		return;

	dir = g_dir_open (CAPABILITY_CALLOUT_DIR, 0, &err);

	if (dir == NULL) {
		HAL_WARNING (("Unable to open capability callout directory: "
			      "%s", err->message));
		g_error_free (err);
		return;
	}

	while ((filename = g_dir_read_name (dir)) != NULL) {
		char *full_filename;
		Callout *callout;
		int num_props;

		full_filename = g_build_filename (CAPABILITY_CALLOUT_DIR,
						  filename, NULL);

		if (!g_file_test (full_filename, G_FILE_TEST_IS_EXECUTABLE)) {
			g_free (full_filename);
			continue;
		}

		g_free (full_filename);

		callout = g_new0 (Callout, 1);

		callout->working_dir = CAPABILITY_CALLOUT_DIR;
		callout->filename = g_strdup (filename);
		callout->action = added ? CALLOUT_ADD : CALLOUT_REMOVE;
		callout->device = g_object_ref (device);

		num_props = hal_device_num_properties (device);

		callout->envp_index = 2;
		callout->envp = g_new (char *, callout->envp_index);

		callout->envp[0] = g_strdup_printf ("UDI=%s",
						    hal_device_get_udi (device));
		callout->envp[1] = g_strdup_printf ("CAPABILITY=%s",
						    capability);

		add_pending_callout (callout->device, callout);
	}

	g_dir_close (dir);

	if (!processing_callouts)
		g_idle_add (process_callouts_idle, NULL);
}

void
hal_callout_property (HalDevice *device, const char *key)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;

	/* Directory doesn't exist.  This isn't an error, just exit
	 * quietly. */
	if (!g_file_test (PROPERTY_CALLOUT_DIR, G_FILE_TEST_EXISTS))
		return;

	dir = g_dir_open (PROPERTY_CALLOUT_DIR, 0, &err);

	if (dir == NULL) {
		HAL_WARNING (("Unable to open capability callout directory: "
			      "%s", err->message));
		g_error_free (err);
		return;
	}

	while ((filename = g_dir_read_name (dir)) != NULL) {
		char *full_filename, *value;
		Callout *callout;
		int num_props;

		full_filename = g_build_filename (PROPERTY_CALLOUT_DIR,
						  filename, NULL);

		if (!g_file_test (full_filename, G_FILE_TEST_IS_EXECUTABLE)) {
			g_free (full_filename);
			continue;
		}

		g_free (full_filename);

		callout = g_new0 (Callout, 1);

		callout->working_dir = PROPERTY_CALLOUT_DIR;
		callout->filename = g_strdup (filename);
		callout->action = CALLOUT_MODIFY;
		callout->device = g_object_ref (device);

		num_props = hal_device_num_properties (device);

		value = hal_device_property_to_string (device, key);

		callout->envp_index = 3;
		callout->envp = g_new (char *, callout->envp_index);

		callout->envp[0] = g_strdup_printf ("UDI=%s",
						    hal_device_get_udi (device));
		callout->envp[1] = g_strdup_printf ("PROPERTY=%s", key);
		callout->envp[2] = g_strdup_printf ("VALUE=%s", value);

		add_pending_callout (callout->device, callout);

		g_free (value);
	}

	g_dir_close (dir);

	if (!processing_callouts)
		g_idle_add (process_callouts_idle, NULL);
}
