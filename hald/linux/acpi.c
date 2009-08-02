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
#include "../device_pm.h"
#include "../hald_dbus.h"
#include "../logger.h"
#include "../util.h"
#include "../util_pm.h"

#include "osspec_linux.h"

#include "acpi.h"
#include "device.h"

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
	ACPI_TYPE_OMNIBOOK_DISPLAY,
	ACPI_TYPE_SONYPI_DISPLAY,
	ACPI_TYPE_BUTTON
};

#define ACPI_POLL_INTERVAL 30 /* in seconds */

typedef struct ACPIDevHandler_s
{
	int acpi_type;
	HalDevice *(*add) (const gchar *acpi_path, HalDevice *parent, struct ACPIDevHandler_s *handler);
	gboolean (*compute_udi) (HalDevice *d, struct ACPIDevHandler_s *handler);
	gboolean (*remove) (HalDevice *d, struct ACPIDevHandler_s *handler);
	gboolean (*refresh) (HalDevice *d, struct ACPIDevHandler_s *handler, gboolean force_full_refresh);
} ACPIDevHandler;

/** 
 *  ac_adapter_refresh_poll
 *  @d:		valid ac_adapter HalDevice
 *
 * Just sets the ac_adapter.present key when called 
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
	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return;

	hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_charging", path,
					  "state", "charging state", 0, "charging", TRUE);
	hal_util_set_bool_elem_from_file (d, "battery.rechargeable.is_discharging", path,
					  "state", "charging state", 0, "discharging", TRUE);
	hal_util_set_string_elem_from_file (d, "battery.charge_level.capacity_state", path, 
					    "state", "capacity state", 0, TRUE);
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

	/* we've now got the 'reporting' keys, now we need to populate the
	 * processed 'charge_level' keys so stuff like desktop power managers
	 * do not have to deal with odd quirks */
	device_pm_abstract_props (d);

	/* we calculate this more precisely */
	device_pm_calculate_percentage (d);	

	/* try to calculate the time accurately (sic) using the rate */
	device_pm_calculate_time (d);
}

/** 
 *  battery_poll_infrequently:
 *  @data:		Ignored
 *  Returns:  		TRUE if we updated values
 *
 *  Recalculates the battery.reporting.last_full key as this may drift
 *  over time. 
 *
 *  Note: This is called 120x less often than battery_refresh_poll
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


/** 
 *  acpi_poll_battery:
 *
 *  @return			TRUE
 *
 * Fallback polling method to refresh battery objects is plugged in 
 *
 * Note: This just calls battery_refresh_poll for each battery
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

/** 
 *  acpi_poll_acadap:
 * 
 *  Returns:			TRUE
 *
 *  Fallback polling method to detect if the ac_adapter is plugged in 
 *
 *  Note: This just calls ac_adapter_refresh_poll for each ac_adapter
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

/** 
 *  acpi_poll:
 *  @data:		Ignored
 *
 *  Returns:		TRUE
 *
 *  Fallback polling method called every minute. 
 *
 *  Note: This just forces a poll refresh for *every* ac_adapter
 *        and primary battery in the system.
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
ac_adapter_refresh (HalDevice *d, ACPIDevHandler *handler, gboolean force_full_refresh)
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

/** 
 *  battery_refresh_add:
 *  @d:		Valid battery HalDevice
 *
 *  Adds all the possible battery.* keys and does coldplug (slowpath)
 *  type calculations. 
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
	const char *technology;

	hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info",
					    "OEM info", 0, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.model", path, "info",
					    "model number", G_MAXUINT, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.serial", path, "info",
					    "serial number", 0, TRUE);
	hal_util_set_string_elem_from_file (d, "battery.vendor", path, "info",
					    "OEM info", 0, TRUE);

	/* This is needed as ACPI does not specify the description text for a
	 *  battery, and so we have to calculate it from the hardware output */
	technology = hal_util_grep_string_elem_from_file (path, "info",
							  "battery type", 0, TRUE);
	if (technology != NULL) {
		hal_device_property_set_string (d, "battery.reporting.technology",
						technology);
		hal_device_property_set_string (d, "battery.technology",
						util_get_battery_technology (technology));
	}

	/* we'll use the .reporting prefix as we don't know
	 * if this data is energy (mWh) or unit enery (mAh) */
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
	/* we'll need this is we want to convert mAh to mWh */
	hal_util_set_string_elem_from_file (d, "battery.voltage.unit", path, "info",
					    "design voltage", 1, TRUE);
	hal_util_set_int_elem_from_file (d, "battery.voltage.design", path,
					 "info", "design voltage", 0, 10, TRUE);

	/* Convert the mWh or mAh units into mWh...
	 * We'll do as many as we can here as the values are not going to change.
	 * We'll set the correct unit (or unknown) also. */
	reporting_unit = hal_device_property_get_string (d, "battery.reporting.unit");
	reporting_design = hal_device_property_get_int (d, "battery.reporting.design");
	reporting_warning = hal_device_property_get_int (d, "battery.reporting.warning");
	reporting_low = hal_device_property_get_int (d, "battery.reporting.low");
	reporting_gran1 = hal_device_property_get_int (d, "battery.reporting.granularity_1");
	reporting_gran2 = hal_device_property_get_int (d, "battery.reporting.granularity_2");

	if (reporting_unit && strcmp (reporting_unit, "mAh") == 0) {
		voltage_design = hal_device_property_get_int (d, "battery.voltage.design");
	
		/* If design voltage is unknown, use 1V. */
		if (voltage_design <= 0)
			voltage_design = 1000; /* mV */;

		/* scale by factor battery.voltage.design */
		hal_device_property_set_int (d, "battery.charge_level.design",
			(reporting_design * voltage_design) / 1000);
		hal_device_property_set_int (d, "battery.charge_level.warning",
			(reporting_warning * voltage_design) / 1000);
		hal_device_property_set_int (d, "battery.charge_level.low",
			(reporting_low * voltage_design) / 1000);
		hal_device_property_set_int (d, "battery.charge_level.granularity_1",
			(reporting_gran1 * voltage_design) / 1000);
		hal_device_property_set_int (d, "battery.charge_level.granularity_2",
			(reporting_gran2 * voltage_design) / 1000);

		/* set unit */
		hal_device_property_set_string (d, "battery.charge_level.unit", "mWh"); /* not mAh! */
	} else {
		/* Some ACPI BIOS's do not report the unit, so we'll assume they are mWh.
		 * We will report the guessing with the battery.charge_level.unit key. */
		hal_device_property_set_int (d, "battery.charge_level.design", reporting_design);
		hal_device_property_set_int (d, "battery.charge_level.warning", reporting_warning);
		hal_device_property_set_int (d, "battery.charge_level.low", reporting_low);
		hal_device_property_set_int (d, "battery.charge_level.granularity_1", reporting_gran1);
		hal_device_property_set_int (d, "battery.charge_level.granularity_2", reporting_gran2);

		/* set unit */
		if (reporting_unit && strcmp (reporting_unit, "mWh") == 0) {
			hal_device_property_set_string (d, "battery.charge_level.unit", "mWh");
		} else {
			/* set "Unknown ACPI Unit" unit so we can debug */
			HAL_WARNING (("Unknown ACPI Unit!"));
			hal_device_property_set_string (d, "battery.charge_level.unit", "unknown");
		}
	}

	/* set alarm if present */
	if (hal_util_set_int_elem_from_file (d, "battery.alarm.design", path,
					     "alarm", "alarm", 0, 10, TRUE))
		hal_util_set_string_elem_from_file (d, "battery.alarm.unit", path, "alarm",
						    "alarm", 1, TRUE);

	/* we are assuming a laptop battery is rechargeable */
	hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);

	/* we need to populate the processed 'charge_level' keys handling odd quirks */
	device_pm_abstract_props (d);

	return TRUE;
}

/** 
 *  battery_refresh:
 *  @d:		Valid battery HalDevice
 *  @handler:	The ACPIDevHandler.
 *  
 *  Returns:    TRUE/FALSE
 *
 *  Refresh the battery device information. 
 */
static gboolean
battery_refresh (HalDevice *d, ACPIDevHandler *handler, gboolean force_full_refresh)
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
		device_pm_remove_optional_props (d);
		device_property_atomic_update_end ();		
	} else {
		/* battery is present */
		device_property_atomic_update_begin ();

		/* So, it's pretty expensive to read from
		 * /proc/acpi/battery/BAT%d/[info|state] so don't read
		 * static data that won't change
		 */
		if (force_full_refresh || !hal_device_has_property (d, "battery.vendor")) {
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

/** 
 *  get_processor_model_name:
 *  @proc_num:	Number of processor
 *  
 *  Returns:    Name of the processor model
 *
 *  Give the model name of the processor. 
 */
static gchar *
get_processor_model_name (gint proc_num)
{
	gchar *model_name;

	gchar  *contents = NULL;
	GError *error    = NULL;

	gchar **lines;

	gint proc_num_i;

	gchar *cursor;
	gint   i;


	if (g_file_get_contents ("/proc/cpuinfo", & contents, NULL, & error)) {
		lines = g_strsplit (contents, "\n", 0);

		for (i = 0; lines [i]; ++i) {
			if (strstr (lines [i], "processor\t:")) {
				cursor = strstr (lines [i], ":");

				proc_num_i = atoi (cursor + 1);

				if (proc_num_i == proc_num) {
					for (; lines [i]; ++i) {
						if (strstr (lines [i], "model name\t:")) {
							cursor = strstr (lines [i], ":");

							g_strstrip (++ cursor);

							model_name = g_strdup (cursor);

							g_strfreev (lines);
							g_free     (contents);

							return model_name;
						}
					}
				}
			}
		}

		if (lines) {
			g_strfreev (lines);
		}
	}
	else {
		HAL_ERROR (("Couldn't open /proc/cpuinfo: %s", error->message));

		g_error_free (error);
	}

	return NULL;
}

static gboolean
processor_refresh (HalDevice *d, ACPIDevHandler *handler, gboolean force_full_refresh)
{
	const char *path;

	gchar *model_name;
	gint   proc_num;

	path = hal_device_property_get_string (d, "linux.acpi_path");
	if (path == NULL)
		return FALSE;

	proc_num = hal_util_grep_int_elem_from_file (
		path, "info", "processor id", 0, 10, FALSE
	);

	if ((model_name = get_processor_model_name (proc_num))) {
		hal_device_property_set_string (d, "info.product", model_name);

		g_free (model_name);
	}
	else
		hal_device_property_set_string (d, "info.product", "Unknown Processor");

	hal_device_property_set_string (d, "info.category", "processor");
	hal_device_add_capability (d, "processor");
	hal_util_set_int_elem_from_file (d, "processor.number", path,
					 "info", "processor id", 0, 10, FALSE);
	hal_util_set_bool_elem_from_file (d, "processor.can_throttle", path,
					  "info", "throttling control", 0, "yes", FALSE);

	return TRUE;
}

static gboolean
fan_refresh (HalDevice *d, ACPIDevHandler *handler, gboolean force_full_refresh)
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
laptop_panel_refresh (HalDevice *d, ACPIDevHandler *handler, gboolean force_full_refresh)
{
	const char *path;
	int acpi_type;
	char *type = NULL;
	char *desc = NULL;
	int br_levels = -1;
	
	if ((hal_device_store_find (hald_get_gdl (),
				    "/org/freedesktop/Hal/devices/computer_backlight")) != NULL)
		return FALSE;

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
	} else if (acpi_type == ACPI_TYPE_OMNIBOOK_DISPLAY) {
		gchar *proc_lcd;
		int current = -1;
		int max = -1;

		type = "omnibook";
		desc = "Omnibook LCD Panel";
		/* 
		 * There are different support brightness level, depending on 
		 * the hardware and the kernel module version.
		 */
		proc_lcd = hal_util_grep_file("/proc/omnibook", "lcd", "LCD brightness:", FALSE);
		proc_lcd = g_strstrip (proc_lcd);
		if (sscanf (proc_lcd, "%d (max value: %d)", &current, &max) == 2) {	
			br_levels = max + 1;
		} else {	
			br_levels = 11;
		}
	} else if (acpi_type == ACPI_TYPE_SONYPI_DISPLAY) {
		type = "sonypi";
		desc = "Sony LCD Panel";
		br_levels = 256;
	} else {
		type = "unknown";
		desc = "Unknown LCD Panel";
		br_levels = 0;
		HAL_WARNING (("acpi_type not recognised!"));
	}

	hal_device_property_set_string (d, "info.product", desc);
	/*
	 * We will set laptop_panel.access_method as the scripts can use this to
	 * determine the set/get parameters.
	 */
	hal_device_property_set_string (d, "laptop_panel.access_method", type);
	/*
	 * We can set laptop_panel.num_levels as it will not change, and allows us
	 * to work out the percentage in the scripts.
	 */
	hal_device_property_set_int (d, "laptop_panel.num_levels", br_levels);
	hal_device_add_capability (d, "laptop_panel");
	return TRUE;
}

static gboolean
button_refresh (HalDevice *d, ACPIDevHandler *handler, gboolean force_full_refresh)
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

/** 
 *  acpi_synthesize_item:
 *  @fullpath:		The ACPI path, e.g. "/proc/acpi/battery/BAT1"
 *  @acpi_type:		The type of device, e.g. ACPI_TYPE_BATTERY
 *
 *  Synthesizes a *specific* acpi object.
 */
static void
acpi_synthesize_item (const gchar *fullpath, int acpi_type)
{
	HotplugEvent *hotplug_event;
	HAL_INFO (("Processing %s", fullpath));
	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, fullpath, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = acpi_type;
	hotplug_event_enqueue (hotplug_event);
}

/** 
 *  acpi_synthesize:
 *  @path:		The ACPI path, e.g. "/proc/acpi/battery"
 *  @acpi_type:		The type of device, e.g. ACPI_TYPE_BATTERY
 *  @synthesize:	If a device should get synthesized
 *
 *  Synthesizes generic acpi objects, i.e. registers all the objects of type
 *  into HAL. This lets us have more than one type of device e.g. BATx
 *  in the same battery class. 
 */
static void
acpi_synthesize (const gchar *path, int acpi_type, gboolean synthesize)
{
	const gchar *f;
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
				if ( strcmp (path, "/proc/acpi/button/lid") == 0 )
					is_laptop = TRUE;
			} else if (_have_sysfs_lid_button) {
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

		if (synthesize) {
			snprintf (buf, sizeof (buf), "%s/%s", path, f);
			acpi_synthesize_item (buf, acpi_type);
		}
	}

	/* close directory */
	g_dir_close (dir);
}

/** 
 *  acpi_synthesize_display:
 *  @vendor:		The vendor name, e.g. sony
 *  @display:		The *possible* name of the brightness file
 *  @method:		The HAL enumerated type.
 *
 *  If {procfs_path}/acpi/{vendor}/{display} is found, then add the
 *  LaptopPanel device. 
 */
static void
acpi_synthesize_display (char *vendor, char *display, int method)
{
	gchar path[HAL_PATH_MAX];
	snprintf (path, sizeof (path), "/proc/%s/%s", vendor, display);
	/*
	 * We do not use acpi_synthesize as the target is not a directory full
	 * of directories, but a flat file list.
	 */
	if (g_file_test (path, G_FILE_TEST_EXISTS))
		acpi_synthesize_item (path, method);
}

static int sonypi_irq_list[] = { 11, 10, 9, 6, 5 };

/** 
 *  acpi_synthesize_sonypi_display:
 *
 *  Synthesizes a sonypi object.
 */
static void
acpi_synthesize_sonypi_display (void)
{
	HotplugEvent *hotplug_event;
	gboolean found = FALSE;
	guint i;
	gchar *path;

	HAL_INFO (("Processing sonypi display"));

        /* Check that we don't support brightness change through ACPI,
	 * for type3 VAIOs */
	if (g_file_test ("/proc/acpi/sony/brightness", G_FILE_TEST_EXISTS))
		return;

	/* Find the sonypi device, this doesn't work
	 * if the sonypi device doesn't have an IRQ, sorry */
	for (i = 0; i < G_N_ELEMENTS (sonypi_irq_list); i++) {
		path =  g_strdup_printf ("/proc/irq/%d/sonypi", sonypi_irq_list[i]);
		if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
			found = TRUE;
			break;
		}
		g_free (path);
	}

	if (!found)
		return;

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, path, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = ACPI_TYPE_SONYPI_DISPLAY;
	hotplug_event_enqueue (hotplug_event);

	g_free (path);
}

/** 
 *  acpi_synthesize_hotplug_events:
 *
 *  Returns:		TRUE if, and only if, ACPI capabilities
 *			were detected
 *
 *  Scan the data structures exported by the kernel and add hotplug
 *  events for adding ACPI objects. 
 */
gboolean
acpi_synthesize_hotplug_events (void)
{
	HalDevice *computer;

	if (!g_file_test ("/proc/acpi/", G_FILE_TEST_IS_DIR))
		return FALSE;

	if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) == NULL &&
	    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) == NULL) {
		HAL_ERROR (("No computer object?"));
		return TRUE;
	}

	/* Set appropriate properties on the computer object */
	hal_device_property_set_string (computer, "power_management.type", "acpi");
	if (g_file_test ("/proc/acpi/info", G_FILE_TEST_EXISTS)) {
		hal_util_set_string_elem_from_file (computer, "power_management.acpi.linux.version",
						    "/proc/acpi", "info", "version", 0, FALSE);
	} else {
		if (!hal_util_set_string_from_file (computer, "power_management.acpi.linux.version",
						    "/sys/module/acpi/parameters", "acpica_version"))
			/* Fallback for some older kernel version, can get removed if HAL depends on >= 2.6.21 */
			hal_util_set_string_elem_from_file (computer, "power_management.acpi.linux.version",
			       			            "/sys/firmware/acpi", "info", "version", 0, FALSE);
	}

	/* collect batteries */
	acpi_synthesize ("/proc/acpi/battery", ACPI_TYPE_BATTERY, TRUE);
	/* collect processors */
	acpi_synthesize ("/proc/acpi/processor", ACPI_TYPE_PROCESSOR, TRUE);
	/* collect fans */
	acpi_synthesize ("/proc/acpi/fan", ACPI_TYPE_FAN, TRUE);
	/* collect AC adapters */
	acpi_synthesize ("/proc/acpi/ac_adapter", ACPI_TYPE_AC_ADAPTER, TRUE);

	/* collect buttons */
	acpi_synthesize ("/proc/acpi/button/lid", ACPI_TYPE_BUTTON, TRUE);
	acpi_synthesize ("/proc/acpi/button/power", ACPI_TYPE_BUTTON, TRUE);
	acpi_synthesize ("/proc/acpi/button/sleep", ACPI_TYPE_BUTTON, TRUE);

	/*
	 * Collect video adaptors (from vendor added modules)
	 * I *know* we should use the /proc/acpi/video/LCD method, but this
	 * doesn't work. And it's depreciated.
	 * When the sysfs code comes into mainline, we can use that, but until
	 * then we can supply an abstracted interface to the user.
	 */
	acpi_synthesize_display ("acpi/toshiba", "lcd", ACPI_TYPE_TOSHIBA_DISPLAY);
	acpi_synthesize_display ("acpi/asus", "brn", ACPI_TYPE_ASUS_DISPLAY);
	acpi_synthesize_display ("acpi/pcc", "brightness", ACPI_TYPE_PANASONIC_DISPLAY);
	acpi_synthesize_display ("acpi/ibm", "brightness", ACPI_TYPE_IBM_DISPLAY);
	acpi_synthesize_display ("acpi/sony", "brightness", ACPI_TYPE_SONY_DISPLAY);
	/* omnibook does not live under acpi GNOME#331458 */
	acpi_synthesize_display ("omnibook", "lcd", ACPI_TYPE_OMNIBOOK_DISPLAY);
	/* sonypi doesn't have an acpi object fd.o#6729 */
	acpi_synthesize_sonypi_display ();

	/* setup timer for things that we need to poll */
#ifdef HAVE_GLIB_2_14
	g_timeout_add_seconds (ACPI_POLL_INTERVAL,
                               acpi_poll,
                               NULL);
#else
	g_timeout_add (1000 * ACPI_POLL_INTERVAL,
		       acpi_poll,
		       NULL);
#endif

	/* setup timer for things that we need only to poll infrequently */

        /* don't use g_timeout_add_seconds() here as the related code path
         * is possibly CPU and time eating and we don't want have any
         * other timeout synced with this one
         */
	g_timeout_add (1000 * ACPI_POLL_INTERVAL * 120,
                       battery_poll_infrequently,
                       NULL);

	return TRUE;
}

static HalDevice *
acpi_generic_add (const gchar *acpi_path, HalDevice *parent, ACPIDevHandler *handler)
{
	HalDevice *d;

	if (((handler->acpi_type == ACPI_TYPE_BATTERY) || (handler->acpi_type == ACPI_TYPE_AC_ADAPTER)) && _have_sysfs_power_supply)
		return NULL;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.acpi_path", acpi_path);
	hal_device_property_set_int (d, "linux.acpi_type", handler->acpi_type);

	if (parent != NULL)
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent));
	else
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");

	if (handler->refresh == NULL || !handler->refresh (d, handler, FALSE)) {
		g_object_unref (d);
		d = NULL;
	}
	return d;
}

static HalDevice *
acpi_button_add (const gchar *acpi_path, HalDevice *parent, ACPIDevHandler *handler)
{
	char *parent_path;
	const char *button_type;

	parent_path = hal_util_get_parent_path (acpi_path);
	button_type = hal_util_get_last_element (parent_path);
	if (strcmp (button_type, "lid") == 0 && _have_sysfs_lid_button)
		goto out;
	else if (strcmp (button_type, "power") == 0 && _have_sysfs_power_button)
		goto out;
	else if (strcmp (button_type, "sleep") == 0 && _have_sysfs_sleep_button)
		goto out;

	g_free (parent_path);
	return acpi_generic_add (acpi_path, parent, handler);

out:
	g_free (parent_path);
	return NULL;
}

static gboolean
acpi_generic_compute_udi (HalDevice *d, ACPIDevHandler *handler)
{
	gchar udi[256];
	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/acpi_%s",
			  hal_util_get_last_element (hal_device_property_get_string (d, "linux.acpi_path")));
	hal_device_set_udi (d, udi);
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

static ACPIDevHandler acpidev_handler_laptop_panel_omnibook = {
	.acpi_type   = ACPI_TYPE_OMNIBOOK_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_laptop_panel_sonypi = {
	.acpi_type   = ACPI_TYPE_SONYPI_DISPLAY,
	.add         = acpi_generic_add,
	.compute_udi = acpi_generic_compute_udi,
	.refresh     = laptop_panel_refresh,
	.remove      = acpi_generic_remove
};

static ACPIDevHandler acpidev_handler_button = {
	.acpi_type   = ACPI_TYPE_BUTTON,
	.add         = acpi_button_add,
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
	&acpidev_handler_laptop_panel_omnibook,
	&acpidev_handler_laptop_panel_sonypi,
	NULL
};

static void
acpi_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void
acpi_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Remove callouts completed udi=%s", hal_device_get_udi (d)));

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

			/* Compute UDI */
			if (!handler->compute_udi (d, handler)) {
				hal_device_store_remove (hald_get_tdl (), d);
				hotplug_event_end (end_token);
				return;
			}

			/* Merge properties from .fdi files */
			di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
			di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);
			
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
			return handler->refresh (d, handler, TRUE);
		}
	}

	HAL_WARNING (("Didn't find a rescan handler for udi %s", hal_device_get_udi (d)));
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

	hotplug_event = g_slice_new0 (HotplugEvent);
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

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_ACPI;
	g_strlcpy (hotplug_event->acpi.acpi_path, acpi_path, sizeof (hotplug_event->acpi.acpi_path));
	hotplug_event->acpi.acpi_type = acpi_type;
	return hotplug_event;
}

void 
acpi_check_is_laptop (const gchar *acpi_type) 
{
	if (acpi_type != NULL) {

		if (strcmp (acpi_type, "BATTERY") == 0) {
			acpi_synthesize ("/proc/acpi/battery", ACPI_TYPE_BATTERY, FALSE);
		} else if (strcmp (acpi_type, "LID") == 0) {
			acpi_synthesize ("/proc/acpi/button/lid", ACPI_TYPE_BUTTON, FALSE);
		} 
	}
}
