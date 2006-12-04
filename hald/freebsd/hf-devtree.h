/***************************************************************************
 * CVSID: $Id$
 *
 * hf-devtree.h : generic device tree support
 *
 * Copyright (C) 2006 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#ifndef _HF_DEVTREE_H
#define _HF_DEVTREE_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "hf-osspec.h"

extern HFHandler hf_devtree_handler;

HalDevice *hf_devtree_find_from_name (HalDeviceStore *store,
				      const char *name);
HalDevice *hf_devtree_find_from_info (HalDeviceStore *store,
				      const char *driver,
				      int unit);

HalDevice *hf_devtree_find_parent_from_name (HalDeviceStore *store,
					     const char *name);
HalDevice *hf_devtree_find_parent_from_info (HalDeviceStore *store,
					     const char *driver,
					     int unit);

void hf_devtree_device_set_info (HalDevice *device,
				 const char *driver,
				 int unit);
gboolean hf_devtree_device_get_info (HalDevice *device,
				     const char **driver,
				     int *unit);

void hf_devtree_device_set_name (HalDevice *device, const char *devname);
char *hf_devtree_device_get_name (HalDevice *device);

gboolean hf_devtree_is_driver (const char *name, const char *driver);

#endif /* _HF_DEVTREE_H */
