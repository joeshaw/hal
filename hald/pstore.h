/***************************************************************************
 * CVSID: $Id$
 *
 * pstore.h : persistent property store on disk
 *
 * Copyright (C) 2004 Kay Sievers, <kay.sievers@vrfy.org>
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

#ifndef PSTORE_H
#define PSTORE_H

#include "device.h"
#include "property.h"

typedef struct _HalPStore	HalPStore;

void		hal_pstore_init			(const char *path);

HalPStore	*hal_pstore_open		(const char *path);

void		hal_pstore_close		(HalPStore *pstore);

void		hal_pstore_save_property	(HalPStore *pstore,
						 HalDevice *device,
						 HalProperty *prop);

void		hal_pstore_load_property	(HalPStore *pstore,
						 HalDevice *device,
						 const char *key);

void		hal_pstore_delete_property	(HalPStore *pstore,
						 HalDevice *device,
						 HalProperty *prop);

void		hal_pstore_load_device		(HalPStore *pstore,
						 HalDevice *device);


#endif /* PSTORE_H */
