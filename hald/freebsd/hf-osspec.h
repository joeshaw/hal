/***************************************************************************
 * CVSID: $Id$
 *
 * hf-osspec.h : HAL backend for FreeBSD
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

#ifndef _HF_OSSPEC_H
#define _HF_OSSPEC_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "../hald.h"

typedef struct
{
  void		(*privileged_init)	(void);
  void		(*init)			(void);
  void		(*probe)		(void);
  gboolean	(*device_rescan)	(HalDevice *device);
  gboolean	(*device_reprobe)	(HalDevice *device);
} HFHandler;

#endif /* _HF_OSSPEC_H */
