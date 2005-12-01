/***************************************************************************
 * CVSID: $Id$
 *
 * Copyright (C) 2005 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2005 David Zeuthen, Red Hat Inc., <davidz@redhat.com>
 * Copyright (C) 2005 Danny Kukawka, <danny.kukawka@web.de>
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
	ACPI_TYPE_TOSHIBA_DISPLAY,
	ACPI_TYPE_ASUS_DISPLAY,
	ACPI_TYPE_IBM_DISPLAY,
	ACPI_TYPE_PANASONIC_DISPLAY,
	ACPI_TYPE_SONY_DISPLAY,
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

/** Just sets the ac_adapter.present key when called
 *
 *  @param	d		valid ac_adaptor HalDevice
 */
static void
ac_adapter_refresh_poll (HalDevice *d)
{
	const char *path;
	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return;
	hal_util_set_bool_elem_from_file (d, "ac_adapter.present", path, "state", "state", 0, "on-line", FALSE);
}

static void
battery_refresh_poll (HalDevice *d)
{
	const char *path;
	const char *reporting_unit;
	int reporting_current;
	int reporting_lastfull;
	int reporting_rate;
	int normalised_current;
	int normalised_lastfull;
	int normalised_rate;
	int design_voltage;
	int voltage;
	int remaining_time;
	int remaining_percentage;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return;

	hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_charging", path,
					  "state", "charging state", 0, "charging", TRUE);
	hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_discharging", path,
					  "state", "charging state", 0, "discharging", TRUE);
	/*
	 * we'll use the .reporting prefix as we don't know
	 * if this data is energy (mWh) or unit enery (mAh)
	 */
	if (!hal_util_set_int_elem_from_file (d, "battery.reporting.current", path,
					      "state", "remaining capacity", 0, 10, TRUE))
		hal_device_property_set_int (d, "battery.reporting.current", 0);
	if (!hal_util_set_int_elem_from_file (d, "battery.reporting.rate", path,
					      "state", "present rate", 0, 10, TRUE))
		hal_device_property_set_int (d, "battery.reporting.rate", 0);
	/*
	 * we'll need this if we need to convert mAh to mWh, but we should
	 * also update it here anyway as the value will have changed
	 */
	hal_util_set_int_elem_from_file (d, "battery.voltage.current", path,
					 "state", "present voltage", 0, 10, TRUE);
	/* get all the data we know */
	reporting_unit = hal_device_property_get_string (d,
					"battery.reporting.unit");
	reporting_current = hal_device_property_get_int (d,
					"battery.reporting.current");
	reporting_lastfull = hal_device_property_get_int (d,
					"battery.reporting.last_full");
	reporting_rate = hal_device_property_get_int (d,
					"battery.reporting.rate");

	/*
	 * We are converting the unknown units into mWh because of ACPI's nature
	 * of not having a standard "energy" unit.
	 *
	 * full details here: http://bugzilla.gnome.org/show_bug.cgi?id=309944
	 */
	if (reporting_unit && strcmp (reporting_unit, "mWh") == 0) {
		/* units do not need conversion */
		normalised_current = reporting_current;
		normalised_lastfull = reporting_lastfull;
		normalised_rate = reporting_rate;
	} else if (reporting_unit && strcmp (reporting_unit, "mAh") == 0) {
		/* convert mAh to mWh by multiplying by voltage.  due to the
		 * general wonkiness of ACPI implementations, this is a lot
		 * harder than it should have to be...
		 */

		design_voltage = hal_device_property_get_int (d, "battery.voltage.design");
		voltage = hal_device_property_get_int (d, "battery.voltage.current");

		/* Just in case we don't get design voltage information, then
		 * this will pretend that we have 1mV.  This degrades our
		 * ability to report accurate times on multi-battery systems
		 * but will always prevent negative charge levels and allow
		 * accurate reporting on single-battery systems.
		 */
		if (design_voltage <= 0)
			design_voltage = 1;

		/* If the current voltage is unknown or greater than design,
		 * then use design voltage.
		 */
		if (voltage <= 0 || voltage > design_voltage)
			voltage = design_voltage;

		normalised_current = reporting_current * voltage;
		normalised_lastfull = reporting_lastfull * voltage;
		normalised_rate = reporting_rate * voltage;
	} else {
		/*
		 * handle as if mWh, which is the most common case.
		 */
		normalised_current = reporting_current;
		normalised_lastfull = reporting_lastfull;
		normalised_rate = reporting_rate;
	}

	/*
	 * Set the normalised keys.
	 */
	if (normalised_current < 0)
		normalised_current = 0;
	if (normalised_lastfull < 0)
		normalised_lastfull = 0;
	if (normalised_rate < 0)
		normalised_rate = 0;

	/*
	 * Some laptops report current charge much larger than
	 * full charge when at 100%.  Clamp back down to 100%.
	 */
	if (normalised_current > normalised_lastfull)
	  	normalised_current = normalised_lastfull;

	hal_device_property_set_int (d, "battery.charge_level.current", normalised_current);
	hal_device_property_set_int (d, "battery.charge_level.last_full", normalised_lastfull);
	hal_device_property_set_int (d, "battery.charge_level.rate", normalised_rate);

	remaining_time = util_compute_time_remaining (d->udi, normalised_rate, normalised_current, normalised_lastfull,
				hal_device_property_get_bool (d, "battery.rechargeable.is_discharging"),
				hal_device_property_get_bool (d, "battery.rechargeable.is_charging"),
				hal_device_property_get_bool (d, "battery.remaining_time.calculate_per_time"));
	remaining_percentage = util_compute_percentage_charge (d->udi, normalised_current, normalised_lastfull);
	/*
	 * Only set keys if no error (signified with negative return value)
	 * Scrict checking is needed to ensure that the values presented by HAL
	 * are 100% acurate.
	 */

	if (remaining_time > 0)
		hal_device_property_set_int (d, "battery.remaining_time", remaining_time);
	else
		hal_device_property_remove (d, "battery.remaining_time");

	if (remaining_percentage > 0)
		hal_device_property_set_int (d, "battery.charge_level.percentage", remaining_percentage);
	else
		hal_device_property_remove (d, "battery.charge_level.percentage");
}

/** Recalculates the battery.reporting.last_full key as this may drift
 *  over time.
 *
 *  @param	data		Ignored
 *  @return			TRUE if we updated values
 *
 *  @note	This is called 120x less often than battery_refresh_poll
 */
static gboolean
battery_poll_infrequently (gpointer data) {
	
	GSList *i;
	GSList *battery_devices;
	HalDevice *d;
	const char *path;

	battery_devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "battery.type",
								    "primary");
	
	for (i = battery_devices; i != NULL; i = g_slist_next (i)) {
		d = HAL_DEVICE (i->data);
		if (hal_device_has_property (d, "linux.acpi_type") &&
		    hal_device_property_get_bool (d, "battery.present")) {
			hal_util_grep_discard_existing_data ();
			device_property_atomic_update_begin ();
			path = hal_device_property_get_string (d, "linux.acpi_path");
			if (path != NULL)
				hal_util_set_int_elem_from_file (d, "battery.reporting.last_full", path,
								 "info", "last full capacity", 0, 10, TRUE);
			device_property_atomic_update_end ();		
		}
	}

	g_slist_free (battery_devices);
	return TRUE;
}


/** Fallback polling method to refresh battery objects is plugged in
 *
 *  @return			TRUE
 *
 *  @note	This just calls battery_refresh_poll for each battery
 */
static gboolean
acpi_poll_battery (void)
{
	GSList *i;
	GSList *battery_devices;
	HalDevice *d;

	battery_devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "battery.type",
								    "primary");
	/*
	 * These forced updates take care of really broken BIOS's that don't
	 * emit batt events.
	 */
	for (i = battery_devices; i != NULL; i = g_slist_next (i)) {
		d = HAL_DEVICE (i->data);
		if (hal_device_has_property (d, "linux.acpi_type") &&
		    hal_device_property_get_bool (d, "battery.present")) {
			hal_util_grep_discard_existing_data ();
			device_property_atomic_update_begin ();
			battery_refresh_poll (d);
			device_property_atomic_update_end ();		
		}
	}

	g_slist_free (battery_devices);
	return TRUE;
}

/** Fallback polling method to detect if the ac_adaptor is plugged in
 *
 *  @return			TRUE
 *
 *  @note	This just calls ac_adapter_refresh_poll for each ac_adapter
 */
static gboolean
acpi_poll_acadap (void)
{
	GSList *i;
	GSList *acadap_devices;
	HalDevice *d;

	acadap_devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
								    "info.category",
								    "ac_adapter");
	/*
	 * These forced updates take care of really broken BIOS's that don't
	 * emit acad events.
	 */
	for (i = acadap_devices; i != NULL; i = g_slist_next (i)) {
		d = HAL_DEVICE (i->data);
		if (hal_device_has_property (d, "linux.acpi_type")) {
			hal_util_grep_discard_existing_data ();
			device_property_atomic_update_begin ();
			ac_adapter_refresh_poll (d);
			device_property_atomic_update_end ();		
		}
	}
	g_slist_free (acadap_devices);
	return TRUE;
}

/** Fallback polling method called every minute.
 *
 *  @param	data		Ignored
 *  @return			TRUE
 *
 *  @note	This just forces a poll refresh for *every* ac_adaptor
 *		and primary battery in the system.
 */
static gboolean
acpi_poll (gpointer data)
{
	/*
	 * These forced updates take care of really broken BIOS's that don't
	 * emit acad or acadapt events.
	 */
	acpi_poll_acadap ();
	acpi_poll_battery ();
	return TRUE;
}

static gboolean
ac_adapter_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;
	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	device_property_atomic_update_begin ();
	/* only set up device new if really needed */
	if (!hal_device_has_capability (d, "ac_adapter")){
		hal_device_property_set_string (d, "info.product", "AC Adapter");
		hal_device_property_set_string (d, "info.category", "ac_adapter");
		hal_device_add_capability (d, "ac_adapter");
	}
	/* get .present value */
	ac_adapter_refresh_poll (d);
	device_property_atomic_update_end ();

	/* refresh last full if ac plugged in/out */
	battery_poll_infrequently (NULL);

	/*
	 * Refresh all the data for each battery.
	 * This is required as the batteries may go from charging->
	 * discharging, or charged -> discharging state, and we don't
	 * want to wait for the next random refresh from acpi_poll.
	 */
	acpi_poll_battery ();
	
	return TRUE;
}

/** Removes all the possible battery.* keys.
 *
 *  @param	d		Valid battery HalDevice
 *
 *  @note	Removing a key that doesn't exist is OK.
 */
static void
battery_refresh_remove (HalDevice *d)
{
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
	hal_device_property_remove (d, "battery.charge_level.rate");
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
	hal_device_property_remove (d, "battery.reporting.unit");
	hal_device_property_remove (d, "battery.remaining_time");
}

/** Adds all the possible battery.* keys and does coldplug (slowpath)
 *  type calculations.
 *
 *  @param	d		Valid battery HalDevice
 */
static gboolean
battery_refresh_add (HalDevice *d, const char *path)
{
	int reporting_design;
	int reporting_warning;
	int reporting_low;
	int reporting_gran1;
	int reporting_gran2;
	int voltage_design;

	const char *reporting_unit;

	hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info",
					    "OEM info", 0, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.model", path, "info",
					    "model number", 0, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.serial", path, "info",
					    "serial number", 0, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.technology", path, "info",
					    "battery type", 0, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info",
					    "OEM info", 0, TRUE);

	/*
	 * we'll use the .reporting prefix as we don't know
	 * if this data is energy (mWh) or unit enery (mAh)
	 */
	hal_util_set_string_elem_from_file (d, "battery.reporting.unit", path,
					 "info", "design capacity", 1, TRUE);
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
	reporting_unit = hal_device_property_get_string (d, "battery.reporting.unit");
	reporting_design = hal_device_property_get_int (d, "battery.reporting.design");
	reporting_warning = hal_device_property_get_int (d, "battery.reporting.warning");
	reporting_low = hal_device_property_get_int (d, "battery.reporting.low");
	reporting_gran1 = hal_device_property_get_int (d, "battery.reporting.granularity_1");
	reporting_gran2 = hal_device_property_get_int (d, "battery.reporting.granularity_2");

	if (reporting_unit && strcmp (reporting_unit, "mWh") == 0) {
		/* do not scale */
		hal_device_property_set_int (d, "battery.charge_level.design", reporting_design);
		hal_device_property_set_int (d, "battery.charge_level.warning", reporting_warning);
		hal_device_property_set_int (d, "battery.charge_level.low", reporting_low);
		hal_device_property_set_int (d, "battery.charge_level.granularity_1", reporting_gran1);
		hal_device_property_set_int (d, "battery.charge_level.granularity_2", reporting_gran2);

		/* set unit */
		hal_device_property_set_string (d, "battery.charge_level.unit", "mWh");
	} else if (reporting_unit && strcmp (reporting_unit, "mAh") == 0) {
		voltage_design = hal_device_property_get_int (d, "battery.voltage.design");
	
		/* If design voltage is unknown, use 1mV. */
		if (voltage_design <= 0)
			voltage_design = 1;

		/* scale by factor battery.voltage.design */
		hal_device_property_set_int (d, "battery.charge_level.design",
			reporting_design * voltage_design);
		hal_device_property_set_int (d, "battery.charge_level.warning",
			reporting_warning * voltage_design);
		hal_device_property_set_int (d, "battery.charge_level.low",
			reporting_low * voltage_design);
		hal_device_property_set_int (d, "battery.charge_level.granularity_1",
			reporting_gran1 * voltage_design);
		hal_device_property_set_int (d, "battery.charge_level.granularity_2",
			reporting_gran2 * voltage_design);

		/* set unit */
		hal_device_property_set_string (d, "battery.charge_level.unit",
			"mWh"); /* not mAh! */
	} else {
		/*
		 * Some ACPI BIOS's do not report the unit,
		 * so we'll assume they are mWh.
		 * We will report the guessing with the
		 * battery.charge_level.unit key.
		 */
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

		/* set "Unknown ACPI Unit" unit so we can debug */
		HAL_WARNING (("Unknown ACPI Unit!"));
		hal_device_property_set_string (d, "battery.charge_level.unit",
			"unknown");
	}

	/* set alarm if present */
	if (hal_util_set_int_elem_from_file (d, "battery.alarm.design", path,
					     "alarm", "alarm", 0, 10, TRUE))
		hal_util_set_string_elem_from_file (d, "battery.alarm.unit", path, "alarm",
						    "alarm", 1, TRUE);

	/* we are assuming a laptop battery is rechargeable */
	hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);
	return TRUE;
}

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

	/* Since we're using reuse==TRUE make sure we get fresh data for first read */
	hal_util_grep_discard_existing_data ();

	hal_util_set_bool_elem_from_file (d, "battery.present", path, "state", "present", 0, "yes", TRUE);
	if (!hal_device_property_get_bool (d, "battery.present")) {
		/* remove battery.* tags as battery not present */
		device_property_atomic_update_begin ();
		battery_refresh_remove (d);
		device_property_atomic_update_end ();		
	} else {
		/* battery is present */
		device_property_atomic_update_begin ();

		/* So, it's pretty expensive to read from
		 * /proc/acpi/battery/BAT%d/[info|state] so don't read
		 * static data that won't change
		 */
		if (!hal_device_has_property (d, "battery.vendor")) {
			/* battery has no information, so coldplug */
			battery_refresh_add (d, path);
		}

		/* fill in the fast-path refresh values */
		battery_refresh_poll (d);

		device_property_atomic_update_end ();

		/* poll ac adapter for machines which never give ACAP events */
		acpi_poll_acadap ();
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

/*
 * The different laptop_panel ACPI handling code is below. When a nice sysfs
 * interface comes along, we'll use that instead of these hacks.
 * Using /proc/acpi/video does not work, this method is much more reliable.
 */
static gboolean
laptop_panel_refresh (HalDevice *d, ACPIDevHandler *handler)
{
	const char *path;
	int acpi_type;
	char *type = NULL;
	char *desc = NULL;
	int br_levels = -1;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	acpi_type = hal_device_property_get_int (d, "linux.acpi_type");
	hal_device_property_set_string (d, "info.category", "laptop_panel");
	if (acpi_type == ACPI_TYPE_TOSHIBA_DISPLAY) {
		type = "toshiba";
		desc = "Toshiba LCD Panel";
		br_levels = 8;
	} else if (acpi_type == ACPI_TYPE_ASUS_DISPLAY) {
		type = "asus";
		desc = "ASUS LCD Panel";
		br_levels = 16;
	} else if (acpi_type == ACPI_TYPE_IBM_DISPLAY) {
		type = "ibm";
		desc = "IBM LCD Panel";
		br_levels = 8;
	} else if (acpi_type == ACPI_TYPE_PANASONIC_DISPLAY) {
		type = "panasonic";
		desc = "Panasonic LCD Panel";
		br_levels = 16;
	} else if (acpi_type == ACPI_TYPE_SONY_DISPLAY) {
		type = "sony";
		desc = "Sony LCD Panel";
		br_levels = 8;
	} else {
		type = "unknown";
		desc = "Unknown LCD Panel";
		br_levels = 0;
		HAL_WARNING (("acpi_type not recognised!"));
	}

	hal_device_property_set_string (d, "info.product", desc);
	/*
	 * We will set laptop_panel.acpi_method as the scripts can use this to
	 * determine the set/get parameters.
	 */
	hal_device_property_set_string (d, "laptop_panel.acpi_method", type);
	/*
	 * We can set laptop_panel.num_levels as it will not change, and allows us
	 * to work out the percentage in the scripts.
	 */
	hal_device_property_set_int (d, "laptop_panel.num_levels", br_levels);
	hal_device_add_capability (d, "laptop_panel");
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

/** Synthesizes a *specific* acpi object.
 *
 *  @param	fullpath	The ACPI path, e.g. "/proc/acpi/battery/BAT1"
 *  @param	acpi_type	The type of device, e.g. ACPI_TYPE_BATTERY
 */
static void
acpi_synthesize_item (const gchar *fullpath, int acpi_type)
{
	HotplugEvent *hotplug_event;
	HAL_INFO (("Processing %s", fullpath));
	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, fullpath, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = acpi_type;
	hotplug_event_enqueue (hotplug_event);
}

/** Synthesizes generic acpi objects, i.e. registers all the objects of type
 *  into HAL. This lets us have more than one type of device e.g. BATx
 *  in the same battery class.
 *
 *  @param	path		The ACPI path, e.g. "/proc/acpi/battery"
 *  @param	acpi_type	The type of device, e.g. ACPI_TYPE_BATTERY
 */
static void
acpi_synthesize (const gchar *path, int acpi_type)
{
	const gchar *f;
	gchar _path[HAL_PATH_MAX];
	gboolean is_laptop = FALSE;
	GDir *dir;
	GError *error = NULL;

	dir = g_dir_open (path, 0, &error);
	if (dir == NULL) {
		HAL_ERROR (("Couldn't open %s: %s", path, error->message));
		g_error_free (error);
		return;
	}

	/* do for each object in directory */
	while ((f = g_dir_read_name (dir)) != NULL) {
		gchar buf[HAL_PATH_MAX];
		
		/* check if there is a battery bay or a LID button */
		if (!is_laptop) {
			if ( acpi_type == ACPI_TYPE_BATTERY ) { 
				is_laptop = TRUE;
			} else if ( acpi_type == ACPI_TYPE_BUTTON ) {
				snprintf (_path, sizeof (_path), "%s/acpi/button/lid", get_hal_proc_path ());
				if ( strcmp (path, _path) == 0 )
					is_laptop = TRUE;
			}
		}
		/* if there is a battery bay or LID, this is a laptop -> set the formfactor */
		if ( is_laptop ) {
			HalDevice *computer;

			if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
			    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
				HAL_ERROR (("No computer object?"));
        		} else {
				hal_device_property_set_string (computer, "system.formfactor", "laptop");
			}
		}

		snprintf (buf, sizeof (buf), "%s/%s", path, f);
		acpi_synthesize_item (buf, acpi_type);
	}

	/* close directory */
	g_dir_close (dir);
}

/** If {procfs_path}/acpi/{vendor}/{display} is found, then add the
 *  LaptopPanel device.
 *
 *  @param	vendor		The vendor name, e.g. sony
 *  @param	display		The *possible* name of the brightness file
 *  @param	method		The HAL enumerated type.
 */
static void
acpi_synthesize_display (char *vendor, char *display, int method)
{
	gchar path[HAL_PATH_MAX];
	snprintf (path, sizeof (path), "%s/acpi/%s/%s", get_hal_proc_path (), vendor, display);
	/*
	 * We do not use acpi_synthesize as the target is not a directory full
	 * of directories, but a flat file list.
	 */
	if (g_file_test (path, G_FILE_TEST_EXISTS))
		acpi_synthesize_item (path, method);
}

/** Scan the data structures exported by the kernel and add hotplug
 *  events for adding ACPI objects.
 *
 *  @return			TRUE if, and only if, ACPI capabilities
 *				were detected
 */
gboolean
acpi_synthesize_hotplug_events (void)
{
	HalDevice *computer;
	gchar path[HAL_PATH_MAX];

	if (!g_file_test ("/proc/acpi/info", G_FILE_TEST_EXISTS))
		return FALSE;

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
	    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		return TRUE;
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

	/*
	 * Collect video adaptors (from vendor added modules)
	 * I *know* we should use the /proc/acpi/video/LCD method, but this
	 * doesn't work. And it's depreciated.
	 * When the sysfs code comes into mainline, we can use that, but until
	 * then we can supply an abstracted interface to the user.
	 */
	acpi_synthesize_display ("toshiba", "lcd", ACPI_TYPE_TOSHIBA_DISPLAY);
	acpi_synthesize_display ("asus", "brn", ACPI_TYPE_ASUS_DISPLAY);
	acpi_synthesize_display ("pcc", "brightness", ACPI_TYPE_PANASONIC_DISPLAY);
	acpi_synthesize_display ("ibm", "brightness", ACPI_TYPE_IBM_DISPLAY);
	acpi_synthesize_display ("sony", "brightness", ACPI_TYPE_SONY_DISPLAY);

	/* setup timer for things that we need to poll */
	g_timeout_add (ACPI_POLL_INTERVAL,
		       acpi_poll,
		       NULL);
	/* setup timer for things that we need only to poll infrequently*/
	g_timeout_add ((ACPI_POLL_INTERVAL*120),
		       battery_poll_infrequently,
		       NULL);

	return TRUE;
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

static ACPIDevHandler acpidev_handler_laptop_panel_toshiba = {
	.acpi_type   = ACPI_TYPE_TOSHIBA_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_laptop_panel_asus = {
	.acpi_type   = ACPI_TYPE_ASUS_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_laptop_panel_panasonic = {
	.acpi_type   = ACPI_TYPE_PANASONIC_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_laptop_panel_ibm = {
	.acpi_type   = ACPI_TYPE_IBM_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_laptop_panel_sony = {
	.acpi_type   = ACPI_TYPE_SONY_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
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
	&acpidev_handler_laptop_panel_toshiba,
	&acpidev_handler_laptop_panel_ibm,
	&acpidev_handler_laptop_panel_panasonic,
	&acpidev_handler_laptop_panel_asus,
	&acpidev_handler_laptop_panel_sony,
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
				return;
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
				return;
			}

			/* Run callouts */
			hal_util_callout_device_add (d, acpi_callouts_add_done, end_token, NULL);
			return;
		}
	}
	
	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
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
	int acpi_type;

	acpi_type = hal_device_property_get_int (d, "linux.acpi_type");

	for (i = 0; acpi_handlers [i] != NULL; i++) {
		ACPIDevHandler *handler;

		handler = acpi_handlers[i];
		if (handler->acpi_type == acpi_type) {
			return handler->refresh (d, handler);
		}
	}

	HAL_WARNING (("Didn't find a rescan handler for udi %s", d->udi));
	return TRUE;
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
