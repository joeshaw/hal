/***************************************************************************
 *
 * util_pm.c - Various power management related utilities that do not need
 *             to use HalDevice. This is suitable to use in addons and probers.
 *
 * Copyright (C) 2005 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2005 Danny Kukawka <danny.kukawka@web.de>
 *
 * Licensed under the Academic Free License version 2.1
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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>

#include <glib.h>

#include "logger.h"

#include "util_pm.h"

typedef struct {
	int last_level;
	int last_chargeRate;
	int high_time_warn_count;
	time_t last_time;
} batteryInfo;

static GHashTable *saved_battery_info = NULL;

/**  
 *  util_get_battery_technology_
 *  @type:                The battery type recieved from the hardware
 *
 *  Returns:              The battery technology which is one of: unknown, lithium-ion or lead-acid
 *
 *  Convert the hardware reported value into a few sane choices
 *
 *  This is needed as ACPI does not specify the description text for a
 *  battery, and so we have to calculate it from the hardware output
 */
const char *
util_get_battery_technology (const char *type)
{
	if (type == NULL) {
		return "unknown";
	}
	/* every case combination of Li-Ion is commonly used.. */
	if (strcasecmp (type, "li-ion") == 0 ||
	    strcasecmp (type, "lion") == 0) {
		return "lithium-ion";
	}
	if (strcasecmp (type, "pb") == 0 ||
	    strcasecmp (type, "pbac") == 0) {
		return "lead-acid";
	}
	if (strcasecmp (type, "lip") == 0 ||
	    strcasecmp (type, "lipo") == 0) {
		return "lithium-polymer";
	}
	if (strcasecmp (type, "nimh") == 0) {
		return "nickel-metal-hydride";
	}
	if (strcasecmp (type, "lifo") == 0) {
		return "lithium-iron-phosphate";
	}
	return "unknown";
}

/**  
 *  util_compute_time_remaining:
 *  @id:                 Optional ID given to this battery. Unused at present.
 *  @chargeRate:         The "rate" (typically mW)
 *  @chargeLevel:        The current charge level of the battery (typically mWh)
 *  @chargeLastFull:     The last "full" charge of the battery (typically mWh)
 *  @isDischarging:      If battery is discharging
 *  @isCharging:         If battery is charging
 *  @guessChargeRate:    If ignore chargeRate and guess them.
 *
 *  Returns:                     Number of seconds, or -1 if invalid
 *
 *  Given all the required parameters, this function will return the number 
 *  of seconds until the battery is charged (if charging) or the number
 *  of seconds until empty (if discharging)
 */
int 
util_compute_time_remaining (const char *id,
			     int chargeRate,
			     int chargeLevel,
			     int chargeLastFull,
			     gboolean isDischarging,
			     gboolean isCharging,
			     gboolean guessChargeRate)
{
	int remaining_time = 0;

	/* should not get negative values */
	if (chargeRate < 0 || chargeLevel < 0 || chargeLastFull < 0) {
		HAL_WARNING (("chargeRate, chargeLevel or chargeLastFull < 0, returning -1"));
		return -1;
	}
	/* batteries cannot charge and discharge at the same time */
	if (isDischarging && isCharging) {
		HAL_WARNING (("isDischarging & isCharging TRUE, returning -1"));
		return -1;
	}
	/* 
	 * Some laptops don't supply any rate info, but that's no reason for HAL not
	 * to. We use the current and previous chargeLevel to estimate the rate.
	 * The info is stored in a GHashTable because there could be more than one battery.
	 */
	if (chargeRate == 0 || guessChargeRate) {
		batteryInfo *battery_info;
		time_t cur_time = time(NULL);

		/* Initialize the save_battery_info GHashTable */
		if (!saved_battery_info) 
			saved_battery_info = g_hash_table_new(g_str_hash, g_str_equal);

		if ((battery_info = g_hash_table_lookup(saved_battery_info, id))) {
			/* check this to prevent division by zero */
			if ((cur_time == battery_info->last_time) || (chargeLevel == battery_info->last_level)) {
				/* if we can't calculate because nothing changed, use last 
				 * chargeRate to prevent removing battery.remaining_time.
				 */
				chargeRate = battery_info->last_chargeRate;
			} else {
				chargeRate = ((chargeLevel - battery_info->last_level) * 60 * 60) / (cur_time - battery_info->last_time);
				/* During discharging chargeRate would be negative, which would mess 
				 * up the the calculation below, so we make sure it's always positive.
				 */ 
				chargeRate = (chargeRate > 0) ? chargeRate : -chargeRate;
	
				battery_info->last_level = chargeLevel;
				battery_info->last_time = cur_time;
				battery_info->last_chargeRate = chargeRate;
			}
		} else {
			battery_info = g_new0(batteryInfo, 1);
			g_hash_table_insert(saved_battery_info, (char*) id, battery_info);

			battery_info->last_level = chargeLevel;
			battery_info->last_time = cur_time;
			battery_info->last_chargeRate = 0;
 			battery_info->high_time_warn_count = 0;
			return -1;
		}
	} 

	if (chargeRate == 0)
		return -1;

	if (isDischarging) { 
		remaining_time = ((double) chargeLevel / (double) chargeRate) * 60 * 60;
	} else if (isCharging) {
		/* 
		 * Some ACPI BIOS's don't update chargeLastFull, 
		 * so return 0 as we don't know how much more there is left
		 */
		if (chargeLevel > chargeLastFull ) {
			HAL_WARNING (("chargeLevel > chargeLastFull, returning -1"));
			return -1;
		}
		remaining_time = ((double) (chargeLastFull - chargeLevel) / (double) chargeRate) * 60 * 60;
	}
	
	/* This shouldn't happen, but check for completeness */
	if (remaining_time < 0) {
		HAL_WARNING (("remaining_time %i, returning -1", remaining_time));
		remaining_time = -1;
	}
	/* Battery life cannot be above 60 hours */
	else if (remaining_time > 60*60*60) {
		batteryInfo *battery_info;

		/* to be sure saved_battery_info is initialised */
		if (!saved_battery_info) 
			saved_battery_info = g_hash_table_new(g_str_hash, g_str_equal);

		if (!(battery_info = g_hash_table_lookup(saved_battery_info, id))) {
			battery_info = g_new0(batteryInfo, 1);
			g_hash_table_insert(saved_battery_info, (char*) id, battery_info);
			battery_info->last_level = -1;
			battery_info->last_time = -1;
			battery_info->last_chargeRate = -1;
			battery_info->high_time_warn_count = 0;
		}

		/* display the warning only 10 times and then ever 100 calls , should  avoid to flood syslog */
		if (battery_info->high_time_warn_count < 10 || !(battery_info->high_time_warn_count % 100)) {
			HAL_WARNING (("remaining_time *very* high, returning -1"));
			battery_info->high_time_warn_count += 1;
		}

		remaining_time = -1;
	}

	return remaining_time;
}

