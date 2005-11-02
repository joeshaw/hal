/***************************************************************************
 * CVSID: $Id$
 *
 * hal_monitor.h : monitor mode for watching stuff about devices
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#ifndef HAL_MONITOR_H
#define HAL_MONITOR_H

#include "main.h"

void hal_monitor_enter(GMainLoop* loop);

void etc_mtab_process_all_block_devices(dbus_bool_t setup_watcher);

#endif /* HAL_MONITOR_H */
