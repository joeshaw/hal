/***************************************************************************
 * CVSID: $Id$
 *
 * Copyright (C) 2005 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2005 David Zeuthen, Red Hat Inc., <davidz@redhat.com>
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

#include <string.h>

#include "../callout.h"
#include "../logger.h"
#include "../hald_dbus.h"
#include "util.h"

#include "acpi.h"

static void
acpi_refresh_ac_adapter (HalDevice *d, const gchar *path)
{
	hal_device_property_set_string (d, "info.product", "AC Adapter");
	hal_device_property_set_string (d, "info.category", "system.ac_adapter");
	hal_device_add_capability (d, "system.ac_adapter");
	hal_util_set_bool_elem_from_file (d, "system.ac_adapter.present", path, "state", "state", 0, "on-line");
}

static void
acpi_refresh_button (HalDevice *d, const gchar *path)
{
	gchar *parent_path = hal_util_get_parent_path (path);
	const gchar *button_type = hal_util_get_last_element (parent_path);

	hal_device_property_set_string (d, "system.button.type", button_type);

	if (strcmp (button_type, "power") == 0)   
		hal_device_property_set_string (d, "info.product", "Power Button");
	else if (strcmp (button_type, "lid") == 0)   
		hal_device_property_set_string (d, "info.product", "Lid Switch");
	else if (strcmp (button_type, "sleep") == 0)   
		hal_device_property_set_string (d, "info.product", "Sleep Button");

	hal_device_property_set_string (d, "info.category", "system.button");
	hal_device_add_capability (d, "system.button");
	if (!hal_util_set_bool_elem_from_file (d, "system.button.state.value", path, "state", "state", 0, "closed")) {
		hal_device_property_set_bool (d, "system.button.has_state", FALSE);
	} else {
		hal_device_property_set_bool (d, "system.button.has_state", TRUE);
	}

	g_free (parent_path);
}

static void
acpi_refresh_battery (HalDevice *d, const gchar *path)
{
	hal_device_property_set_string (d, "info.product", "Battery Bay");
	hal_device_property_set_string (d, "battery.type", "primary");
	hal_device_property_set_string (d, "info.category", "battery");
	hal_device_add_capability (d, "battery");

	hal_util_set_bool_elem_from_file (d, "battery.present", path, "info", "present", 0, "yes");
	if (!hal_device_property_get_bool (d, "battery.present")) {
		device_property_atomic_update_begin ();
		hal_device_property_remove (d, "battery.is_rechargeable");
		hal_device_property_remove (d, "battery.rechargeable.is_charging");
		hal_device_property_remove (d, "battery.rechargeable.is_discharging");
		hal_device_property_remove (d, "battery.vendor");
		hal_device_property_remove (d, "battery.model");
		hal_device_property_remove (d, "battery.serial");
		hal_device_property_remove (d, "battery.technology");
		hal_device_property_remove (d, "battery.vendor");
		hal_device_property_remove (d, "battery.charge_level.unit");
		hal_device_property_remove (d, "battery.charge_level.current");
		hal_device_property_remove (d, "battery.charge_level.maximum_specified");
		device_property_atomic_update_end ();		
	} else {
		device_property_atomic_update_begin ();
		hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);
		hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_charging", path, 
						  "state", "charging state", 0, "charging");

		hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_discharging", path, 
						  "state", "charging state", 0, "discharging");
		
		hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info", "OEM info", 0);
		hal_util_set_string_elem_from_file (d, "battery.model", path, "info", "model number", 0);
		hal_util_set_string_elem_from_file (d, "battery.serial", path, "info", "serial number", 0);
		hal_util_set_string_elem_from_file (d, "battery.technology", path, "info", "battery type", 0);
		hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info", "OEM info", 0);
		hal_util_set_string_elem_from_file (d, "battery.charge_level.unit", path, "info", "design capacity", 1);

		hal_util_set_int_elem_from_file (d, "battery.charge_level.current", path, 
						 "state", "remaining capacity", 0);

		hal_util_set_int_elem_from_file (d, "battery.charge_level.maximum_specified", path, 
						 "info", "design capacity", 0);
		device_property_atomic_update_end ();
	}
}

static void
acpi_add_objects (const gchar *path, void (*refresh_handler)(HalDevice *d, const gchar *path))
{
	const gchar *f;
	GDir *dir;
	GError *error;

	dir = g_dir_open (path, 0, &error);
	if (dir == NULL) {
		HAL_ERROR (("Couldn't open %s", path));
		goto out;
	}

	while ((f = g_dir_read_name (dir)) != NULL) {
		gchar buf[HAL_PATH_MAX];
		gchar buf2[HAL_PATH_MAX];
		HalDevice *d;

		snprintf (buf, sizeof (buf), "%s/%s", path, f);
		HAL_INFO (("Processing %s", buf));

		d = hal_device_new ();
		snprintf (buf2, sizeof (buf2), "/org/freedesktop/Hal/devices/acpi_%s", f);
		hal_device_set_udi (d, buf2);
		hal_device_property_set_string (d, "info.udi", buf2);
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
		hal_device_property_set_string (d, "linux.acpi_path", buf);

		refresh_handler (d, buf);

		hal_device_store_add (hald_get_gdl (), d);
	}

	/* close directory */
	g_dir_close (dir);

out:
	;
}

static gboolean
poll_acpi_device (HalDeviceStore *store,
		  HalDevice      *device,
		  gpointer        user_data)
{
	if (hal_device_has_property (device, "linux.acpi_path")) {
		const char *path;

		path = hal_device_property_get_string (device, "linux.acpi_path");

		if (hal_device_has_capability (device, "battery"))
			acpi_refresh_battery (device, path);
		else if (hal_device_has_capability (device, "system.button"))
			acpi_refresh_button (device, path);
		else if (hal_device_has_capability (device, "system.ac_adapter"))
			acpi_refresh_ac_adapter (device, path);
	}

	return TRUE;
}


static gboolean
poll_acpi (gpointer data)
{
	hal_device_store_foreach (hald_get_gdl (), poll_acpi_device, NULL);
	return TRUE;
}


/** Scan the data structures exported by the kernel and build
 *  device objects representing ACPI objects
 */
void
acpi_probe (void)
{
	HalDevice *computer;
	gchar path[HAL_PATH_MAX];

	if (!g_file_test ("/proc/acpi/info", G_FILE_TEST_EXISTS))
		goto out;

	HAL_INFO (("Adding device objects for ACPI"));

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		goto out;
	}

	/* only do ACPI specific options */
	hal_device_property_set_bool (computer, "power_management.is_enabled", TRUE);
	hal_device_property_set_string (computer, "power_management.type", "acpi");
	hal_util_set_string_elem_from_file (computer, "power_management.acpi.linux.version", 
					    "/proc/acpi", "info", "version", 0);

	/* collect batteries */
	snprintf (path, sizeof (path), "%s/acpi/battery", hal_proc_path);
	acpi_add_objects (path, acpi_refresh_battery);

	/* collect AC adapters */
	snprintf (path, sizeof (path), "%s/acpi/ac_adapter", hal_proc_path);
	acpi_add_objects (path, acpi_refresh_ac_adapter);

	/* collect buttons */
	snprintf (path, sizeof (path), "%s/acpi/button/lid", hal_proc_path);
	acpi_add_objects (path, acpi_refresh_button);
	snprintf (path, sizeof (path), "%s/acpi/button/power", hal_proc_path);
	acpi_add_objects (path, acpi_refresh_button);
	snprintf (path, sizeof (path), "%s/acpi/button/sleep", hal_proc_path);
	acpi_add_objects (path, acpi_refresh_button);

	/* For fun, poll the objects..
	 *
	 * This needs to be replaced with listening to the ACPI socket! Uhm, also
	 * because this is the only way to catch when someone presses a button
	 * without state.
	 */
	g_timeout_add (2000, poll_acpi, NULL);	

out:
	;
}
