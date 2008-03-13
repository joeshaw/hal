/***************************************************************************
 * CVSID: $Id$
 *
 * Dummy backend for HAL
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

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../util.h"
#include "../device_info.h"

HalFileMonitor *
osspec_get_file_monitor (void)
{
	return NULL;
}

guint
hal_file_monitor_add_notify (HalFileMonitor          *monitor,
                            const char             *path,
                            int                     mask,
                            HalFileMonitorNotifyFunc notify_func,
                            gpointer                data)
{
	return 0;
}

void
osspec_privileged_init (void)
{
}

void
osspec_init (void)
{
}

static void 
computer_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	osspec_probe_done ();
}

void 
osspec_probe (void)
{
	HalDevice *root;

	root = hal_device_new ();
	hal_device_property_set_string (root, "info.subsystem", "unknown");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");

	hal_device_store_add (hald_get_tdl (), root);

	di_search_and_merge (root, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (root, DEVICE_INFO_TYPE_POLICY);

	hal_util_callout_device_add (root, computer_callouts_add_done, NULL, NULL);
}

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

gboolean
osspec_device_rescan (HalDevice *d)
{
	return FALSE;
}

gboolean
osspec_device_reprobe (HalDevice *d)
{
	return FALSE;
}

void
osspec_refresh_mount_state_for_block_device (HalDevice *d)
{
}

