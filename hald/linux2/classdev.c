/***************************************************************************
 * CVSID: $Id$
 *
 * classdev.c : Handling of functional kernel devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

#include "util.h"
#include "coldplug.h"
#include "hotplug_helper.h"

#include "hotplug.h"
#include "classdev.h"


static void 
input_helper_done(HalDevice *d, gboolean timed_out, gint return_code, gpointer data1, gpointer data2)
{
	gchar udi[256];
	void *end_token = (void *) data1;

	HAL_INFO (("entering; timed_out=%d, return_code=%d", timed_out, return_code));

	/* Discard device if probing reports failure */
	if (return_code != 0) {
		hal_device_store_remove (hald_get_tdl (), d);
		goto out;
	}

	/* Merge properties from .fdi files */
	di_search_and_merge (d);
	
	/* TODO: Run callouts */
	
	/* Compute UDI */
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_logicaldev_input",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	
	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	/* TODO: adjust capabilities on physdev */

out:
	hotplug_event_end (end_token);
}


static gboolean
input_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, void *end_token)
{
	HalDevice *d;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	if (physdev != NULL) {
		hal_device_property_set_string (d, "input.physical_device", physdev->udi);
		hal_device_property_set_string (d, "info.parent", physdev->udi);
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}
	hal_device_property_set_string (d, "info.category", "input");
	hal_device_add_capability (d, "input");
	hal_device_property_set_string (d, "info.bus", "unknown");

	hal_device_property_set_string (d, "input.device", device_file);

	/* Add to temporary device store */
	hal_device_store_add (hald_get_tdl (), d);

	/* probe the actual device node */
	if (!helper_invoke ("hald-probe-input", d, (gpointer) end_token, NULL, input_helper_done, HAL_HELPER_TIMEOUT)) {
		hal_device_store_remove (hald_get_tdl (), d);
		goto out;
	}

	return TRUE;
out:
	hotplug_event_end (end_token);
	return TRUE;
}



void
hotplug_event_begin_add_classdev (const gchar *subsystem, const gchar *sysfs_path, const gchar *device_file, 
				  HalDevice *physdev, void *end_token)
{
	HAL_INFO (("class_add: subsys=%s sysfs_path=%s dev=%s physdev=0x%08x", subsystem, sysfs_path, device_file, physdev));

	if (strcmp (subsystem, "input") == 0)
		input_add (sysfs_path, device_file, physdev, end_token);
	else
		hotplug_event_end (end_token);
}

void
hotplug_event_begin_remove_classdev (const gchar *subsystem, const gchar *sysfs_path, void *end_token)
{
	HalDevice *d;

	HAL_INFO (("class_rem: subsys=%s sysfs_path=%s", subsystem, sysfs_path));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path_device", sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Error removing device"));
	} else {
		const gchar *physdev_udi;
		HalDevice *physdev;

		physdev_udi = hal_device_property_get_string (d, "info.physical_device");

		if (!hal_device_store_remove (hald_get_gdl (), d)) {
			HAL_WARNING (("Error removing device"));
		}

		if (physdev_udi != NULL) {
			physdev = hal_device_store_find (hald_get_gdl (), physdev_udi);
			/* TODO: adjust capabilities on physdev */
		}

	}

	hotplug_event_end (end_token);
}
