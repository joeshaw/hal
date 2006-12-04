/***************************************************************************
 * CVSID: $Id$
 *
 * hf-block.h : block device support
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

#ifndef _HF_BLOCK_H
#define _HF_BLOCK_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "../hald.h"

void hf_block_device_enable (HalDevice *device, const char *devname);
void hf_block_device_complete (HalDevice *device,
			       HalDevice *storage_device,
			       gboolean is_volume);

#endif /* _HF_BLOCK_H */
