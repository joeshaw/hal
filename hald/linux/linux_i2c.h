/***************************************************************************
 * CVSID: $Id$
 *
 * linux_i2c.h : I2C handling on Linux 2.6; based on linux_pci.h
 *
 * Copyright (C) 2004 Matthew Mastracci <matt@aclaro.com>
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

#ifndef LINUX_I2C_H
#define LINUX_I2C_H

#include "linux_common.h"

void visit_device_i2c(const char* path, struct sysfs_device *device);

void linux_i2c_init();
void linux_i2c_detection_done();
void linux_i2c_shutdown();

#endif /* LINUX_I2C_H */
