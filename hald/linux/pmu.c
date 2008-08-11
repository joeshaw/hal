/***************************************************************************
 * CVSID: $Id$
 *
 * Copyright (C) 2005 Sjoerd Simons <sjoerd at luon.net>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
 
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "../device_info.h"
#include "../device_pm.h"
#include "../hald_dbus.h"
#include "../logger.h"
#include "../util.h"
#include "../util_pm.h"

#include "hotplug.h"
#include "osspec_linux.h"
#include "device.h"

#include "pmu.h"

enum {
	PMU_TYPE_BATTERY,
	PMU_TYPE_AC_ADAPTER,
	PMU_TYPE_LID_BUTTON,
	PMU_TYPE_LAPTOP_PANEL
};


typedef struct PMUDevHandler_s
{
	int pmu_type;
	HalDevice *(*add) (const gchar *pmu_path, HalDevice *parent, struct PMUDevHandler_s *handler);
	gboolean (*compute_udi) (HalDevice *d, struct PMUDevHandler_s *handler);
	gboolean (*remove) (HalDevice *d, struct PMUDevHandler_s *handler);
	gboolean (*refresh) (HalDevice *d, struct PMUDevHandler_s *handler);
} PMUDevHandler;


/* defines from the kernel PMU driver (include/linux/pmu.h) */
#define PMU_BATT_PRESENT	0x00000001
#define PMU_BATT_CHARGING	0x00000002
#define PMU_BATT_TYPE_MASK	0x000000f0
#define PMU_BATT_TYPE_SMART	0x00000010	/* Smart battery */
#define PMU_BATT_TYPE_HOOPER	0x00000020	/* 3400/3500 */
#define PMU_BATT_TYPE_COMET	0x00000030	/* 2400 */

#define PMU_POLL_INTERVAL	2  /* in seconds */

#define PMUDEV			"/dev/pmu"


static gboolean
battery_refresh (HalDevice *d, PMUDevHandler *handler)
{
	const char *path;
	int flags;

	path = hal_device_property_get_string (d, "linux.pmu_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "Battery Bay");
	hal_device_property_set_string (d, "battery.type", "primary");
	hal_device_property_set_string (d, "battery.technology", "unknown");
	hal_device_property_set_string (d, "info.category", "battery");
	hal_device_add_capability (d, "battery");

	flags = hal_util_grep_int_elem_from_file (path, "", "flags", 0, 16, FALSE);

	hal_device_property_set_bool (d, "battery.present", flags & PMU_BATT_PRESENT);

	if (flags & PMU_BATT_PRESENT) {
		int current;

		device_property_atomic_update_begin ();
		hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);

		/* we don't know the unit here :-/ */
		/*hal_device_property_set_string (d, "battery.charge_level.unit", "percent");*/

		hal_device_property_set_bool (d, "battery.rechargeable.is_charging", flags & PMU_BATT_CHARGING);
		/* we're discharging if, and only if, we are not plugged into the wall */
		{
			hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_discharging", "/proc/pmu/info", "", 
							  "AC Power", 0, "0", FALSE);
		}

		hal_util_set_int_elem_from_file (d, "battery.charge_level.current", 
						 path, "", "charge", 0, 10, FALSE);
		hal_util_set_int_elem_from_file (d, "battery.charge_level.last_full", 
						 path, "", "max_charge", 0, 10, FALSE);
		hal_util_set_int_elem_from_file (d, "battery.charge_level.design", 
						 path, "", "max_charge", 0, 10, FALSE);

		current = hal_util_grep_int_elem_from_file (path, "", "current", 0, 10, FALSE);
		if (current > 0)
			hal_device_property_set_int (d, "battery.charge_level.rate", current);
		else
			hal_device_property_set_int (d, "battery.charge_level.rate", -current);

		/* TODO: could read some pmu file? */
		device_pm_calculate_time (d);
		device_pm_calculate_percentage (d);

		device_property_atomic_update_end ();
	} else {
		device_property_atomic_update_begin ();
		device_pm_remove_optional_props (d);
		device_property_atomic_update_end ();		
	}

	return TRUE;
}

static gboolean
ac_adapter_refresh (HalDevice *d, PMUDevHandler *handler)
{
	const char *path;

	path = hal_device_property_get_string (d, "linux.pmu_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "AC Adapter");
	hal_device_property_set_string (d, "info.category", "ac_adapter");
	hal_device_add_capability (d, "ac_adapter");

	hal_util_set_bool_elem_from_file (d, "ac_adapter.present", path, "", "AC Power", 0, "1", FALSE);

	return TRUE;
}

static gboolean
lid_button_refresh (HalDevice *d, PMUDevHandler *handler)
{
	hal_device_property_set_string (d, "info.product", "Lid Switch");
	hal_device_add_capability (d, "button");
	hal_device_property_set_string (d, "info.category", "button");
	hal_device_property_set_string (d, "button.type", "lid");
	hal_device_property_set_bool (d, "button.has_state", TRUE);
	hal_device_property_set_bool (d, "button.state.value", FALSE); 

	/* assume lid is open, polling will tell us otherwise 
	 * (TODO: figure out initial state)
	 */
	return TRUE;
}

/** 
 *  laptop_panel_refresh:
 *  @d:			The hal device
 *  @handler:		What to do
 *
 *  Returns: 		TRUE/FALSE as result of the operation
 *
 *  Refreshes a laptop screen connected to a PMU controller.
 *  This is much simpler than ACPI as we have a *standard* ioctl to use. 
 */
static gboolean
laptop_panel_refresh (HalDevice *d, PMUDevHandler *handler)
{
	if ((hal_device_store_find (hald_get_gdl (),
				    "/org/freedesktop/Hal/devices/computer_backlight")) != NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.category", "laptop_panel");
	hal_device_property_set_string (d, "info.product", "Apple Laptop Panel");

	hal_device_property_set_string (d, "laptop_panel.access_method", "pmu");
	/*
	 * We can set laptop_panel.num_levels as it will not change, 
	 * all powerbooks have 16 steps for brightness, where state 0
	 * is backlight disable.
	 * In tools/hal-system-power-pmu.c we add 1 to the brightness
	 * so we do not turn off the backlight, so we actually have 15
	 * brightness steps inclusive (0-14).
	 */
	hal_device_property_set_int (d, "laptop_panel.num_levels", 15);
	hal_device_add_capability (d, "laptop_panel");
	return TRUE;
}

static gboolean
pmu_lid_compute_udi (HalDevice *d, PMUDevHandler *handler)
{
	gchar udi[256];
	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pmu_lid");
	hal_device_set_udi (d, udi);
	return TRUE;
}

static gboolean
pmu_laptop_panel_compute_udi (HalDevice *d, PMUDevHandler *handler)
{
	gchar udi[256];
	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pmu_lcd");
	hal_device_set_udi (d, udi);
	return TRUE;
}

static gboolean
pmu_poll (gpointer data)
{
	GSList *i;
	GSList *devices;

	devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "battery.type",
								    "primary");
	for (i = devices; i != NULL; i = g_slist_next (i)) {
		HalDevice *d;
		
		d = HAL_DEVICE (i->data);
		if (hal_device_has_property (d, "linux.pmu_type")) {
			hal_util_grep_discard_existing_data ();
			device_property_atomic_update_begin ();
			battery_refresh (d, NULL);
			device_property_atomic_update_end ();		
		}
	}

	g_slist_free (devices);
	devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "info.category",
								    "ac_adapter");
	for (i = devices; i != NULL; i = g_slist_next (i)) {
		HalDevice *d;
		
		d = HAL_DEVICE (i->data);
		if (hal_device_has_property (d, "linux.pmu_type")) {
			hal_util_grep_discard_existing_data ();
			device_property_atomic_update_begin ();
			ac_adapter_refresh (d, NULL);
			device_property_atomic_update_end ();		
		}
	}

	g_slist_free (devices);
	return TRUE;
}

/** 
 *  pmu_synthesize_item:
 *  @fullpath:		The PMU path, e.g. "/dev/pmu/info"
 *  @pmu_type:		The type of device, e.g. PMU_TYPE_BATTERY
 *
 *  Synthesizes a *specific* PMU object. 
 */
static void
pmu_synthesize_item (const gchar *fullpath, int pmu_type)
{
	HotplugEvent *hotplug_event;
	HAL_INFO (("Processing %s", fullpath));
	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_PMU;
	g_strlcpy (hotplug_event->pmu.pmu_path, fullpath, sizeof (hotplug_event->pmu.pmu_path));
	hotplug_event->acpi.acpi_type = pmu_type;
	hotplug_event_enqueue (hotplug_event);
}

/**
 *  pmu_synthesize_hotplug_events  
 *
 *  Returns:              TRUE if, and only if, PMU capabilities were detected
 *
 *  Scan the data structures exported by the kernel and add hotplug
 *  events for adding PMU objects.
 */
gboolean
pmu_synthesize_hotplug_events (void)
{
	gboolean ret;
	HalDevice *computer;
	GError *error;
	GDir *dir;
	gboolean has_battery_bays;

	ret = FALSE;

	has_battery_bays = FALSE;

	if (!g_file_test ("/proc/pmu/info", G_FILE_TEST_EXISTS))
		goto out;

	ret = TRUE;

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
	    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		goto out;
	}

	/* Set appropriate properties on the computer object */
	hal_device_property_set_string (computer, "power_management.type", "pmu");

	/* AC Adapter */
	pmu_synthesize_item ("/proc/pmu/info", PMU_TYPE_AC_ADAPTER);

	error = NULL;
	dir = g_dir_open ("/proc/pmu", 0, &error);
	if (dir != NULL) {
		const gchar *f;
		while ((f = g_dir_read_name (dir)) != NULL) {
			gchar buf[HAL_PATH_MAX];
			int battery_num;

			snprintf (buf, sizeof (buf), "/proc/pmu/%s", f);
			if (sscanf (f, "battery_%d", &battery_num) == 1) {
				has_battery_bays = TRUE;
				pmu_synthesize_item (buf, PMU_TYPE_BATTERY);
			}
			
		}
	} else {
		HAL_ERROR (("Couldn't open /proc/pmu/info: %s", error->message));
		g_error_free (error);
	}

	/* close directory */
	g_dir_close (dir);

	/* We need to make another assumption - that there is a lid button,
	 * if, and only if, the machine has got battery bays
	 */
	if (has_battery_bays) {
		/* Add lid button */
		pmu_synthesize_item ("/proc/pmu/info", PMU_TYPE_LID_BUTTON);

		/* Add Laptop Panel */
		pmu_synthesize_item ("/proc/pmu/info", PMU_TYPE_LAPTOP_PANEL);
	}

	if (!_have_sysfs_power_supply) {
	  	/* setup timer for things that we need to poll */
#ifdef HAVE_GLIB_2_14
		g_timeout_add_seconds (PMU_POLL_INTERVAL,
                	               pmu_poll,
                        	       NULL);
#else
		g_timeout_add (1000 * PMU_POLL_INTERVAL,
			       pmu_poll,
			       NULL);
#endif
	}

out:
	return ret;
}

static HalDevice *
pmu_generic_add (const gchar *pmu_path, HalDevice *parent, PMUDevHandler *handler)
{
	HalDevice *d;

	if (((handler->pmu_type == PMU_TYPE_BATTERY) || (handler->pmu_type == PMU_TYPE_AC_ADAPTER)) && _have_sysfs_power_supply)
		return NULL;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.pmu_path", pmu_path);
	hal_device_property_set_int (d, "linux.pmu_type", handler->pmu_type);
	if (parent != NULL)
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent));
	else
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	if (handler->refresh == NULL || !handler->refresh (d, handler)) {
		g_object_unref (d);
		d = NULL;
	}
	return d;
}

static gboolean
pmu_generic_compute_udi (HalDevice *d, PMUDevHandler *handler)
{
	gchar udi[256];
	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pmu_%s_%d",
			  hal_util_get_last_element (hal_device_property_get_string (d, "linux.pmu_path")),
			  hal_device_property_get_int (d, "linux.pmu_type"));
	hal_device_set_udi (d, udi);
	return TRUE;
}

static gboolean
pmu_generic_remove (HalDevice *d, PMUDevHandler *handler)
{
	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	return TRUE;
}

static PMUDevHandler pmudev_handler_battery = { 
	.pmu_type    = PMU_TYPE_BATTERY,
	.add         = pmu_generic_add,
	.compute_udi = pmu_generic_compute_udi,
	.refresh     = battery_refresh,
	.remove      = pmu_generic_remove
};

static PMUDevHandler pmudev_handler_ac_adapter = { 
	.pmu_type    = PMU_TYPE_AC_ADAPTER,
	.add         = pmu_generic_add,
	.compute_udi = pmu_generic_compute_udi,
	.refresh     = ac_adapter_refresh,
	.remove      = pmu_generic_remove
};

static PMUDevHandler pmudev_handler_lid_button = { 
	.pmu_type    = PMU_TYPE_LID_BUTTON,
	.add         = pmu_generic_add,
	.compute_udi = pmu_lid_compute_udi,
	.refresh     = lid_button_refresh,
	.remove      = pmu_generic_remove
};

static PMUDevHandler pmudev_handler_laptop_panel = { 
	.pmu_type    = PMU_TYPE_LAPTOP_PANEL,
	.add         = pmu_generic_add,
	.compute_udi = pmu_laptop_panel_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = pmu_generic_remove
};

static PMUDevHandler *pmu_handlers[] = {
	&pmudev_handler_battery,
	&pmudev_handler_ac_adapter,
	&pmudev_handler_lid_button,
	&pmudev_handler_laptop_panel,
	NULL
};

void
hotplug_event_begin_add_pmu (const gchar *pmu_path, int pmu_type, HalDevice *parent, void *end_token)
{
	guint i;

	HAL_INFO (("pmu_add: pmu_path=%s pmu_type=%d, parent=0x%08x", pmu_path, pmu_type, parent));

	for (i = 0; pmu_handlers [i] != NULL; i++) {
		PMUDevHandler *handler;

		handler = pmu_handlers[i];
		if (handler->pmu_type == pmu_type) {
			HalDevice *d;

			d = handler->add (pmu_path, parent, handler);
			if (d == NULL) {
				/* didn't find anything - thus, ignore this hotplug event */
				hotplug_event_end (end_token);
				goto out;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_PMU);

			/* Add to temporary device store */
			hal_device_store_add (hald_get_tdl (), d);

			/* Compute UDI */
			if (!handler->compute_udi (d, handler)) {
				hal_device_store_remove (hald_get_tdl (), d);
				hotplug_event_end (end_token);
				goto out;
			}

			/* Merge properties from .fdi files */
			di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
			di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

			/* TODO: Run callouts */
			
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
hotplug_event_begin_remove_pmu (const gchar *pmu_path, int pmu_type, void *end_token)
{
	guint i;
	HalDevice *d;

	HAL_INFO (("pmu_rem: pmu_path=%s pmu_type=%d", pmu_path, pmu_type));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.pmu_path", pmu_path);
	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device with pmu path %s - not found", pmu_path));
		goto out;
	}

	for (i = 0; pmu_handlers [i] != NULL; i++) {
		PMUDevHandler *handler;

		handler = pmu_handlers[i];
		if (handler->pmu_type == pmu_type) {
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
pmu_rescan_device (HalDevice *d)
{
	guint i;
	gboolean ret;
	int pmu_type;

	ret = FALSE;

	pmu_type = hal_device_property_get_int (d, "linux.pmu_type");

	for (i = 0; pmu_handlers [i] != NULL; i++) {
		PMUDevHandler *handler;

		handler = pmu_handlers[i];
		if (handler->pmu_type == pmu_type) {
			ret = handler->refresh (d, handler);
			goto out;
		}
	}

	HAL_WARNING (("Didn't find a rescan handler for udi %s", hal_device_get_udi (d)));

out:
	return ret;
}

HotplugEvent *
pmu_generate_add_hotplug_event (HalDevice *d)
{
	int pmu_type;
	const char *pmu_path;
	HotplugEvent *hotplug_event;

	pmu_path = hal_device_property_get_string (d, "linux.pmu_path");
	pmu_type = hal_device_property_get_int (d, "linux.pmu_type");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_PMU;
	g_strlcpy (hotplug_event->pmu.pmu_path, pmu_path, sizeof (hotplug_event->pmu.pmu_path));
	hotplug_event->pmu.pmu_type = pmu_type;
	return hotplug_event;
}

HotplugEvent *
pmu_generate_remove_hotplug_event (HalDevice *d)
{
	int pmu_type;
	const char *pmu_path;
	HotplugEvent *hotplug_event;

	pmu_path = hal_device_property_get_string (d, "linux.pmu_path");
	pmu_type = hal_device_property_get_int (d, "linux.pmu_type");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_PMU;
	g_strlcpy (hotplug_event->pmu.pmu_path, pmu_path, sizeof (hotplug_event->pmu.pmu_path));
	hotplug_event->pmu.pmu_type = pmu_type;
	return hotplug_event;
}
