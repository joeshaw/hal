/***************************************************************************
 * Linux kernel device handling
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2006 Kay Sievers <kay.sievers@novell.com>
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

#ifndef DEV_H
#define DEV_H

#include <glib.h>
#include "hotplug.h"

typedef enum {
	OSS_DEVICE_TYPE_DSP,
	OSS_DEVICE_TYPE_ADSP,
	OSS_DEVICE_TYPE_MIDI,
	OSS_DEVICE_TYPE_AMIDI,
	OSS_DEVICE_TYPE_AUDIO,
	OSS_DEVICE_TYPE_MIXER,
	OSS_DEVICE_TYPE_UNKNOWN
} ClassDevOSSDeviceTypes;

void hotplug_event_begin_add_dev (const gchar *subsystem, const gchar *sysfs_path, const gchar *device_file,
				  HalDevice *parent_dev, const gchar *parent_path,
				  void *end_token);

void hotplug_event_begin_remove_dev (const gchar *subsystem, const gchar *sysfs_path, void *end_token);
void hotplug_event_refresh_dev (const gchar *subsystem, const gchar *sysfs_path, HalDevice *d, void *end_token);

gboolean dev_rescan_device (HalDevice *d);

HotplugEvent *dev_generate_add_hotplug_event (HalDevice *d);

HotplugEvent *dev_generate_remove_hotplug_event (HalDevice *d);

extern gboolean _have_sysfs_lid_button;
extern gboolean _have_sysfs_power_button;
extern gboolean _have_sysfs_sleep_button;
extern gboolean _have_sysfs_power_supply;

#endif
