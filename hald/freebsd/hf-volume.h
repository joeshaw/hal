/***************************************************************************
 * CVSID: $Id$
 *
 * hf-volume.h : volume device support
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

#ifndef _HF_VOLUME_H
#define _HF_VOLUME_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "hf-osspec.h"

extern HFHandler hf_volume_handler;

void hf_volume_update_mount (HalDevice *device);

HalDevice *hf_volume_device_add (HalDevice *parent,
				 HalDevice *storage_device,
				 gboolean has_children,
				 gboolean is_swap,
				 const char *devname,
				 const char *gclass,
				 const char *gstr_type,
				 int gtype,
				 int gindex,
				 dbus_int64_t partition_offset,
				 dbus_int64_t partition_size);

#endif /* _HF_VOLUME_H */
