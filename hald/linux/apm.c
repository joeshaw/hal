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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>

#include "../hald_dbus.h"
#include "../device_info.h"
#include "../logger.h"
#include "../util.h"
#include "../util_pm.h"

#include "osspec_linux.h"

#include "apm.h"

enum {
	APM_TYPE_BATTERY,
	APM_TYPE_AC_ADAPTER
};

static int interval_poll_round = 0;

typedef struct APMDevHandler_s
{
	int apm_type;
	HalDevice *(*add) (const gchar *apm_path, HalDevice *parent, struct APMDevHandler_s *handler);
	gboolean (*compute_udi) (HalDevice *d, struct APMDevHandler_s *handler);
	gboolean (*remove) (HalDevice *d, struct APMDevHandler_s *handler);
	gboolean (*refresh) (HalDevice *d, struct APMDevHandler_s *handler);
} APMDevHandler;

typedef struct {
	char driver_version[256];
	int version_major;
	int version_minor;
	int flags;
	int ac_line_status;
	int battery_status;
	int battery_flags;
	int battery_percentage;
	int battery_time;
} APMInfo;

#define APM_POLL_INTERVAL 2  /* in seconds */

static gboolean
apm_poll (gpointer data)
{
	GSList *i;
	GSList *devices;

	devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "linux.apm_path",
								    "/proc/apm");
	for (i = devices; i != NULL; i = g_slist_next (i)) {
		HalDevice *d;		
		d = HAL_DEVICE (i->data);
		apm_rescan_device (d);
	}

	g_slist_free (devices);
	return TRUE;
}

static gboolean 
read_from_apm (const char *apm_file, APMInfo *i)
{
	char *buf;
	gboolean ret;

	ret = FALSE;

	if ((buf = hal_util_get_string_from_file ("", apm_file)) == NULL)
		goto out;

	if (sscanf (buf, "%s %d.%d %x %x %x %x %d%% %d",
		    i->driver_version,
		    &i->version_major,
		    &i->version_minor,
		    &i->flags,
		    &i->ac_line_status,
		    &i->battery_status,
		    &i->battery_flags,
		    &i->battery_percentage,
		    &i->battery_time) != 9)
		goto out;

	ret = TRUE;

out:
	return ret;
}

enum
{
	BATTERY_HIGH     = 0,
	BATTERY_LOW      = 1,
	BATTERY_CRITICAL = 2,
	BATTERY_CHARGING = 3
};

static gboolean
battery_refresh (HalDevice *d, APMDevHandler *handler)
{
	const char *path;
	int remaining_percentage;
	int remaining_time;
	gboolean is_charging;
	gboolean is_discharging;
	HalDevice *computer;
	APMInfo i;

	path = hal_device_property_get_string (d, "linux.apm_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "Battery Bay");
	hal_device_property_set_string (d, "battery.type", "primary");
	hal_device_property_set_string (d, "battery.technology", "unknown");
	hal_device_property_set_string (d, "info.category", "battery");
	hal_device_add_capability (d, "battery");

	/* typical : 1.16ac 1.2 0x02 0x01 0x03 0x09 98% 88 min */

	read_from_apm (path, &i);

	/* we check this to know if there is a battery: 
	 * - if i.battery_percentage < 0: no battery   
	 * - if driver version starts with [B]: old style (pre-0.7), not supported -> no battery
	 * - if battery status == 0xff : no battery present
	 */
	if (i.battery_percentage < 0 || i.driver_version[0] == 'B' || i.battery_status == 0xff ) {
		device_property_atomic_update_begin ();
		hal_device_property_remove (d, "battery.is_rechargeable");
		hal_device_property_remove (d, "battery.rechargeable.is_charging");
		hal_device_property_remove (d, "battery.rechargeable.is_discharging");
		hal_device_property_remove (d, "battery.charge_level.unit");
		hal_device_property_remove (d, "battery.charge_level.current");
		/* Is this key really needed? We don't know the value of this key */
		hal_device_property_remove (d, "battery.charge_level.last_full");
		hal_device_property_remove (d, "battery.charge_level.design");
		hal_device_property_remove (d, "battery.charge_level.percentage");
		hal_device_property_remove (d, "battery.remaining_time");
		hal_device_property_set_bool (d, "battery.present", FALSE);
		device_property_atomic_update_end ();		
	} else {
		if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
		    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
			HAL_ERROR (("No computer object?"));
		} else {
			if (!hal_device_has_property(computer, "system.formfactor")) {
				hal_device_property_set_string (computer, "system.formfactor", "laptop");
			}
			else if (strcmp (hal_device_property_get_string (computer, "system.formfactor"), "laptop") != 0) {
				hal_device_property_set_string (computer, "system.formfactor", "laptop");
			}
		}
		
		device_property_atomic_update_begin ();
		hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);
		hal_device_property_set_bool (d, "battery.present", TRUE);
		hal_device_property_set_int (d, "battery.charge_level.current", i.battery_percentage);
		hal_device_property_set_string (d, "battery.charge_level.unit", "percent");

		hal_device_property_set_int (d, "battery.charge_level.design", 100);
		/* Is this key really needed? We don't know the value of this key */
		hal_device_property_set_int (d, "battery.charge_level.last_full", 100); 

		/* TODO: clean the logic below up; it appears my T41
		 * with 2.6.10-1.1110_FC4 and acpi=off always report
		 * BATTERY_CHARGING so look at ac_line_status
		 * instead..
		 */
		if (i.battery_status == BATTERY_CHARGING) {
			is_charging = TRUE;
			is_discharging = FALSE;
			remaining_time = 0; /* apm doesn't give info */
		} else {
			is_charging = FALSE;
			is_discharging = (i.ac_line_status == FALSE);
			/* apm returns time in minutes, not seconds */
			remaining_time = i.battery_time * 60;
		}

		/* set the time to discharge, or remove key for charging */
		if (remaining_time > 0) {
			/* switched from charging to discharging, set key */
			if (!is_charging && is_discharging && !hal_device_has_property(d,"battery.remaining_time")) {
				hal_device_property_set_int (d, "battery.remaining_time", remaining_time);
				interval_poll_round = 0;
			}
			/* after 30 seconds (15*APM_POLL_INTERVAL) set key */
			else if (interval_poll_round == 15) {
				hal_device_property_set_int (d, "battery.remaining_time", remaining_time);
				interval_poll_round = 0;
			}
			/* else: only increment counter and set no key to avoid needless events/changes
			   because APM produce with each read a new value for remaining time */
			else
				interval_poll_round++;
		} else {
			hal_device_property_remove (d, "battery.remaining_time");
		}

		/* set the correct charge states */
		hal_device_property_set_bool (d, "battery.rechargeable.is_charging", is_charging);
		hal_device_property_set_bool (d, "battery.rechargeable.is_discharging", is_discharging);

		/* set the percentage charge, easy. */
		remaining_percentage = i.battery_percentage;
	
		/* Only set keys if no error (signified with negative return value) */
		if (remaining_percentage >= 0 && remaining_percentage <= 100)
			hal_device_property_set_int (d, "battery.charge_level.percentage", remaining_percentage);
		else
			hal_device_property_remove (d, "battery.charge_level.percentage");

		device_property_atomic_update_end ();
	}

	return TRUE;
}

static gboolean
ac_adapter_refresh (HalDevice *d, APMDevHandler *handler)
{
	const char *path;
	APMInfo i;

	path = hal_device_property_get_string (d, "linux.apm_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "AC Adapter");
	hal_device_property_set_string (d, "info.category", "ac_adapter");
	hal_device_add_capability (d, "ac_adapter");

	read_from_apm(path, &i);

	if (i.ac_line_status)
		hal_device_property_set_bool (d, "ac_adapter.present", TRUE);
	else
		hal_device_property_set_bool (d, "ac_adapter.present", FALSE);

	return TRUE;
}

/** Scan the data structures exported by the kernel and add hotplug
 *  events for adding APM objects.
 *
 *  @param                      TRUE if, and only if, APM capabilities
 *                              were detected
 */
gboolean
apm_synthesize_hotplug_events (void)
{
	gboolean ret;
	HalDevice *computer;
	HotplugEvent *hotplug_event;

	ret = FALSE;

	if (!g_file_test ("/proc/apm", G_FILE_TEST_EXISTS))
		goto out;

	ret = TRUE;

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
	    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		goto out;
	}

	/* Set appropriate properties on the computer object */
	hal_device_property_set_string (computer, "power_management.type", "apm");

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_APM;
	g_strlcpy (hotplug_event->apm.apm_path, "/proc/apm", sizeof (hotplug_event->apm.apm_path));
	hotplug_event->apm.apm_type = APM_TYPE_BATTERY;
	hotplug_event_enqueue (hotplug_event);

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->type = HOTPLUG_EVENT_APM;
	g_strlcpy (hotplug_event->apm.apm_path, "/proc/apm", sizeof (hotplug_event->apm.apm_path));
	hotplug_event->apm.apm_type = APM_TYPE_AC_ADAPTER;
	hotplug_event_enqueue (hotplug_event);

#ifdef HAVE_GLIB_2_14
	g_timeout_add_seconds (APM_POLL_INTERVAL,
                               apm_poll,
                               NULL);
#else
	g_timeout_add (1000 * APM_POLL_INTERVAL,
		       apm_poll,
		       NULL);
#endif

out:
	return ret;
}

static HalDevice *
apm_generic_add (const gchar *apm_path, HalDevice *parent, APMDevHandler *handler)
{
	HalDevice *d;
	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.apm_path", apm_path);
	hal_device_property_set_int (d, "linux.apm_type", handler->apm_type);
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
apm_generic_compute_udi (HalDevice *d, APMDevHandler *handler)
{
	gchar udi[256];

	if (handler->apm_type == APM_TYPE_BATTERY ) {
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/apm_battery");

	} else if (handler->apm_type == APM_TYPE_AC_ADAPTER ) {
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/apm_ac_adapter");
	} else {
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/apm_%d",
				  hal_device_property_get_int (d, "info.category"));
	}

	hal_device_set_udi (d, udi);
	return TRUE;
}

static gboolean
apm_generic_remove (HalDevice *d, APMDevHandler *handler)
{
	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	return TRUE;
}

static APMDevHandler apmdev_handler_battery = { 
	.apm_type    = APM_TYPE_BATTERY,
	.add         = apm_generic_add,
	.compute_udi = apm_generic_compute_udi,
	.refresh     = battery_refresh,
	.remove      = apm_generic_remove
};

static APMDevHandler apmdev_handler_ac_adapter = { 
	.apm_type    = APM_TYPE_AC_ADAPTER,
	.add         = apm_generic_add,
	.compute_udi = apm_generic_compute_udi,
	.refresh     = ac_adapter_refresh,
	.remove      = apm_generic_remove
};

static APMDevHandler *apm_handlers[] = {
	&apmdev_handler_battery,
	&apmdev_handler_ac_adapter,
	NULL
};

void
hotplug_event_begin_add_apm (const gchar *apm_path, int apm_type, HalDevice *parent, void *end_token)
{
	guint i;

	HAL_INFO (("apm_add: apm_path=%s apm_type=%d, parent=0x%08x", apm_path, apm_type, parent));

	for (i = 0; apm_handlers [i] != NULL; i++) {
		APMDevHandler *handler;

		handler = apm_handlers[i];
		if (handler->apm_type == apm_type) {
			HalDevice *d;

			d = handler->add (apm_path, parent, handler);
			if (d == NULL) {
				/* didn't find anything - thus, ignore this hotplug event */
				hotplug_event_end (end_token);
				goto out;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_APM);

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
hotplug_event_begin_remove_apm (const gchar *apm_path, int apm_type, void *end_token)
{
	guint i;
	HalDevice *d;

	HAL_INFO (("apm_rem: apm_path=%s apm_type=%d", apm_path, apm_type));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.apm_path", apm_path);
	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device with apm path %s - not found", apm_path));
		goto out;
	}

	for (i = 0; apm_handlers [i] != NULL; i++) {
		APMDevHandler *handler;

		handler = apm_handlers[i];
		if (handler->apm_type == apm_type) {
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
apm_rescan_device (HalDevice *d)
{
	guint i;
	gboolean ret;
	int apm_type;

	ret = FALSE;

	apm_type = hal_device_property_get_int (d, "linux.apm_type");

	for (i = 0; apm_handlers [i] != NULL; i++) {
		APMDevHandler *handler;

		handler = apm_handlers[i];
		if (handler->apm_type == apm_type) {
			ret = handler->refresh (d, handler);
			goto out;
		}
	}

	HAL_WARNING (("Didn't find a rescan handler for udi %s", hal_device_get_udi (d)));

out:
	return ret;
}

HotplugEvent *
apm_generate_add_hotplug_event (HalDevice *d)
{
	int apm_type;
	const char *apm_path;
	HotplugEvent *hotplug_event;

	apm_path = hal_device_property_get_string (d, "linux.apm_path");
	apm_type = hal_device_property_get_int (d, "linux.apm_type");

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_APM;
	g_strlcpy (hotplug_event->apm.apm_path, apm_path, sizeof (hotplug_event->apm.apm_path));
	hotplug_event->apm.apm_type = apm_type;
	return hotplug_event;
}

HotplugEvent *
apm_generate_remove_hotplug_event (HalDevice *d)
{
	int apm_type;
	const char *apm_path;
	HotplugEvent *hotplug_event;

	apm_path = hal_device_property_get_string (d, "linux.apm_path");
	apm_type = hal_device_property_get_int (d, "linux.apm_type");

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_APM;
	g_strlcpy (hotplug_event->apm.apm_path, apm_path, sizeof (hotplug_event->apm.apm_path));
	hotplug_event->apm.apm_type = apm_type;
	return hotplug_event;
}
