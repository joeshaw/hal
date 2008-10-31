/***************************************************************************
 * CVSID: $Id$
 *
 * hotplug.h : Handling of hotplug events
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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
	HOTPLUG_ACTION_CHANGE,
	HOTPLUG_ACTION_MOVE
} HotplugActionType;

typedef enum {
	HOTPLUG_EVENT_SYSFS        = 0,
	HOTPLUG_EVENT_SYSFS_DEVICE = 2,
	HOTPLUG_EVENT_SYSFS_BLOCK  = 3,
	HOTPLUG_EVENT_ACPI         = 4,
	HOTPLUG_EVENT_APM          = 5,
	HOTPLUG_EVENT_PMU          = 6
} HotplugEventType;

/** Data structure representing a hotplug event; also used for
 *  coldplugging.
 */
typedef struct
{
	HotplugActionType action;				/* Whether the event is add or remove */
	HotplugEventType type;					/* Type of event */
	gboolean reposted;					/* Avoid loops */
	union {
		struct {
			char subsystem[HAL_NAME_MAX];		/* Kernel subsystem the device belongs to */
			char sysfs_path[HAL_PATH_MAX];		/* Kernel device devpath */
			char sysfs_path_old[HAL_PATH_MAX];	/* Old kernel device devpath (for 'move') */
			char device_file[HAL_PATH_MAX];	        /* Device node for the device */
			unsigned long long seqnum;		/* kernel uevent sequence number */
			int net_ifindex;			/* Kernel ifindex for network devices */

			/* if the device is a Device mapper device, used to prevent multiple string compares */
			gboolean is_dm_device;

			/* stuff udev may tell us about the device and we don't want to query */
			char vendor[HAL_NAME_MAX];
			char model[HAL_NAME_MAX];
			char revision[HAL_NAME_MAX];
			char serial[HAL_NAME_MAX];
			char fsusage[HAL_NAME_MAX];
			char fstype[HAL_NAME_MAX];
			char fsversion[HAL_NAME_MAX];
			char fslabel[HAL_NAME_MAX];
			char fsuuid[HAL_NAME_MAX];
		} sysfs;

		struct {
			int  acpi_type;				/* Type of ACPI object; see acpi.c */
			char acpi_path[HAL_PATH_MAX];		/* Path into procfs, e.g. /proc/acpi/battery/BAT0/ */
		} acpi;

		struct {
			int  apm_type;				/* Type of APM object; see apm.c */
			char apm_path[HAL_PATH_MAX];		/* Path into procfs, e.g. /proc/apm */
		} apm;

		struct {
			int  pmu_type;				/* Type of PMU object; see pmu.c */
			char pmu_path[HAL_PATH_MAX];		/* Path into procfs, e.g. /proc/pmu/battery_0 */
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
