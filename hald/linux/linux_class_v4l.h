/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_v4l.h : V4L device handling on Linux 2.6
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

#ifndef LINUX_CLASS_V4L_ADAPTER_H
#define LINUX_CLASS_V4L_ADAPTER_H

#include "linux_common.h"

void visit_class_device_v4l(const char* path, 
                            struct sysfs_class_device *class_device);


void linux_class_v4l_init();
void linux_class_v4l_detection_done();
void linux_class_v4l_shutdown();

#endif /* LINUX_CLASS_V4L_ADAPTER_H */
