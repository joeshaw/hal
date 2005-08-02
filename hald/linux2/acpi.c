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

#include "../device_info.h"
#include "../logger.h"
#include "../hald_dbus.h"
#include "../util.h"

#include "osspec_linux.h"

#include "acpi.h"
#include "hotplug.h"

enum {
	ACPI_TYPE_BATTERY,
	ACPI_TYPE_PROCESSOR,
	ACPI_TYPE_FAN,
	ACPI_TYPE_AC_ADAPTER,
	ACPI_TYPE_BUTTON
};

#define ACPI_POLL_INTERVAL 30000


typedef struct ACPIDevHandler_s
{
	int acpi_type;
	HalDevice *(*add) (const gchar *acpi_path, HalDevice *parent, struct ACPIDevHandler_s *handler);
	gboolean (*compute_udi) (HalDevice *d, struct ACPIDevHandler_s *handler);
	gboolean (*remove) (HalDevice *d, struct ACPIDevHandler_s *handler);
	gboolean (*refresh) (HalDevice *d, struct ACPIDevHandler_s *handler);
} ACPIDevHandler;

static void
battery_refresh_poll (HalDevice *d)
{
	const char *path;
	const char *reporting_unit;
	int reporting_current;
	int reporting_lastfull;
	int reporting_rate;
	int mwh_current;
	int mwh_lastfull;
	int mwh_rate;
	int voltage_current;
	int voltage_design;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		goto out;

	hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_charging", path, 
					  "state", "charging state", 0, "charging", TRUE);
	hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_discharging", path, 
					  "state", "charging state", 0, "discharging", TRUE);
	/* 
	 * we'll use the .reporting prefix as we don't know
	 * if this data is energy (mWh) or unit enery (mAh)
	 */
	hal_util_set_int_elem_from_file (d, "battery.reporting.current", path, 
					 "state", "remaining capacity", 0, 10, TRUE);
	hal_util_set_int_elem_from_file (d, "battery.reporting.rate", path, 
					 "state", "present rate", 0, 10, TRUE);
	hal_util_set_int_elem_from_file (d, "battery.reporting.last_full", path, 
					 "info", "last full capacity", 0, 10, TRUE);
	/* 
	 * we'll need this if we need to convert mAh to mWh, but we should
	 * also update it here anyway as the value will have changed
	 */
	hal_util_set_int_elem_from_file (d, "battery.voltage.current", path,
					 "state", "present voltage", 0, 10, TRUE);
	/* get all the data we know */
	reporting_unit = hal_device_property_get_string (d, 
					"battery.reporting.unit");
	if (reporting_unit == NULL)
		goto out;

	reporting_current = hal_device_property_get_int (d, 
					"battery.reporting.current");
	reporting_lastfull = hal_device_property_get_int (d, 
					"battery.reporting.last_full");
	reporting_rate = hal_device_property_get_int (d, 
					"battery.reporting.rate");

	/* 
	 * we are converting the unknown units into mWh because of ACPI's nature
	 * of not having a standard "energy" unit. 
	 *
	 * full details here: http://bugzilla.gnome.org/show_bug.cgi?id=309944
	 */
	if (strcmp (reporting_unit, "mWh") == 0) {
		/* units do not need conversion */
		mwh_current = reporting_current;
		mwh_lastfull = reporting_lastfull;
		mwh_rate = reporting_rate;
	} else if (strcmp (reporting_unit, "mAh") == 0) {
		voltage_current = hal_device_property_get_int (d, 
					"battery.voltage.current");
		voltage_design = hal_device_property_get_int (d, 
					"battery.voltage.design");
		/* 
		 * we really want battery.voltage.last_full, but ACPI doesn't provide it 
		 * we can use battery.voltage.last_full ~= battery.voltage.design
		 */
		mwh_current = reporting_current * voltage_current;
		mwh_lastfull = reporting_lastfull * voltage_design;
		mwh_rate = reporting_rate * voltage_current;
	} else {
		/* 
		 * report as 0 so we get some bug reports on types other than mWh 
		 * and mAh, although I suspect these would cover 99.99% of cases.
		 */
		mwh_current = 0;
		mwh_lastfull = 0;
		mwh_rate = 0;
	}

	/*
	* Set these new mWh only keys. 
	*/
	hal_device_property_set_int (d, "battery.charge_level.current", mwh_current);
	hal_device_property_set_int (d, "battery.charge_level.last_full", mwh_lastfull);
	hal_device_property_set_int (d, "battery.charge_level.rate", mwh_rate);

	hal_device_property_set_int (d, "battery.remaining_time", 
				     util_compute_time_remaining (
					     d->udi,
					     mwh_rate,
					     mwh_current,
					     mwh_lastfull,
					     hal_device_property_get_bool (d, "battery.rechargeable.is_discharging"),
					     hal_device_property_get_bool (d, "battery.rechargeable.is_charging")));

	hal_device_property_set_int (d, "battery.charge_level.percentage", 
				     util_compute_percentage_charge (
					     d->udi,
					     mwh_current,
					     mwh_lastfull));
out:
	;
}

static gboolean
battery_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;
	int reporting_design;
	int reporting_warning;
	int reporting_low;
	int reporting_gran1;
	int reporting_gran2;
	int voltage_design;
	
	const char *reporting_unit;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "Battery Bay");
	hal_device_property_set_string (d, "battery.type", "primary");
	hal_device_property_set_string (d, "info.category", "battery");
	hal_device_add_capability (d, "battery");

	/* Since we're using reuse==TRUE make sure we get fresh data for first read */
	hal_util_grep_discard_existing_data ();

	hal_util_set_bool_elem_from_file (d, "battery.present", path, "info", "present", 0, "yes", TRUE);
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
		hal_device_property_remove (d, "battery.charge_level.percentage");
		hal_device_property_remove (d, "battery.charge_level.last_full");
		hal_device_property_remove (d, "battery.charge_level.design");
		hal_device_property_remove (d, "battery.charge_level.capacity_state");
		hal_device_property_remove (d, "battery.charge_level.warning");
		hal_device_property_remove (d, "battery.charge_level.low");
		hal_device_property_remove (d, "battery.charge_level.granularity_1");
		hal_device_property_remove (d, "battery.charge_level.granularity_2");
		hal_device_property_remove (d, "battery.voltage.unit");
		hal_device_property_remove (d, "battery.voltage.design");
		hal_device_property_remove (d, "battery.voltage.current");
		hal_device_property_remove (d, "battery.alarm.unit");
		hal_device_property_remove (d, "battery.alarm.design");
		hal_device_property_remove (d, "battery.reporting.current");
		hal_device_property_remove (d, "battery.reporting.last_full");
		hal_device_property_remove (d, "battery.reporting.design");
		hal_device_property_remove (d, "battery.reporting.rate");
		hal_device_property_remove (d, "battery.reporting.warning");
		hal_device_property_remove (d, "battery.reporting.low");
		hal_device_property_remove (d, "battery.reporting.granularity_1");
		hal_device_property_remove (d, "battery.reporting.granularity_2");
		device_property_atomic_update_end ();		
	} else {
		device_property_atomic_update_begin ();

		/* So, it's pretty expensive to read from
		 * /proc/acpi/battery/BAT%d/[info|state] so don't read
		 * static data that won't change
		 */
		if (!hal_device_has_property (d, "battery.vendor")) {
			hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info", "OEM info", 0, TRUE);
			hal_util_set_string_elem_from_file (d, "battery.model", path, "info", "model number", 0, TRUE);
			hal_util_set_string_elem_from_file (d, "battery.serial", path, "info", 
							    "serial number", 0, TRUE);
			hal_util_set_string_elem_from_file (d, "battery.technology", path, "info", 
							    "battery type", 0, TRUE);
			hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info", "OEM info", 0, TRUE);

			/* 
			 * we'll use the .reporting prefix as we don't know
			 * if this data is energy (mWh) or unit enery (mAh)
			 */
			hal_util_set_string_elem_from_file (d, "battery.reporting.unit", path, "info", 
							    "design capacity", 1, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.reporting.last_full", path, 
							 "info", "last full capacity", 0, 10, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.reporting.design", path, 
							 "info", "design capacity", 0, 10, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.reporting.warning", path,
							 "info", "design capacity warning", 0, 10, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.reporting.low", path,
							 "info", "design capacity low", 0, 10, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.reporting.granularity_1", path,
							 "info", "capacity granularity 1", 0, 10, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.reporting.granularity_2", path,
							 "info", "capacity granularity 2", 0, 10, TRUE);
			/* 
			 * we'll need this is we want to convert mAh to mWh
			 */
			hal_util_set_string_elem_from_file (d, "battery.voltage.unit", path, "info",
							    "design voltage", 1, TRUE);
			hal_util_set_int_elem_from_file (d, "battery.voltage.design", path,
							 "info", "design voltage", 0, 10, TRUE);
			/* 
			 * Convert the mWh or mAh units into mWh...
			 * We'll do as many as we can here as the values
			 * are not going to change.
			 * We'll set the correct unit (or unknown) also.
			 */
			reporting_unit = hal_device_property_get_string (d, 
					"battery.reporting.unit");
			reporting_design = hal_device_property_get_int (d, 
					"battery.reporting.design");
			reporting_warning = hal_device_property_get_int (d, 
					"battery.reporting.warning");
			reporting_low = hal_device_property_get_int (d, 
					"battery.reporting.low");
			reporting_gran1 = hal_device_property_get_int (d, 
					"battery.reporting.granularity_1");
			reporting_gran2 = hal_device_property_get_int (d, 
					"battery.reporting.granularity_2");
			if (strcmp (reporting_unit, "mWh") == 0) {
				/* do not scale */
				hal_device_property_set_int (d, 
					"battery.charge_level.design", reporting_design);
				hal_device_property_set_int (d, 
					"battery.charge_level.warning", reporting_warning);
				hal_device_property_set_int (d, 
					"battery.charge_level.low", reporting_low);
				hal_device_property_set_int (d, 
					"battery.charge_level.granularity_1", reporting_gran1);
				hal_device_property_set_int (d, 
					"battery.charge_level.granularity_2", reporting_gran2);

				/* set unit */
				hal_device_property_set_string (d, 
					"battery.charge_level.design", "mWh");
			} else if (strcmp (reporting_unit, "mAh") == 0) {
				voltage_design = hal_device_property_get_int (d, 
					"battery.voltage.design");

				/* scale by factor battery.voltage.design */
				hal_device_property_set_int (d, 
					"battery.charge_level.design", 
					reporting_design * voltage_design);
				hal_device_property_set_int (d, 
					"battery.charge_level.warning", 
					reporting_warning * voltage_design);
				hal_device_property_set_int (d, 
					"battery.charge_level.low", 
					reporting_low * voltage_design);
				hal_device_property_set_int (d, 
					"battery.charge_level.granularity_1", 
					reporting_gran1 * voltage_design);
				hal_device_property_set_int (d, 
					"battery.charge_level.granularity_2", 
					reporting_gran2 * voltage_design);

				/* set unit */
				hal_device_property_set_string (d, 
					"battery.charge_level.design", 
					"mWh"); /* not mAh! */
			} else {
				hal_device_property_set_int (d, 
					"battery.charge_level.design", 0);

				/* set "Unknown ACPI Unit" unit so we can debug */
				hal_device_property_set_string (d, 
					"battery.charge_level.design",
					"Unknown ACPI Unit");
			}
			/* set alarm if present */
			if (hal_util_set_int_elem_from_file (d, "battery.alarm.design", path,
							     "alarm", "alarm", 0, 10, TRUE))
				hal_util_set_string_elem_from_file (d, "battery.alarm.unit", path, "alarm",
								    "alarm", 1, TRUE);
		}

		hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);

		battery_refresh_poll (d);

		device_property_atomic_update_end ();
	}

	return TRUE;
}

static gboolean
processor_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "Processor");
	hal_device_property_set_string (d, "info.category", "processor");
	hal_device_add_capability (d, "processor");
	hal_util_set_int_elem_from_file (d, "processor.number", path, 
					 "info", "processor id", 0, 10, FALSE);
	hal_util_set_bool_elem_from_file (d, "processor.can_throttle", path, 
					  "info", "throttling control", 0, "yes", FALSE);
	return TRUE;
}

static gboolean
fan_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	hal_device_property_set_string (d, "info.product", "Fan");
	hal_device_property_set_string (d, "info.category", "fan");
	hal_device_add_capability (d, "fan");
	hal_util_set_bool_elem_from_file (d, "fan.enabled", path, 
					  "state", "status", 0, "on", FALSE);
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
	hal_device_property_set_string (d, "info.category", "ac_adapter");
	hal_device_add_capability (d, "ac_adapter");
	hal_util_set_bool_elem_from_file (d, "ac_adapter.present", path, "state", "state", 0, "on-line", FALSE);

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

	hal_device_property_set_string (d, "button.type", button_type);

	if (strcmp (button_type, "power") == 0)   
		hal_device_property_set_string (d, "info.product", "Power Button");
	else if (strcmp (button_type, "lid") == 0)   
		hal_device_property_set_string (d, "info.product", "Lid Switch");
	else if (strcmp (button_type, "sleep") == 0)   
		hal_device_property_set_string (d, "info.product", "Sleep Button");

	hal_device_property_set_string (d, "info.category", "button");
	hal_device_add_capability (d, "button");
	if (!hal_util_set_bool_elem_from_file (d, "button.state.value", path, "state", "state", 0, "closed", FALSE)) {
		hal_device_property_set_bool (d, "button.has_state", FALSE);
	} else {
		hal_device_property_set_bool (d, "button.has_state", TRUE);
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
		hotplug_event->action = HOTPLUG_ACTION_ADD;
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


static gboolean
acpi_poll (gpointer data)
{
	GSList *i;
	GSList *devices;

	devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "battery.type",
								    "primary");
	for (i = devices; i != NULL; i = g_slist_next (i)) {
		HalDevice *d;
		
		d = HAL_DEVICE (i->data);
		if (hal_device_has_property (d, "linux.acpi_type") &&
		    hal_device_property_get_bool (d, "battery.present")) {
			hal_util_grep_discard_existing_data ();
			device_property_atomic_update_begin ();
			battery_refresh_poll (d);
			device_property_atomic_update_end ();		
		}
	}

	return TRUE;
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

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
	    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		goto out;
	}

	/* Set appropriate properties on the computer object */
	hal_device_property_set_bool (computer, "power_management.is_enabled", TRUE);
	hal_device_property_set_string (computer, "power_management.type", "acpi");
	hal_util_set_string_elem_from_file (computer, "power_management.acpi.linux.version", 
					    "/proc/acpi", "info", "version", 0, FALSE);

	/* collect batteries */
	snprintf (path, sizeof (path), "%s/acpi/battery", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_BATTERY);

	/* collect processors */
	snprintf (path, sizeof (path), "%s/acpi/processor", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_PROCESSOR);

	/* collect fans */
	snprintf (path, sizeof (path), "%s/acpi/fan", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_FAN);

	/* collect AC adapters */
	snprintf (path, sizeof (path), "%s/acpi/ac_adapter", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_AC_ADAPTER);

	/* collect buttons */
	snprintf (path, sizeof (path), "%s/acpi/button/lid", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_BUTTON);
	snprintf (path, sizeof (path), "%s/acpi/button/power", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_BUTTON);
	snprintf (path, sizeof (path), "%s/acpi/button/sleep", get_hal_proc_path ());
	acpi_synthesize (path, ACPI_TYPE_BUTTON);

	/* setup timer for things that we need to poll */
	g_timeout_add (ACPI_POLL_INTERVAL,
		       acpi_poll,
		       NULL);

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
	return TRUE;
}


static ACPIDevHandler acpidev_handler_battery = { 
	.acpi_type   = ACPI_TYPE_BATTERY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = battery_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_processor = { 
	.acpi_type   = ACPI_TYPE_PROCESSOR,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = processor_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_fan = { 
	.acpi_type   = ACPI_TYPE_FAN,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = fan_refresh,
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
	&acpidev_handler_processor,
	&acpidev_handler_fan,
	&acpidev_handler_button,
	&acpidev_handler_ac_adapter,
	NULL
};

static void 
acpi_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", d->udi));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
acpi_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Remove callouts completed udi=%s", d->udi));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	hotplug_event_end (end_token);
}

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
			di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
			di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

			
			/* Compute UDI */
			if (!handler->compute_udi (d, handler)) {
				hal_device_store_remove (hald_get_tdl (), d);
				hotplug_event_end (end_token);
				goto out;
			}

			/* Run callouts */
			hal_util_callout_device_add (d, acpi_callouts_add_done, end_token, NULL);
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
				hal_util_callout_device_remove (d, acpi_callouts_remove_done, end_token, NULL);
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
	hotplug_event->action = HOTPLUG_ACTION_ADD;
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
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, acpi_path, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = acpi_type;
	return hotplug_event;
}
