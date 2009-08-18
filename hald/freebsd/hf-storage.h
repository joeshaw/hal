/***************************************************************************
 * CVSID: $Id$
 *
 * hf-storage.h : storage device support
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

#ifndef _HF_STORAGE_H
#define _HF_STORAGE_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "hf-osspec.h"
#include "hf-devd.h"

extern HFHandler hf_storage_handler;
extern HFDevdHandler hf_storage_devd_handler;

void hf_storage_device_enable (HalDevice *device);
void hf_storage_device_enable_tape (HalDevice *device);
void hf_storage_device_enable_cdrom (HalDevice *device);
void hf_storage_device_probe (HalDevice *device, gboolean only_media);
void hf_storage_device_add (HalDevice *device);
GSList *hf_storage_get_geoms (const char *devname);

#endif /* _HF_STORAGE_H */
