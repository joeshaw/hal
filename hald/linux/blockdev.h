/***************************************************************************
 * CVSID: $Id$
 *
 * blockdev.h : Handling of block devices 
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

#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <glib.h>

void hotplug_event_begin_add_blockdev (const gchar *sysfs_path, const char *device_file, gboolean is_partition, HalDevice *parent, void *end_token);

void hotplug_event_begin_remove_blockdev (const gchar *sysfs_path, void *end_token);

void hotplug_event_refresh_blockdev (gchar *sysfs_path, HalDevice *d, void *end_token);

gboolean blockdev_rescan_device (HalDevice *d);

HotplugEvent *blockdev_generate_add_hotplug_event (HalDevice *d);

HotplugEvent *blockdev_generate_remove_hotplug_event (HalDevice *d);

void blockdev_refresh_mount_state (HalDevice *d);

void blockdev_process_mdstat (void);

#endif /* BLOCKDEV_H */
