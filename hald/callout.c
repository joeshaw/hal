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

#include "callout.h"
#include "logger.h"

#define DEVICE_CALLOUT_DIR     PACKAGE_SYSCONF_DIR "/hal/device.d"
#define CAPABILITY_CALLOUT_DIR PACKAGE_SYSCONF_DIR "/hal/capability.d"

typedef struct {
	char **envp;
	int index;
} ForeachPropInfo;

static gboolean
add_property_to_env (HalDevice *device, HalProperty *property, 
		     gpointer user_data)
{
	ForeachPropInfo *info = user_data;
	char *prop_upper;
	char *c;

	prop_upper = g_ascii_strup (hal_property_get_key (property), -1);

	/* periods aren't valid in the environment, so replace them with
	 * underscores. */
	for (c = prop_upper; *c; c++) {
		if (*c == '.')
			*c = '_';
	}

	info->envp[info->index] =
		g_strdup_printf ("HAL_PROP_%s=%s",
				 prop_upper,
				 hal_property_get_as_string (property));

	g_free (prop_upper);

	info->index++;

	return TRUE;
}

void
hal_callout_device (HalDevice *device, gboolean added)
{
	GDir *dir;
	GError *err = NULL;
	const char *filename;

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
		char *argv[] = { (char *) filename,
				 added == TRUE ? "add" : "remove",
				 NULL };
		char **envp;
		char *full_filename;
		int num_props;
		ForeachPropInfo info;

		full_filename = g_build_filename (DEVICE_CALLOUT_DIR,
						  filename, NULL);

		if (!g_file_test (full_filename, G_FILE_TEST_IS_EXECUTABLE)) {
			g_free (full_filename);
			continue;
		}

		g_free (full_filename);

		num_props = hal_device_num_properties (device);

		/* Extra one for the UDI, extra one for NULL */
		envp = g_new0 (char *, num_props + 2);
		
		envp[0] = g_strdup_printf ("UDI=%s",
					   hal_device_get_udi (device));

		info.envp = envp;
		info.index = 1;

		hal_device_property_foreach (device, add_property_to_env,
					     &info);

		if (!g_spawn_sync (DEVICE_CALLOUT_DIR, argv, envp, 0,
				   NULL, NULL, NULL, NULL, NULL, &err)) {
			HAL_WARNING (("Couldn't invoke %s: %s", argv[0],
				      err->message));
			g_error_free (err);
		}

		g_strfreev (envp);
	}
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
		char *argv[] = { (char *) filename,
				 added == TRUE ? "add" : "remove",
				 NULL };
		char **envp;
		char *full_filename;
		int num_props;
		ForeachPropInfo info;

		full_filename = g_build_filename (CAPABILITY_CALLOUT_DIR,
						  filename, NULL);

		if (!g_file_test (full_filename, G_FILE_TEST_IS_EXECUTABLE)) {
			g_free (full_filename);
			continue;
		}

		g_free (full_filename);
		
		num_props = hal_device_num_properties (device);

		/* Extra one for UDI, one for capability, and one for NULL */
		envp = g_new0 (char *, num_props + 3);

		envp[0] = g_strdup_printf ("UDI=%s",
					   hal_device_get_udi (device));
		envp[1] = g_strdup_printf ("CAPABILITY=%s", capability);

		info.envp = envp;
		info.index = 2;

		if (!g_spawn_sync (DEVICE_CALLOUT_DIR, argv, envp, 0,
				   NULL, NULL, NULL, NULL, NULL, &err)) {
			HAL_WARNING (("Couldn't invoke %s: %s", argv[0],
				      err->message));
			g_error_free (err);
		}

		g_strfreev (envp);
	}
}
