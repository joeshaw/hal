/***************************************************************************
 * CVSID: $Id$
 *
 * HAL daemon hotplug.d and dev.d helper details
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

#ifndef HALD_HELPER_H
#define HALD_HELPER_H

#define HALD_HELPER_MAGIC 0x68616c64
#define HALD_HELPER_SOCKET_PATH "/var/run/hald/hotplug_socket2"
#define HALD_HELPER_STRLEN 256

struct hald_helper_msg
{
	unsigned int magic;			/* magic */
	unsigned long long seqnum;		/* Sequence number (may be 0 if for dev if udev has no support) */
	char action[HALD_HELPER_STRLEN];	/* hotplug action */
	char subsystem[HALD_HELPER_STRLEN];	/* subsystem e.g. usb, pci (only for hotplug msg) */
	char sysfs_path[HALD_HELPER_STRLEN];	/* path into sysfs without sysfs mountpoint, e.g. /block/sda */
	char device_name[HALD_HELPER_STRLEN];	/* absolute path of device node (only for device msg) */
	int net_ifindex;                        /* For networking class devices only; the value of the ifindex file*/
	time_t time_stamp;                      /* Time of day we received the hotplug event */
};

#endif /* HALD_HELPER_H */
