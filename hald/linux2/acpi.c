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
#include "../device_info.h"
#include "../logger.h"
#include "../hald_dbus.h"
#include "util.h"

#include "acpi.h"
#include "hotplug.h"

enum {
	ACPI_TYPE_BATTERY,
	ACPI_TYPE_AC_ADAPTER,
	ACPI_TYPE_BUTTON
};

typedef struct ACPIDevHandler_s
{
	int acpi_type;
	HalDevice *(*add) (const gchar *acpi_path, HalDevice *parent, struct ACPIDevHandler_s *handler);
	gboolean (*compute_udi) (HalDevice *d, struct ACPIDevHandler_s *handler);
	gboolean (*remove) (HalDevice *d, struct ACPIDevHandler_s *handler);
	gboolean (*refresh) (HalDevice *d, struct ACPIDevHandler_s *handler);
} ACPIDevHandler;

static gboolean
battery_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

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

	return TRUE;
}

static gboolean
ac_adapter_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "AC Adapter");
	hal_device_property_set_string (d, "info.category", "system.ac_adapter");
	hal_device_add_capability (d, "system.ac_adapter");
	hal_util_set_bool_elem_from_file (d, "system.ac_adapter.present", path, "state", "state", 0, "on-line");

	return TRUE;
}

static gboolean
button_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;
	gchar *parent_path;
	const gchar *button_type;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	parent_path = hal_util_get_parent_path (path);
	button_type = hal_util_get_last_element (parent_path);

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

	return TRUE;
}


static void
acpi_synthesize (const gchar *path, int acpi_type)
{
	const gchar *f;
	GDir *dir;
	GError *error = NULL;

	dir = g_dir_open (path, 0, &error);
	if (dir == NULL) {
		HAL_ERROR (("Couldn't open %s: %s", path, error->message));
		g_error_free (error);
		goto out;
	}

	while ((f = g_dir_read_name (dir)) != NULL) {
		HotplugEvent *hotplug_event;
		gchar buf[HAL_PATH_MAX];

		snprintf (buf, sizeof (buf), "%s/%s", path, f);
		HAL_INFO (("Processing %s", buf));

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = TRUE;
		hotplug_event->type = HOTPLUG_EVENT_ACPI;
		g_strlcpy (hotplug_event->acpi.acpi_path, buf, sizeof (hotplug_event->acpi.acpi_path));
		hotplug_event->acpi.acpi_type = acpi_type;

		hotplug_event_enqueue (hotplug_event);
	}

	/* close directory */
	g_dir_close (dir);

out:
	;
}

/** Scan the data structures exported by the kernel and add hotplug
 *  events for adding ACPI objects.
 *
 *  @param                      TRUE if, and only if, ACPI capabilities
 *                              were detected
 */
gboolean
acpi_synthesize_hotplug_events (void)
{
	gboolean ret;
	HalDevice *computer;
	gchar path[HAL_PATH_MAX];

	ret = FALSE;

	if (!g_file_test ("/proc/acpi/info", G_FILE_TEST_EXISTS))
		goto out;

	ret = TRUE;

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		goto out;
	}

	/* Set appropriate properties on the computer object */
	hal_device_property_set_bool (computer, "power_management.is_enabled", TRUE);
	hal_device_property_set_string (computer, "power_management.type", "acpi");
	hal_util_set_string_elem_from_file (computer, "power_management.acpi.linux.version", 
					    "/proc/acpi", "info", "version", 0);

	/* collect batteries */
	snprintf (path, sizeof (path), "%s/acpi/battery", hal_proc_path);
	acpi_synthesize (path, ACPI_TYPE_BATTERY);

	/* collect AC adapters */
	snprintf (path, sizeof (path), "%s/acpi/ac_adapter", hal_proc_path);
	acpi_synthesize (path, ACPI_TYPE_AC_ADAPTER);

	/* collect buttons */
	snprintf (path, sizeof (path), "%s/acpi/button/lid", hal_proc_path);
	acpi_synthesize (path, ACPI_TYPE_BUTTON);
	snprintf (path, sizeof (path), "%s/acpi/button/power", hal_proc_path);
	acpi_synthesize (path, ACPI_TYPE_BUTTON);
	snprintf (path, sizeof (path), "%s/acpi/button/sleep", hal_proc_path);
	acpi_synthesize (path, ACPI_TYPE_BUTTON);

out:
	return ret;
}

static HalDevice *
acpi_generic_add (const gchar *acpi_path, HalDevice *parent, ACPIDevHandler *handler)
{
	HalDevice *d;
	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.acpi_path", acpi_path);
	hal_device_property_set_int (d, "linux.acpi_type", handler->acpi_type);
	if (parent != NULL)
		hal_device_property_set_string (d, "info.parent", parent->udi);
	else
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	if (handler->refresh == NULL || !handler->refresh (d, handler)) {
		g_object_unref (d);
		d = NULL;
	}
	return d;
}

static gboolean
acpi_generic_compute_udi (HalDevice *d, ACPIDevHandler *handler)
{
	gchar udi[256];
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/acpi_%s",
			      hal_util_get_last_element (hal_device_property_get_string (d, "linux.acpi_path")));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

static gboolean
acpi_generic_remove (HalDevice *d, ACPIDevHandler *handler)
{
	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	return TRUE;
}


static ACPIDevHandler acpidev_handler_battery = { 
	.acpi_type   = ACPI_TYPE_BATTERY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = battery_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_button = { 
	.acpi_type   = ACPI_TYPE_BUTTON,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = button_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_ac_adapter = { 
	.acpi_type   = ACPI_TYPE_AC_ADAPTER,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = ac_adapter_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler *acpi_handlers[] = {
	&acpidev_handler_battery,
	&acpidev_handler_button,
	&acpidev_handler_ac_adapter,
	NULL
};

void
hotplug_event_begin_add_acpi (const gchar *acpi_path, int acpi_type, HalDevice *parent, void *end_token)
{
	guint i;

	HAL_INFO (("acpi_add: acpi_path=%s acpi_type=%d, parent=0x%08x", acpi_path, acpi_type, parent));

	for (i = 0; acpi_handlers [i] != NULL; i++) {
		ACPIDevHandler *handler;

		handler = acpi_handlers[i];
		if (handler->acpi_type == acpi_type) {
			HalDevice *d;

			d = handler->add (acpi_path, parent, handler);
			if (d == NULL) {
				/* didn't find anything - thus, ignore this hotplug event */
				hotplug_event_end (end_token);
				goto out;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_ACPI);

			/* Add to temporary device store */
			hal_device_store_add (hald_get_tdl (), d);

			/* Merge properties from .fdi files */
			di_search_and_merge (d);

			/* TODO: Run callouts */
			
			/* Compute UDI */
			if (!handler->compute_udi (d, handler)) {
				hal_device_store_remove (hald_get_tdl (), d);
				hotplug_event_end (end_token);
				goto out;
			}

			/* Move from temporary to global device store */
			hal_device_store_remove (hald_get_tdl (), d);
			hal_device_store_add (hald_get_gdl (), d);
			
			hotplug_event_end (end_token);
			goto out;
		}
	}
	
	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out:
	;
}

void
hotplug_event_begin_remove_acpi (const gchar *acpi_path, int acpi_type, void *end_token)
{
	guint i;
	HalDevice *d;

	HAL_INFO (("acpi_rem: acpi_path=%s acpi_type=%d", acpi_path, acpi_type));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.acpi_path", acpi_path);
	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device with acpi path %s - not found", acpi_path));
		goto out;
	}

	for (i = 0; acpi_handlers [i] != NULL; i++) {
		ACPIDevHandler *handler;

		handler = acpi_handlers[i];
		if (handler->acpi_type == acpi_type) {
			if (handler->remove (d, handler)) {
				hotplug_event_end (end_token);
				goto out2;
			}
		}
	}
out:
	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out2:
	;
}

gboolean
acpi_rescan_device (HalDevice *d)
{
	guint i;
	gboolean ret;
	int acpi_type;

	ret = FALSE;

	acpi_type = hal_device_property_get_int (d, "linux.acpi_type");

	for (i = 0; acpi_handlers [i] != NULL; i++) {
		ACPIDevHandler *handler;

		handler = acpi_handlers[i];
		if (handler->acpi_type == acpi_type) {
			ret = handler->refresh (d, handler);
			goto out;
		}
	}

	HAL_WARNING (("Didn't find a rescan handler for udi %s", d->udi));

out:
	return ret;
}

HotplugEvent *
acpi_generate_add_hotplug_event (HalDevice *d)
{
	int acpi_type;
	const char *acpi_path;
	HotplugEvent *hotplug_event;

	acpi_path = hal_device_property_get_string (d, "linux.acpi_path");
	acpi_type = hal_device_property_get_int (d, "linux.acpi_type");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->is_add = TRUE;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, acpi_path, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = acpi_type;
	return hotplug_event;
}

HotplugEvent *
acpi_generate_remove_hotplug_event (HalDevice *d)
{
	int acpi_type;
	const char *acpi_path;
	HotplugEvent *hotplug_event;

	acpi_path = hal_device_property_get_string (d, "linux.acpi_path");
	acpi_type = hal_device_property_get_int (d, "linux.acpi_type");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->is_add = FALSE;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, acpi_path, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = acpi_type;
	return hotplug_event;
}
