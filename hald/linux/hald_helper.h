/***************************************************************************
 * CVSID: $Id$
 *
 * HAL daemon hotplug.d and dev.d helper details
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

#ifndef HALD_HELPER_H
#define HALD_HELPER_H

#define HALD_HELPER_MAGIC 0x68616c64
#define HALD_HELPER_SOCKET_PATH "/var/run/hal/hotplug_socket"
#define HALD_HELPER_STRLEN 256

struct hald_helper_msg
{
	unsigned int magic;                    /**< magic */
	int is_hotplug_or_dev;                 /**< 1 if hotplug msg, 0 if device msg */
	int is_add;                            /**< 1 if add, 0 if remove */
	int seqnum;                            /**< Sequence number (may be -1 if for dev if udev has no support) */
	char subsystem[HALD_HELPER_STRLEN];    /**< subsystem e.g. usb, pci (only for hotplug msg) */
	char sysfs_path[HALD_HELPER_STRLEN];   /**< path into sysfs without sysfs mountpoint, e.g. /block/sda */
	char device_node[HALD_HELPER_STRLEN];  /**< fully qualified path of device node (only for device msg) */
};

#endif /* HALD_HELPER_H */
