/***************************************************************************
 * CVSID: $Id$
 *
 * hotplug.h : Handling of hotplug events
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifndef HOTPLUG_H
#define HOTPLUG_H

#include <glib.h>

#include "../device.h"
#include "../util.h"

typedef enum {
	HOTPLUG_ACTION_ADD,
	HOTPLUG_ACTION_REMOVE,
	HOTPLUG_ACTION_ONLINE,
	HOTPLUG_ACTION_OFFLINE,
} HotplugActionType;

typedef enum {
	HOTPLUG_EVENT_SYSFS       = 0,
	HOTPLUG_EVENT_SYSFS_BUS   = 1,
	HOTPLUG_EVENT_SYSFS_CLASS = 2,
	HOTPLUG_EVENT_SYSFS_BLOCK = 3,
	HOTPLUG_EVENT_ACPI        = 4,
	HOTPLUG_EVENT_APM         = 5,
	HOTPLUG_EVENT_PMU         = 6
} HotplugEventType;

/** Data structure representing a hotplug event; also used for
 *  coldplugging.
 */
typedef struct
{
	HotplugActionType action;               /**< Whether the event is add or remove */
	HotplugEventType type;                  /**< Type of hotplug event */

	union {
		struct {
			char subsystem[HAL_PATH_MAX];           /**< Subsystem e.g. usb, pci (only for hotplug msg) */
			char sysfs_path[HAL_PATH_MAX];          /**< Path into sysfs e.g. /sys/block/sda */
		
			char wait_for_sysfs_path[HAL_PATH_MAX];	/**< Wait for completion of events that a) comes 
								 *   before this one ; AND b) has a sysfs path that
								 *   is contained in or equals this */
			
			char device_file [HAL_PATH_MAX];        /**< Path to special device file (may be NULL) */
			
			int net_ifindex;                        /**< For network only; the value of the ifindex file */
		} sysfs;

		struct {
			int  acpi_type;                         /**< Type of ACPI object; see acpi.c */
			char acpi_path[HAL_PATH_MAX];           /**< Path into procfs, e.g. /proc/acpi/battery/BAT0/ */
		} acpi;

		struct {
			int  apm_type;                          /**< Type of APM object; see apm.c */
			char apm_path[HAL_PATH_MAX];            /**< Path into procfs, e.g. /proc/apm */
		} apm;

		struct {
			int  pmu_type;                          /**< Type of PMU object; see pmu.c */
			char pmu_path[HAL_PATH_MAX];            /**< Path into procfs, e.g. /proc/pmu/battery_0 */
		} pmu;
	};

} HotplugEvent;

void hotplug_event_enqueue (HotplugEvent *event);

void hotplug_event_enqueue_at_front (HotplugEvent *hotplug_event);

void hotplug_event_process_queue (void);

void hotplug_event_end (void *end_token);

void hotplug_event_reposted (void *end_token);

gboolean hotplug_rescan_device (HalDevice *d);

gboolean hotplug_reprobe_tree (HalDevice *d);

void hotplug_queue_now_empty (void);

#endif /* HOTPLUG_H */
