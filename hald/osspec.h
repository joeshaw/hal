/***************************************************************************
 * CVSID: $Id$
 *
 * osspec.h : OS Specific interface
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

#ifndef OSSPEC_H
#define OSSPEC_H

#include <stdarg.h>
#include <stdint.h>
#include <dbus/dbus.h>


/** Initialize the OS specific parts of the daemon
 *
 *  @param  dbus_connection     The D-BUS connection the HAL daemon got,
 */
void osspec_init(DBusConnection* dbus_connection);

/** Probe all hardware present in the system and synchronize with the
 *  device list
 *
 */
void osspec_probe();

DBusHandlerResult osspec_filter_function(DBusConnection* connection,
                                         DBusMessage* message,
                                         void* user_data);

#endif /* OSSPEC_H */
