/***************************************************************************
 * CVSID: $Id$
 *
 * blockdev.c : Handling of block devices 
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

#include <stdio.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <sys/types.h>
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
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

#include "util.h"
#include "coldplug.h"
#include "hotplug_helper.h"

#include "hotplug.h"
#include "blockdev.h"

void
hotplug_event_begin_add_blockdev (const gchar *sysfs_path, const char *device_file, gboolean is_partition, 
				  HalDevice *parent, void *end_token)
{
#if 0
	HalDevice *d;

	HAL_INFO (("block_add: sysfs_path=%s dev=%s is_part=%d parent=0x%08x", 
		 sysfs_path, device_file, is_partition, parent));

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path);
	hal_device_property_set_string (d, "info.udi", sysfs_path);
	hal_device_property_set_string (d, "info.bus", "block");
	hal_device_set_udi (d, sysfs_path);
	
	if (parent != NULL) {
		hal_device_property_set_string (d, "info.parent", parent->udi);
	}
	
	hal_device_store_add (hald_get_gdl (), d);
#endif

	hotplug_event_end (end_token);
}

void
hotplug_event_begin_remove_blockdev (const gchar *sysfs_path, gboolean is_partition, void *end_token)
{
#if 0
	HalDevice *d;

	HAL_INFO (("block_rem: sysfs_path=%s is_part=%d", sysfs_path, is_partition));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), 
						     "linux.sysfs_path_device", 
						     sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Couldn't remove device with sysfs path %s - not found", sysfs_path));
		goto out;
	}

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device with sysfs path %s", sysfs_path));
	}

out:
#endif
	hotplug_event_end (end_token);
}
