/***************************************************************************
 * CVSID: $Id$
 *
 * Dummy backend for HAL
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

void
osspec_init (void)
{
}

void
osspec_shutdown (void)
{
}

void 
osspec_probe (void)
{
	HalDevice *root;

	root = hal_device_new ();
	hal_device_property_set_string (root, "info.bus", "unknown");
	hal_device_property_set_string (root, "linux.sysfs_path_device", "(none)");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_property_set_string (root, "info.udi", "/org/freedesktop/Hal/devices/computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");

	hal_device_store_add (hald_get_tdl (), root);

	di_search_and_merge (root);

	hal_device_store_remove (hald_get_tdl (), root);
	hal_device_store_add (hald_get_gdl (), root);


	osspec_probe_done ();
}

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}