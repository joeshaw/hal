/***************************************************************************
 * CVSID: $Id$
 *
 * property.c : HalProperty methods
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#ifndef PROPERTY_H
#define PROPERTY_H

#include <dbus/dbus.h>

typedef struct _HalProperty HalProperty;

void          hal_property_free          (HalProperty  *prop);

HalProperty *hal_property_new_string     (const char   *key,
					  const char   *value);
HalProperty *hal_property_new_int        (const char   *key,
					  dbus_int32_t  value);
HalProperty *hal_property_new_bool       (const char   *key,
					  dbus_bool_t   value);
HalProperty *hal_property_new_double     (const char   *key,
					  double        value);

const char   *hal_property_get_key       (HalProperty  *prop);
int           hal_property_get_type      (HalProperty  *prop);
char         *hal_property_get_as_string (HalProperty  *prop);

const char   *hal_property_get_string    (HalProperty  *prop);
dbus_int32_t  hal_property_get_int       (HalProperty  *prop);
dbus_bool_t   hal_property_get_bool      (HalProperty  *prop);
double        hal_property_get_double    (HalProperty  *prop);

void          hal_property_set_string    (HalProperty  *prop,
					  const char   *value);
void          hal_property_set_int       (HalProperty  *prop,
					  dbus_int32_t  value);
void          hal_property_set_bool      (HalProperty  *prop,
					  dbus_bool_t   value);
void          hal_property_set_double    (HalProperty  *prop,
					  double        value);

#endif /* PROPERTY_H */
