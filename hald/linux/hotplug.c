/***************************************************************************
 * CVSID: $Id$
 *
 * hotplug.c : Handling of hotplug events
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../hald.h"
#include "../logger.h"
#include "../osspec.h"

#include "acpi.h"
#include "apm.h"
#include "blockdev.h"
#include "device.h"
#include "osspec_linux.h"
#include "pmu.h"

#include "hotplug.h"

/** Queue of ordered hotplug events */
GQueue *hotplug_event_queue;

/** List of HotplugEvent objects we are currently processing */
GSList *hotplug_events_in_progress = NULL;

void
hotplug_event_end (void *end_token)
{
	HotplugEvent *hotplug_event = (HotplugEvent *) end_token;

	hotplug_events_in_progress = g_slist_remove (hotplug_events_in_progress, hotplug_event);

	if (hotplug_event->free_function != NULL) {
		hotplug_event->free_function (hotplug_event);
	} else {
		g_free (hotplug_event);
	}
}

void 
hotplug_event_reposted (void *end_token)
{
	HotplugEvent *hotplug_event = (HotplugEvent *) end_token;

	hotplug_event->reposted = TRUE;
	hotplug_events_in_progress = g_slist_remove (hotplug_events_in_progress, hotplug_event);
}

static void
hotplug_event_begin_sysfs (HotplugEvent *hotplug_event)
{
	HalDevice *d;
	char subsystem[HAL_PATH_MAX];
	gchar *subsystem_target;

	d = hal_device_store_match_key_value_string (hald_get_gdl (),
						     "linux.sysfs_path",
						     hotplug_event->sysfs.sysfs_path);

#if 0
	/* we should refresh the device when we get "change" uevent */
	if (d != NULL && hotplug_event->action == HOTPLUG_ACTION_ADD) {
		HAL_ERROR (("device %s already present in the store, so refreshing", hotplug_event->sysfs.sysfs_path));
		hotplug_event_refresh_dev (hotplug_event->sysfs.subsystem, d, (void *) hotplug_event);
		return;
	}
#endif

	/* subsystem "block" are all block devices */
	if (hotplug_event->type == HOTPLUG_EVENT_SYSFS)
		if (strcmp(hotplug_event->sysfs.subsystem, "block") == 0)
			hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;

	/* get device type from already known device object */
	if (hotplug_event->type == HOTPLUG_EVENT_SYSFS && d != NULL) {
		HotplugEventType type;

		type = hal_device_property_get_int (d, "linux.hotplug_type");
		if (type == HOTPLUG_EVENT_SYSFS_DEVICE) {
			HAL_INFO (("%s is a device (store)", hotplug_event->sysfs.sysfs_path));
			hotplug_event->type = HOTPLUG_EVENT_SYSFS_DEVICE;
		} else if (type == HOTPLUG_EVENT_SYSFS_BLOCK) {
			HAL_INFO (("%s is a block device (store)", hotplug_event->sysfs.sysfs_path));
			hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;
		}
	}

	/*
	 * determine device type by "subsystem" link (from kernel 2.6.18, class devices
	 * start to move from /class to /devices and have a "subsystem" link pointing
	 * back to the subsystem
	 */
	if (hotplug_event->type == HOTPLUG_EVENT_SYSFS) {
		g_snprintf (subsystem, HAL_PATH_MAX, "%s/subsystem", hotplug_event->sysfs.sysfs_path);
		/* g_file_read_link leaks memory. We alloc lots of trash here but return NULL, damn
		   Re-implemented this using POSIX readlink() */
		/* subsystem_target = g_file_read_link (subsystem, NULL); */
		subsystem_target = hal_util_readlink(subsystem);
		if (subsystem_target != NULL) {
			if (strstr(subsystem_target, "/block") != NULL) {
				HAL_INFO (("%s is a block device (subsystem)", hotplug_event->sysfs.sysfs_path));
				hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;
			} else {
				HAL_INFO (("%s is a device (subsystem)", hotplug_event->sysfs.sysfs_path));
				hotplug_event->type = HOTPLUG_EVENT_SYSFS_DEVICE;
			}
		}
	}

	/* older kernels get the device type from the devpath */
	if (hotplug_event->type == HOTPLUG_EVENT_SYSFS) {
		char sys_block_path[HAL_PATH_MAX];
		gsize sys_block_path_len;

		sys_block_path_len   = g_snprintf (sys_block_path, HAL_PATH_MAX, "%s/block", get_hal_sysfs_path ());
		if (strncmp (hotplug_event->sysfs.sysfs_path, sys_block_path, sys_block_path_len) == 0) {
			HAL_INFO (("%s is a block device (devpath)", hotplug_event->sysfs.sysfs_path));
			hotplug_event->type = HOTPLUG_EVENT_SYSFS_BLOCK;
		} else
			hotplug_event->type = HOTPLUG_EVENT_SYSFS_DEVICE;
	}

	if (hotplug_event->type == HOTPLUG_EVENT_SYSFS_DEVICE) {
		if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
			HalDevice *parent;
			gchar *parent_path;

			hal_util_find_known_parent (hotplug_event->sysfs.sysfs_path,
							&parent, &parent_path);
			hotplug_event_begin_add_dev (hotplug_event->sysfs.subsystem,
							  hotplug_event->sysfs.sysfs_path,
							  hotplug_event->sysfs.device_file,
							  parent,
							  parent_path,
							  (void *) hotplug_event);
			g_free (parent_path);
		} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
			hotplug_event_begin_remove_dev (hotplug_event->sysfs.subsystem,
							     hotplug_event->sysfs.sysfs_path,
							     (void *) hotplug_event);
		} else if (hotplug_event->action == HOTPLUG_ACTION_CHANGE && d != NULL) {
			hotplug_event_refresh_dev (hotplug_event->sysfs.subsystem,
                                                   hotplug_event->sysfs.sysfs_path,
                                                   d,
                                                   (void *) hotplug_event);
		}
	} else if (hotplug_event->type == HOTPLUG_EVENT_SYSFS_BLOCK) {
		if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
			HalDevice *parent;
			int range;
			gboolean is_partition;

			/* it's a partition if and only if it doesn't have the range file...
			 *
			 * notably the device mapper partitions do have a range file, but that's
			 * fine, we don't count them as partitions anyway...
			 *
			 * also, if the sysfs ends with "fakevolume" the hotplug event is synthesized
			 * from within HAL for partitions on the main block device
			 */
			if ((strstr (hotplug_event->sysfs.sysfs_path, "/fakevolume") != NULL) ||
                            hal_util_get_int_from_file (hotplug_event->sysfs.sysfs_path, "range", &range, 0)) {
				is_partition = FALSE;
			} else {
				is_partition = TRUE;
                        }

			hal_util_find_known_parent (hotplug_event->sysfs.sysfs_path, &parent, NULL);
			hotplug_event_begin_add_blockdev (hotplug_event->sysfs.sysfs_path,
							  hotplug_event->sysfs.device_file,
							  is_partition,
							  parent,
							  (void *) hotplug_event);
		} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
			hotplug_event_begin_remove_blockdev (hotplug_event->sysfs.sysfs_path,
							     (void *) hotplug_event);
		} else if (hotplug_event->action == HOTPLUG_ACTION_CHANGE && d != NULL) {
			hotplug_event_refresh_blockdev (hotplug_event->sysfs.sysfs_path,
                                                        d,
                                                        (void *) hotplug_event);
		}
	} else {
		/* just ignore this hotplug event */
		hotplug_event_end ((void *) hotplug_event);
	}
}

static void
hotplug_event_begin_acpi (HotplugEvent *hotplug_event)
{
	if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
		hotplug_event_begin_add_acpi (hotplug_event->acpi.acpi_path, 
					      hotplug_event->acpi.acpi_type,
					      NULL,
					      (void *) hotplug_event);
	} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
		hotplug_event_begin_remove_acpi (hotplug_event->acpi.acpi_path, 
						 hotplug_event->acpi.acpi_type,
						 (void *) hotplug_event);
	}
}

static void
hotplug_event_begin_apm (HotplugEvent *hotplug_event)
{
	if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
		hotplug_event_begin_add_apm (hotplug_event->apm.apm_path, 
					     hotplug_event->apm.apm_type,
					     NULL,
					     (void *) hotplug_event);
	} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
		hotplug_event_begin_remove_apm (hotplug_event->apm.apm_path, 
						hotplug_event->apm.apm_type,
						(void *) hotplug_event);
	}
}

static void
hotplug_event_begin_pmu (HotplugEvent *hotplug_event)
{
	if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
		hotplug_event_begin_add_pmu (hotplug_event->pmu.pmu_path, 
					     hotplug_event->pmu.pmu_type,
					     NULL,
					     (void *) hotplug_event);
	} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
		hotplug_event_begin_remove_pmu (hotplug_event->pmu.pmu_path, 
						hotplug_event->pmu.pmu_type,
						(void *) hotplug_event);
	}
}

static void
hotplug_event_begin (HotplugEvent *hotplug_event)
{
	switch (hotplug_event->type) {

	/* explicit fallthrough */
	case HOTPLUG_EVENT_SYSFS:
	case HOTPLUG_EVENT_SYSFS_DEVICE:
	case HOTPLUG_EVENT_SYSFS_BLOCK:
		hotplug_event_begin_sysfs (hotplug_event);
		break;

	case HOTPLUG_EVENT_ACPI:
		hotplug_event_begin_acpi (hotplug_event);
		break;

	case HOTPLUG_EVENT_APM:
		hotplug_event_begin_apm (hotplug_event);
		break;

	case HOTPLUG_EVENT_PMU:
		hotplug_event_begin_pmu (hotplug_event);
		break;

	default:
		HAL_ERROR (("Unknown hotplug event type %d", hotplug_event->type));
		hotplug_event_end ((void *) hotplug_event);
		break;
	}
}

void 
hotplug_event_enqueue (HotplugEvent *hotplug_event)
{
	if (hotplug_event_queue == NULL)
		hotplug_event_queue = g_queue_new ();

	g_queue_push_tail (hotplug_event_queue, hotplug_event);
}

void 
hotplug_event_enqueue_at_front (HotplugEvent *hotplug_event)
{
	if (hotplug_event_queue == NULL)
		hotplug_event_queue = g_queue_new ();

	g_queue_push_head (hotplug_event_queue, hotplug_event);
}

void 
hotplug_event_process_queue (void)
{
	HotplugEvent *hotplug_event;

        while (hotplug_events_in_progress != NULL ||
		(hotplug_event_queue != NULL &&
		 !g_queue_is_empty (hotplug_event_queue))) {

		/* do not process events if some other event is in progress 
		 *
		 * TODO: optimize so we can do add events in parallel by inspecting the
		 *       wait_for_sysfs_path parameter and hotplug_events_in_progress list
		 */
		if (hotplug_events_in_progress != NULL && g_slist_length (hotplug_events_in_progress) > 0)
			goto out;

		hotplug_event = g_queue_pop_head (hotplug_event_queue);
		if (hotplug_event == NULL)
			goto out;

		hotplug_events_in_progress = g_slist_append (hotplug_events_in_progress, hotplug_event);
		hotplug_event_begin (hotplug_event);
	}

	hotplug_queue_now_empty ();
out:
        ;
}

gboolean 
hotplug_rescan_device (HalDevice *d)
{
	gboolean ret;

	switch (hal_device_property_get_int (d, "linux.hotplug_type")) {
	case HOTPLUG_EVENT_SYSFS_DEVICE:
		ret = dev_rescan_device (d);
		break;

	case HOTPLUG_EVENT_SYSFS_BLOCK:
		ret = blockdev_rescan_device (d);
		break;

	case HOTPLUG_EVENT_ACPI:
		ret = acpi_rescan_device (d);
		break;

	case HOTPLUG_EVENT_APM:
		ret = apm_rescan_device (d);
		break;

	case HOTPLUG_EVENT_PMU:
		ret = pmu_rescan_device (d);
		break;

	default:
		HAL_INFO (("Unknown hotplug type for udi=%s", hal_device_get_udi (d)));
		ret = FALSE;
		break;
	}

	return ret;
}

static void
hotplug_reprobe_generate_remove_events (HalDevice *d)
{
	GSList *i;
	GSList *childs;
	HotplugEvent *e;

	/* first remove childs */
	childs = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), "info.parent", hal_device_get_udi (d));
	for (i = childs; i != NULL; i = g_slist_next (i)) {
		HalDevice *child;

		child = HAL_DEVICE (i->data);
		hotplug_reprobe_generate_remove_events (child);
	}

	/* then remove self */
	HAL_INFO (("Generate remove event for udi %s", hal_device_get_udi (d)));
	switch (hal_device_property_get_int (d, "linux.hotplug_type")) {
	case HOTPLUG_EVENT_SYSFS_DEVICE:
		e = dev_generate_remove_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_SYSFS_BLOCK:
		e = blockdev_generate_remove_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_ACPI:
		e = acpi_generate_remove_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_APM:
		e = apm_generate_remove_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_PMU:
		e = pmu_generate_remove_hotplug_event (d);
		break;

	default:
		e = NULL;
		HAL_INFO (("Unknown hotplug type for udi=%s", hal_device_get_udi (d)));
		break;
	}

	if (e != NULL) {
		hotplug_event_enqueue (e);
	}
}

static void
hotplug_reprobe_generate_add_events (HalDevice *d)
{
	GSList *i;
	GSList *childs;
	HotplugEvent *e;

	/* first add self */
	HAL_INFO (("Generate add event for udi %s", hal_device_get_udi (d)));
	switch (hal_device_property_get_int (d, "linux.hotplug_type")) {
	case HOTPLUG_EVENT_SYSFS_DEVICE:
		e = dev_generate_add_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_SYSFS_BLOCK:
		e = blockdev_generate_add_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_ACPI:
		e = acpi_generate_add_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_APM:
		e = apm_generate_add_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_PMU:
		e = pmu_generate_add_hotplug_event (d);
		break;

	default:
		e = NULL;
		HAL_INFO (("Unknown hotplug type for udi=%s", hal_device_get_udi (d)));
		break;
	}

	if (e != NULL) {
		hotplug_event_enqueue (e);
	}

	/* then add childs */
	childs = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), "info.parent", hal_device_get_udi (d));
	for (i = childs; i != NULL; i = g_slist_next (i)) {
		HalDevice *child;

		child = HAL_DEVICE (i->data);
		hotplug_reprobe_generate_add_events (child);
	}
}

gboolean
hotplug_reprobe_tree (HalDevice *d)
{
	hotplug_reprobe_generate_remove_events (d);
	hotplug_reprobe_generate_add_events (d);
	hotplug_event_process_queue ();
	return FALSE;
}
