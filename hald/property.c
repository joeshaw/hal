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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>

#include "property.h"

struct _HalProperty {
	char *key;

	int type;
	union {
		char *str_value;
		dbus_int32_t int_value;
		dbus_bool_t bool_value;
		double double_value;
	};
};

void
hal_property_free (HalProperty *prop)
{
	g_free (prop->key);
	
	if (prop->type == DBUS_TYPE_STRING)
		g_free (prop->str_value);

	g_free (prop);
}

HalProperty *
hal_property_new_string (const char *key, const char *value)
{
	HalProperty *prop;

	prop = g_new0 (HalProperty, 1);

	prop->type = DBUS_TYPE_STRING;
	prop->key = g_strdup (key);
	prop->str_value = g_strdup (value);

	return prop;
}

HalProperty *
hal_property_new_int (const char *key, dbus_int32_t value)
{
	HalProperty *prop;

	prop = g_new0 (HalProperty, 1);

	prop->type = DBUS_TYPE_INT32;
	prop->key = g_strdup (key);
	prop->int_value = value;

	return prop;
}

HalProperty *
hal_property_new_bool (const char *key, dbus_bool_t value)
{
	HalProperty *prop;

	prop = g_new0 (HalProperty, 1);

	prop->type = DBUS_TYPE_BOOLEAN;
	prop->key = g_strdup (key);
	prop->bool_value = value;

	return prop;
}

HalProperty *
hal_property_new_double (const char *key, double value)
{
	HalProperty *prop;

	prop = g_new0 (HalProperty, 1);

	prop->type = DBUS_TYPE_DOUBLE;
	prop->key = g_strdup (key);
	prop->double_value = value;

	return prop;
}

const char *
hal_property_get_key (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, NULL);

	return prop->key;
}

int
hal_property_get_type (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, DBUS_TYPE_INVALID);

	return prop->type;
}

const char *
hal_property_get_string (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, NULL);
	g_return_val_if_fail (prop->type == DBUS_TYPE_STRING, NULL);

	return prop->str_value;
}

dbus_int32_t
hal_property_get_int (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, -1);
	g_return_val_if_fail (prop->type == DBUS_TYPE_INT32, -1);

	return prop->int_value;
}

dbus_bool_t
hal_property_get_bool (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, -1);
	g_return_val_if_fail (prop->type == DBUS_TYPE_BOOLEAN, -1);

	return prop->bool_value;
}

char *
hal_property_get_as_string (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, NULL);

	switch (prop->type) {
	case DBUS_TYPE_STRING:
		return g_strdup (prop->str_value);
	case DBUS_TYPE_INT32:
		return g_strdup_printf ("%d", prop->int_value);
	case DBUS_TYPE_BOOLEAN:
		/* FIXME: Maybe use 1 and 0 here instead? */
		return g_strdup (prop->bool_value ? "true" : "false");
	case DBUS_TYPE_DOUBLE:
		return g_strdup_printf ("%f", prop->double_value);
	default:
		return NULL;
	}
}

double
hal_property_get_double (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, -1.0);
	g_return_val_if_fail (prop->type == DBUS_TYPE_DOUBLE, -1.0);

	return prop->double_value;
}

void
hal_property_set_string (HalProperty *prop, const char *value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == DBUS_TYPE_STRING ||
			  prop->type == DBUS_TYPE_NIL);

	prop->type = DBUS_TYPE_STRING;
	prop->str_value = g_strdup (value);
}

void
hal_property_set_int (HalProperty *prop, dbus_int32_t value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == DBUS_TYPE_INT32 ||
			  prop->type == DBUS_TYPE_NIL);

	prop->type = DBUS_TYPE_INT32;
	prop->int_value = value;
}

void
hal_property_set_bool (HalProperty *prop, dbus_bool_t value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == DBUS_TYPE_BOOLEAN ||
			  prop->type == DBUS_TYPE_NIL);

	prop->type = DBUS_TYPE_BOOLEAN;
	prop->bool_value = value;
}

void
hal_property_set_double (HalProperty *prop, double value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == DBUS_TYPE_DOUBLE ||
			  prop->type == DBUS_TYPE_NIL);

	prop->type = DBUS_TYPE_DOUBLE;
	prop->double_value = value;
}
