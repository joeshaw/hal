/***************************************************************************
 * CVSID: $Id$
 *
 * hotplug.c : Handling of hotplug events
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../device_info.h"

#include "osspec_linux.h"

#include "hotplug.h"
#include "physdev.h"
#include "classdev.h"
#include "blockdev.h"
#include "acpi.h"
#include "apm.h"
#include "pmu.h"

/** Queue of ordered hotplug events */
GQueue *hotplug_event_queue;

/** List of HotplugEvent objects we are currently processing */
GSList *hotplug_events_in_progress = NULL;

void 
hotplug_event_end (void *end_token)
{
	HotplugEvent *hotplug_event = (HotplugEvent *) end_token;

	hotplug_events_in_progress = g_slist_remove (hotplug_events_in_progress, hotplug_event);
	g_free (hotplug_event);
	hotplug_event_process_queue ();
}

void 
hotplug_event_reposted (void *end_token)
{
	HotplugEvent *hotplug_event = (HotplugEvent *) end_token;

	hotplug_events_in_progress = g_slist_remove (hotplug_events_in_progress, hotplug_event);
	hotplug_event_process_queue ();
}



static void
fixup_net_device_for_renaming (HotplugEvent *hotplug_event)
{
	/* fixup net devices by looking at ifindex */
	if (strcmp (hotplug_event->sysfs.subsystem, "net") == 0 && hotplug_event->sysfs.net_ifindex != -1) {
		int ifindex;
		
		if (!hal_util_get_int_from_file (hotplug_event->sysfs.sysfs_path, "ifindex", &ifindex, 10) ||
		    (ifindex != hotplug_event->sysfs.net_ifindex)) {
			GDir *dir;
			char path[HAL_PATH_MAX];
			char path1[HAL_PATH_MAX];
			GError *err = NULL;
			const gchar *f;
			
			/* search for new name */
			HAL_WARNING (("Net interface @ %s with ifindex %d was probably renamed",
				      hotplug_event->sysfs.sysfs_path, hotplug_event->sysfs.net_ifindex));
			
			
			g_snprintf (path, HAL_PATH_MAX, "%s/class/net" , get_hal_sysfs_path());
			if ((dir = g_dir_open (path, 0, &err)) == NULL) {
				HAL_ERROR (("Unable to open %/class/net: %s", get_hal_sysfs_path(), err->message));
				g_error_free (err);
				goto out;
			}
			while ((f = g_dir_read_name (dir)) != NULL) {
				g_snprintf (path1, HAL_PATH_MAX, "%s/class/net/%s" , get_hal_sysfs_path (), f);
				if (hal_util_get_int_from_file (path1, "ifindex", &ifindex, 10)) {
					if (ifindex == hotplug_event->sysfs.net_ifindex) {
						HAL_INFO (("Using sysfs path %s for ifindex %d", path1, ifindex));
						strncpy (hotplug_event->sysfs.sysfs_path, path1, HAL_PATH_MAX);
						g_dir_close (dir);
						goto out;
					}
				}
				
			}
			g_dir_close (dir);	
		}
	}
out:
	;
}


static void
hotplug_event_begin_sysfs (HotplugEvent *hotplug_event)
{
	static char sys_devices_path[HAL_PATH_MAX];
	static char sys_class_path[HAL_PATH_MAX];
	static char sys_block_path[HAL_PATH_MAX];
	static gsize sys_devices_path_len = 0;
	static gsize sys_class_path_len = 0;
	static gsize sys_block_path_len = 0;

	if (sys_block_path_len == 0) {
		sys_devices_path_len = g_snprintf (sys_devices_path, HAL_PATH_MAX, "%s/devices", get_hal_sysfs_path ());
		sys_class_path_len   = g_snprintf (sys_class_path, HAL_PATH_MAX, "%s/class", get_hal_sysfs_path ());
		sys_block_path_len   = g_snprintf (sys_block_path, HAL_PATH_MAX, "%s/block", get_hal_sysfs_path ());
	}

	if (hotplug_event->action == HOTPLUG_ACTION_ADD &&
	    hal_device_store_match_key_value_string (hald_get_gdl (),
						     "linux.sysfs_path",
						     hotplug_event->sysfs.sysfs_path)) {
		HAL_ERROR (("devpath %s already present in the store, ignore event", hotplug_event->sysfs.sysfs_path));
		hotplug_event_end ((void *) hotplug_event);
		return;
	}

	if (strncmp (hotplug_event->sysfs.sysfs_path, sys_devices_path, sys_devices_path_len) == 0) {
		if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
			HalDevice *parent;
			parent = hal_util_find_closest_ancestor (hotplug_event->sysfs.sysfs_path);
			hotplug_event_begin_add_physdev (hotplug_event->sysfs.subsystem, 
							 hotplug_event->sysfs.sysfs_path, 
							 parent,
							 (void *) hotplug_event);
		} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
			hotplug_event_begin_remove_physdev (hotplug_event->sysfs.subsystem, 
							    hotplug_event->sysfs.sysfs_path, 
							    (void *) hotplug_event);
		}
	} else if (strncmp (hotplug_event->sysfs.sysfs_path, sys_class_path, sys_class_path_len) == 0) {
		if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
			gchar *target;
			HalDevice *physdev;
			char physdevpath[256];
			gchar *sysfs_path_in_devices;

			sysfs_path_in_devices = NULL;

			/* /sbin/ifrename may be called from a hotplug handler before we process this,
			 * so if index doesn't match, go ahead and find a new sysfs path
			 */
			fixup_net_device_for_renaming (hotplug_event);
			
			g_snprintf (physdevpath, HAL_PATH_MAX, "%s/device", hotplug_event->sysfs.sysfs_path);
			if (((target = g_file_read_link (physdevpath, NULL)) != NULL)) {
				gchar *normalized_target;

				normalized_target = hal_util_get_normalized_path (hotplug_event->sysfs.sysfs_path, target);
				g_free (target);

				sysfs_path_in_devices = g_strdup (normalized_target);

				/* there may be ''holes'' in /sys/devices so try hard to find the closest match */
				do {
					physdev = hal_device_store_match_key_value_string (hald_get_gdl (), 
											   "linux.sysfs_path_device", 
											   normalized_target);
					if (physdev != NULL)
						break;

					/* go up one directory */
					if (!hal_util_path_ascend (normalized_target))
						break;

				} while (physdev == NULL);

				g_free (normalized_target);
			} else {
				physdev = NULL;
			}

			hotplug_event_begin_add_classdev (hotplug_event->sysfs.subsystem,
							  hotplug_event->sysfs.sysfs_path,
							  hotplug_event->sysfs.device_file,
							  physdev,
							  sysfs_path_in_devices,
							  (void *) hotplug_event);

			g_free (sysfs_path_in_devices);

		} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
			hotplug_event_begin_remove_classdev (hotplug_event->sysfs.subsystem,
							     hotplug_event->sysfs.sysfs_path,
							     (void *) hotplug_event);
		}
	} else if (strncmp (hotplug_event->sysfs.sysfs_path, sys_block_path, sys_block_path_len) == 0) {
		gchar *parent_path;
		gboolean is_partition;
		
		parent_path = hal_util_get_parent_path (hotplug_event->sysfs.sysfs_path);
		is_partition = (strcmp (parent_path, sys_block_path) != 0);
		
		if (hotplug_event->action == HOTPLUG_ACTION_ADD) {
			HalDevice *parent;

			if (is_partition) {
				parent = hal_device_store_match_key_value_string (hald_get_gdl (), 
										  "linux.sysfs_path_device", 
										  parent_path);
			} else {
				gchar *target;
				char physdevpath[256];
				
				g_snprintf (physdevpath, HAL_PATH_MAX, "%s/device", hotplug_event->sysfs.sysfs_path);
				if (((target = g_file_read_link (physdevpath, NULL)) != NULL)) {
					gchar *normalized_target;

					normalized_target = hal_util_get_normalized_path (hotplug_event->sysfs.sysfs_path, target);
					g_free (target);
					parent = hal_device_store_match_key_value_string (hald_get_gdl (), 
											  "linux.sysfs_path_device", 
											  normalized_target);
					g_free (normalized_target);
				} else {
					parent = NULL;
				}
			}
			
			hotplug_event_begin_add_blockdev (hotplug_event->sysfs.sysfs_path,
							  hotplug_event->sysfs.device_file,
							  is_partition,
							  parent,
							  (void *) hotplug_event);
		} else if (hotplug_event->action == HOTPLUG_ACTION_REMOVE) {
			hotplug_event_begin_remove_blockdev (hotplug_event->sysfs.sysfs_path,
							     is_partition,
							     (void *) hotplug_event);
		}

		g_free (parent_path);
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
	case HOTPLUG_EVENT_SYSFS_BUS:
	case HOTPLUG_EVENT_SYSFS_CLASS:
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

	if (hotplug_events_in_progress == NULL && 
	    (hotplug_event_queue == NULL || g_queue_is_empty (hotplug_event_queue))) {
		hotplug_queue_now_empty ();
		goto out;
	}

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

out:
	;	
}

gboolean 
hotplug_rescan_device (HalDevice *d)
{
	gboolean ret;

	switch (hal_device_property_get_int (d, "linux.hotplug_type")) {
	case HOTPLUG_EVENT_SYSFS_BUS:
		ret = physdev_rescan_device (d);
		break;

	case HOTPLUG_EVENT_SYSFS_CLASS:
		ret = classdev_rescan_device (d);
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
		HAL_INFO (("Unknown hotplug type for udi=%s", d->udi));
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
	childs = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), "info.parent", d->udi);
	for (i = childs; i != NULL; i = g_slist_next (i)) {
		HalDevice *child;

		child = HAL_DEVICE (i->data);
		hotplug_reprobe_generate_remove_events (child);
	}

	/* then remove self */
	HAL_INFO (("Generate remove event for udi %s", d->udi));
	switch (hal_device_property_get_int (d, "linux.hotplug_type")) {
	case HOTPLUG_EVENT_SYSFS_BUS:
		e = physdev_generate_remove_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_SYSFS_CLASS:
		e = classdev_generate_remove_hotplug_event (d);
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
		HAL_INFO (("Unknown hotplug type for udi=%s", d->udi));
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
	HAL_INFO (("Generate add event for udi %s", d->udi));
	switch (hal_device_property_get_int (d, "linux.hotplug_type")) {
	case HOTPLUG_EVENT_SYSFS_BUS:
		e = physdev_generate_add_hotplug_event (d);
		break;

	case HOTPLUG_EVENT_SYSFS_CLASS:
		e = classdev_generate_add_hotplug_event (d);
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
		HAL_INFO (("Unknown hotplug type for udi=%s", d->udi));
		break;
	}

	if (e != NULL) {
		hotplug_event_enqueue (e);
	}

	/* then add childs */
	childs = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), "info.parent", d->udi);
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
