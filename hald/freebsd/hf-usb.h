/***************************************************************************
 * CVSID: $Id$
 *
 * hf-usb.h : USB support
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

#ifndef _HF_USB_H
#define _HF_USB_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "hf-osspec.h"
#include "hf-devd.h"

extern HFHandler hf_usb_handler;
extern HFDevdHandler hf_usb_devd_handler;

void hf_usb_device_compute_udi(HalDevice *device);
void hf_usb_add_webcam_properties(HalDevice *device);

#endif /* _HF_USB_H */
