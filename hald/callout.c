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
} Callout;

static void process_callouts (void);

static GSList *pending_callouts = NULL;
static gboolean processing_callouts = FALSE;

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

static gboolean
wait_for_callout (gpointer user_data)
{
	Callout *callout = user_data;
	int status;

	status = waitpid (callout->pid, NULL, WNOHANG);

	if (status == 0) {
		/* Not finished yet... */
		return TRUE;
	} else if (status == -1) {
		if (errno == EINTR)
			return TRUE;
		else {
			HAL_WARNING (("waitpid errno %d: %s", errno,
				      strerror (errno)));
		}
	} else {
		g_free (callout->filename);
		g_strfreev (callout->envp);
		g_object_unref (callout->device);
		g_free (callout);

		process_callouts ();
	}

	return FALSE;
}

static void
process_callouts (void)
{
	Callout *callout;
	char *argv[3];
	GError *err = NULL;

	if (pending_callouts == NULL) {
		processing_callouts = FALSE;
		return;
	}

	processing_callouts = TRUE;

	callout = (Callout *) pending_callouts->data;
	pending_callouts = g_slist_remove (pending_callouts, callout);

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

	hal_device_property_foreach (callout->device, add_property_to_env,
				     callout);

	if (!g_spawn_async (callout->working_dir, argv, callout->envp,
			    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
			    &callout->pid, &err)) {
		HAL_WARNING (("Couldn't invoke %s: %s", argv[0],
			      err->message));
		g_error_free (err);
	}

	g_timeout_add (250, wait_for_callout, callout);
}

void
hal_callout_device (HalDevice *device, gboolean added)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;

	/* XXX */
	return;

	/* Directory doesn't exist.  This isn't an error, just exit
	 * quietly. */
	if (!g_file_test (DEVICE_CALLOUT_DIR, G_FILE_TEST_EXISTS))
		return;

	dir = g_dir_open (DEVICE_CALLOUT_DIR, 0, &err);

	if (dir == NULL) {
		HAL_WARNING (("Unable to open device callout directory: %s",
			      err->message));
		g_error_free (err);
		return;
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

		/* Extra one for the UDI, extra one for NULL */
		callout->envp = g_new0 (char *, num_props + 2);

		callout->envp[0] = g_strdup_printf ("UDI=%s",
						    hal_device_get_udi (device));
		callout->envp_index = 1;

		pending_callouts = g_slist_append (pending_callouts, callout);
	}

	g_dir_close (dir);

	if (pending_callouts != NULL && !processing_callouts)
		process_callouts ();
}

void
hal_callout_capability (HalDevice *device, const char *capability, gboolean added)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;

	/* XXX */
	return;


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

		/* Extra one for UDI, one for capability, and one for NULL */
		callout->envp = g_new0 (char *, num_props + 3);

		callout->envp[0] = g_strdup_printf ("UDI=%s",
						    hal_device_get_udi (device));
		callout->envp[1] = g_strdup_printf ("CAPABILITY=%s",
						    capability);
		callout->envp_index = 2;

		pending_callouts = g_slist_append (pending_callouts, callout);
	}

	g_dir_close (dir);

	if (pending_callouts != NULL && !processing_callouts)
		process_callouts ();
}

void
hal_callout_property (HalDevice *device, const char *key)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;

	/* XXX */
	return;


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

		/* Extra one for UDI, two key/value, and one for NULL */
		callout->envp = g_new0 (char *, num_props + 4);

		callout->envp[0] = g_strdup_printf ("UDI=%s",
					   hal_device_get_udi (device));
		callout->envp[1] = g_strdup_printf ("PROPERTY=%s", key);
		callout->envp[2] = g_strdup_printf ("VALUE=%s", value);
		callout->envp_index = 3;

		pending_callouts = g_slist_append (pending_callouts, callout);

		g_free (value);
	}

	g_dir_close (dir);

	if (pending_callouts != NULL && !processing_callouts)
		process_callouts ();
}
