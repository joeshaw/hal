/***************************************************************************
 * CVSID: $Id$
 *
 * Common functionality used by Linux implementation
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#ifndef COMMON_H
#define COMMON_H

#include <dbus/dbus.h>

#include "../device_store.h"
#include "libsysfs/libsysfs.h"

/**
 * @addtogroup HalDaemonLinuxCommon
 *
 * @{
 */

double parse_double (const char *str);
dbus_int32_t parse_dec (const char *str);
dbus_int32_t parse_hex (const char *str);
dbus_uint64_t parse_hex_uint64 (const char *str);

long int find_num (char *pre, char *s, int base);
double find_double (char *pre, char *s);
int find_bcd2 (char *pre, char *s);
char *find_string (char *pre, char *s);

char *read_single_line (char *filename_format, ...);

char *read_single_line_grep (char *begin, char *filename_format, ...);

const char *get_last_element (const char *s);

/* returns the path of the udevinfo program */
const char *udevinfo_path (void);

/** Type for function to compute the UDI (unique device id) for a given
 *  HAL device.
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
typedef char *(*ComputeFDI) (HalDevice * d, int append_num);

char *rename_and_merge (HalDevice *d, ComputeFDI naming_func, const char *namespace);

HalDevice * find_closest_ancestor (const char *sysfs_path);

HalDevice * find_computer ();

dbus_bool_t class_device_get_major_minor (const char *sysfs_path, int *major, int *minor);

dbus_bool_t class_device_get_device_file (const char *sysfs_path, char *dev_file, int dev_file_length);


const char *drivers_lookup (const char *device_path);

void drivers_collect (const char *bus_name);

/** Timeout in milliseconds for waiting for a sysfs device to appear as
 *  a HAL device. Usually used when hotplugging usb-storage since
 *  many HAL devices (usb, usbif, scsi_host, scsi_device, block*2)
 *  appear and the linux kernel gives us these add events out of
 *  order.
 *
 *  This can be changed to WAIT_FOR_UDEV_TIMEOUT once udev support SEQNUM; 60 
 *  secs isn't unreasonable with this setup; if you hotplug a hub full of 
 *  devices and whatnot while hald is under load with h-d-m on a slow machine.
 */
#define HAL_LINUX_HOTPLUG_TIMEOUT 60000

dbus_bool_t got_parent (const char *sysfs_path);

void etc_mtab_process_all_block_devices (dbus_bool_t force);

extern char sysfs_mount_path[SYSFS_PATH_MAX];



/* @} */

#endif				/* COMMON_H */
