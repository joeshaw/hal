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

#include "pmu.h"
#include "hotplug.h"

enum {
	PMU_TYPE_BATTERY,
	PMU_TYPE_AC_ADAPTER
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
#define PMU_BATT_PRESENT  0x00000001
#define PMU_BATT_CHARGING 0x00000002
#define PMU_BATT_TYPE_MASK  0x000000f0
#define PMU_BATT_TYPE_SMART 0x00000010	  /* Smart battery */
#define PMU_BATT_TYPE_HOOPER  0x00000020  /* 3400/3500 */
#define PMU_BATT_TYPE_COMET 0x00000030	  /* 2400 */

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
	hal_device_property_set_string (d, "info.category", "battery");
	hal_device_add_capability (d, "battery");

	flags = hal_util_grep_int_elem_from_file (path, "", "flags", 0, 16);

	hal_device_property_set_bool (d, "battery.present", flags & PMU_BATT_PRESENT);

	if (flags & PMU_BATT_PRESENT) {
		device_property_atomic_update_begin ();
		hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);

		/* we don't know the unit here :-/ */
		/*hal_device_property_set_string (d, "battery.charge_level.unit", "percent");*/

		hal_device_property_set_bool (d, "battery.rechargeable.is_charging", flags & PMU_BATT_CHARGING);
		/* we're discharging if, and only if, we are not plugged into the wall */
		{
			char buf[HAL_PATH_MAX];
			snprintf (buf, sizeof (buf), "%s/pmu/info", hal_proc_path);
			hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_discharging", buf, "", 
							  "AC Power", 0, "0");
		}

		hal_util_set_int_elem_from_file (d, "battery.charge_level.current", path, "", "charge", 0, 10);
		hal_util_set_int_elem_from_file (d, "battery.charge_level.maximum", path, "", "max_charge", 0, 10);

		device_property_atomic_update_end ();
	} else {
		device_property_atomic_update_begin ();
		hal_device_property_remove (d, "battery.is_rechargeable");
		hal_device_property_remove (d, "battery.rechargeable.is_charging");
		hal_device_property_remove (d, "battery.rechargeable.is_discharging");
		/*hal_device_property_remove (d, "battery.charge_level.unit");*/
		hal_device_property_remove (d, "battery.charge_level.current");
		hal_device_property_remove (d, "battery.charge_level.maximum");
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
	hal_device_property_set_string (d, "info.category", "system.ac_adapter");
	hal_device_add_capability (d, "system.ac_adapter");

	hal_util_set_bool_elem_from_file (d, "system.ac_adapter.present", path, "", "AC Power", 0, "1");	

	return TRUE;
}

/** Scan the data structures exported by the kernel and add hotplug
 *  events for adding PMU objects.
 *
 *  @param                      TRUE if, and only if, PMU capabilities
 *                              were detected
 */
gboolean
pmu_synthesize_hotplug_events (void)
{
	gboolean ret;
	HalDevice *computer;
	gchar path[HAL_PATH_MAX];
	HotplugEvent *hotplug_event;
	GError *error;
	GDir *dir;

	ret = FALSE;

	if (!g_file_test ("/proc/pmu/info", G_FILE_TEST_EXISTS))
		goto out;

	ret = TRUE;

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		goto out;
	}

	/* Set appropriate properties on the computer object */
	hal_device_property_set_bool (computer, "power_management.is_enabled", TRUE);
	hal_device_property_set_string (computer, "power_management.type", "pmu");

	/* AC Adapter */
	snprintf (path, sizeof (path), "%s/pmu/info", hal_proc_path);
	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->is_add = TRUE;
	hotplug_event->type = HOTPLUG_EVENT_PMU;
	g_strlcpy (hotplug_event->pmu.pmu_path, path, sizeof (hotplug_event->pmu.pmu_path));
	hotplug_event->pmu.pmu_type = PMU_TYPE_AC_ADAPTER;
	hotplug_event_enqueue (hotplug_event);

	error = NULL;
	snprintf (path, sizeof (path), "%s/pmu", hal_proc_path);
	dir = g_dir_open (path, 0, &error);
	if (dir != NULL) {
		const gchar *f;
			
		while ((f = g_dir_read_name (dir)) != NULL) {
			HotplugEvent *hotplug_event;
			gchar buf[HAL_PATH_MAX];
			int battery_num;

			snprintf (buf, sizeof (buf), "%s/pmu/%s", hal_proc_path, f);
			if (sscanf (f, "battery_%d", &battery_num) == 1) {
				HAL_INFO (("Processing %s", buf));
				
				hotplug_event = g_new0 (HotplugEvent, 1);
				hotplug_event->is_add = TRUE;
				hotplug_event->type = HOTPLUG_EVENT_PMU;
				g_strlcpy (hotplug_event->pmu.pmu_path, buf, sizeof (hotplug_event->pmu.pmu_path));
				hotplug_event->pmu.pmu_type = PMU_TYPE_BATTERY;
				
				hotplug_event_enqueue (hotplug_event);
			}
			
		}
	} else {
		HAL_ERROR (("Couldn't open %s: %s", path, error->message));
		g_error_free (error);
	}

	/* close directory */
	g_dir_close (dir);

out:
	return ret;
}

static HalDevice *
pmu_generic_add (const gchar *pmu_path, HalDevice *parent, PMUDevHandler *handler)
{
	HalDevice *d;
	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.pmu_path", pmu_path);
	hal_device_property_set_int (d, "linux.pmu_type", handler->pmu_type);
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
pmu_generic_compute_udi (HalDevice *d, PMUDevHandler *handler)
{
	gchar udi[256];
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pmu_%s_%d",
			      hal_util_get_last_element (hal_device_property_get_string (d, "linux.pmu_path")),
			      hal_device_property_get_int (d, "linux.pmu_type"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
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

static PMUDevHandler *pmu_handlers[] = {
	&pmudev_handler_battery,
	&pmudev_handler_ac_adapter,
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

	HAL_WARNING (("Didn't find a rescan handler for udi %s", d->udi));

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
	hotplug_event->is_add = TRUE;
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
	hotplug_event->is_add = FALSE;
	hotplug_event->type = HOTPLUG_EVENT_PMU;
	g_strlcpy (hotplug_event->pmu.pmu_path, pmu_path, sizeof (hotplug_event->pmu.pmu_path));
	hotplug_event->pmu.pmu_type = pmu_type;
	return hotplug_event;
}
