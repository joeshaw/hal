/***************************************************************************
 * CVSID: $Id$
 *
 * hal_net.h : Network device functions for sysfs-agent on Linux 2.6
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifndef HAL_NET_H
#define HAL_NET_H

#include "main.h"

/*  @ingroup  HalAgentsLinux
 *  @{
 */

void visit_class_device_net(const char* path, 
                            struct sysfs_class_device* class_device);

void hal_net_init();
void hal_net_shutdown();

/* @} */

#endif /* HAL_NET_H */
