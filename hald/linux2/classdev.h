/***************************************************************************
 * CVSID: $Id$
 *
 * classdev.h : Handling of functional kernel devices
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

#ifndef CLASSDEV_H
#define CLASSDEV_H

#include <glib.h>
#include "hotplug.h"

void hotplug_event_begin_add_classdev (const gchar *subsystem, const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices, void *end_token);

void hotplug_event_begin_remove_classdev (const gchar *subsystem, const gchar *sysfs_path, void *end_token);

gboolean classdev_rescan_device (HalDevice *d);

HotplugEvent *classdev_generate_add_hotplug_event (HalDevice *d);

HotplugEvent *classdev_generate_remove_hotplug_event (HalDevice *d);

#endif /* CLASSDEV_H */
