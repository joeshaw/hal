/***************************************************************************
 * CVSID: $Id$
 *
 * callout.h : Call out to helper programs when devices are added/removed.
 *
 * Copyright (C) 2004 Novell, Inc.
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

#ifndef CALLOUT_H
#define CALLOUT_H

#include <glib.h>
#include "device.h"

void hal_callout_device     (HalDevice  *device,
			     gboolean    added);
void hal_callout_capability (HalDevice  *device,
			     const char *capability,
			     gboolean    added);

#endif /* CALLOUT_H */
