/***************************************************************************
 * CVSID: $Id$
 *
 * hald_conf.h : Global configuration for hal daemon
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

#ifndef HALD_CONF_H
#define HALD_CONF_H

#include <stdarg.h>
#include <stdint.h>
#include <dbus/dbus.h>

typedef struct _HaldConf HaldConf;

/** Configuration of policies for the HAL daemon.
 */
struct _HaldConf {
	/** Default value for storage.media_check_enabled for devices
	 *  of capability storage - this can be overridden by .fdi
	 *  files.
	 *
	 *  Setting this to FALSE results a whitelist policy,
	 *  e.g. media check is only enabled for storage devices with
	 *  a .fdi file saying so.
	 *
	 *  Conversely, setting it to TRUE results in a blacklist
	 *  policy where media check is enabled by default but may be
	 *  overridden by a .fdi for devices causing trouble.
	 *
	 *  Default value is TRUE, this may be overridden in hald.conf.
	 */
	dbus_bool_t storage_media_check_enabled;

	/** Default value for storage.automount_enabled for devices of
	 *  capability storage - this can be overridden by .fdi files.
	 *
	 *  Setting this to FALSE results a whitelist policy,
	 *  e.g. policy agents should only automount storage devices with
	 *  a .fdi file saying so.
	 *
	 *  Conversely, setting it to TRUE results in a blacklist
	 *  policy where policy agents should always automount unless
	 *  this is explicitly overridden by .fdi for devices
	 *  causing trouble.
	 *
	 *  Default value is TRUE, this may be overridden in hald.conf.
	 */
	dbus_bool_t storage_automount_enabled;

	/** Default value for storage.cdrom.eject_check_enabled for
	 *  devices of capability storage.cdrom - this can be overridden by
	 *  .fdi files.
	 *
	 *  Setting this to FALSE results a whitelist policy,
	 *  e.g. the eject button is only checked if this property is
	 *  overridden in a .fdi file.
	 *
	 *  Conversely, setting it to TRUE results in a blacklist
	 *  policy where the eject button is always checked unless
	 *  this is explicitly overridden by .fdi file for devices
	 *  causing trouble.
	 *
	 *  Default value is TRUE, this may be overridden in hald.conf.
	 */
	dbus_bool_t storage_cdrom_eject_check_enabled;

	/** If true, then the device list is saved to disk such that
         *  properties are kept between invocations of hald.
	 */
	dbus_bool_t persistent_device_list;
};

HaldConf *hald_get_conf (void);

void hald_read_conf_file (void);

#endif  /* HALD_CONF_H */
