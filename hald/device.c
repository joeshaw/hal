/***************************************************************************
 * CVSID: $Id$
 *
 * device.c : HalDevice methods
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

#include <stdio.h>
#include <string.h>

#include "device.h"
#include "hald_marshal.h"
#include "logger.h"

static GObjectClass *parent_class;

enum {
	PROPERTY_CHANGED,
	CAPABILITY_ADDED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
hal_device_finalize (GObject *obj)
{
	HalDevice *device = HAL_DEVICE (obj);

	g_slist_foreach (device->properties, (GFunc) hal_property_free, NULL);

	g_free (device->udi);

	if (parent_class->finalize)
		parent_class->finalize (obj);
}

static void
hal_device_class_init (HalDeviceClass *klass)
{
	GObjectClass *obj_class = (GObjectClass *) klass;

	parent_class = g_type_class_peek_parent (klass);

	obj_class->finalize = hal_device_finalize;

	signals[PROPERTY_CHANGED] =
		g_signal_new ("property_changed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceClass,
					       property_changed),
			      NULL, NULL,
			      hald_marshal_VOID__STRING_BOOL_BOOL,
			      G_TYPE_NONE, 3,
			      G_TYPE_STRING,
			      G_TYPE_BOOLEAN,
			      G_TYPE_BOOLEAN);

	signals[CAPABILITY_ADDED] =
		g_signal_new ("capability_added",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceClass,
					       capability_added),
			      NULL, NULL,
			      hald_marshal_VOID__STRING,
			      G_TYPE_NONE, 1,
			      G_TYPE_STRING);
}

static void
hal_device_init (HalDevice *device)
{
	static int temp_device_counter = 0;

	device->udi = g_strdup_printf ("/org/freedesktop/Hal/devices/temp/%d",
				       temp_device_counter++);
}

GType
hal_device_get_type (void)
{
	static GType type = 0;
	
	if (!type) {
		static GTypeInfo type_info = {
			sizeof (HalDeviceClass),
			NULL, NULL,
			(GClassInitFunc) hal_device_class_init,
			NULL, NULL,
			sizeof (HalDevice),
			0,
			(GInstanceInitFunc) hal_device_init
		};

		type = g_type_register_static (G_TYPE_OBJECT,
					       "HalDevice",
					       &type_info,
					       0);
	}

	return type;
}

HalDevice *
hal_device_new (void)
{
	HalDevice *device;

	device = g_object_new (HAL_TYPE_DEVICE, NULL);

	return device;
}

void
hal_device_merge (HalDevice *target, HalDevice *source)
{
	const char *caps;
	GSList *iter;

	/* device_property_atomic_update_begin (); */

	for (iter = source->properties; iter != NULL; iter = iter->next) {
		HalProperty *p = iter->data;
		int type;
		const char *key;
		int target_type;

		key = hal_property_get_key (p);
		type = hal_property_get_type (p);

		/* handle info.capabilities in a special way */
		if (strcmp (key, "info.capabilities") == 0)
			continue;

		/* only remove target if it exists with a different type */
		target_type = hal_device_property_get_type (target, key);
		if (target_type != DBUS_TYPE_NIL && target_type != type)
			hal_device_property_remove (target, key);

		switch (type) {

		case DBUS_TYPE_STRING:
			hal_device_property_set_string (
				target, key,
				hal_property_get_string (p));
			break;

		case DBUS_TYPE_INT32:
			hal_device_property_set_int (
				target, key,
				hal_property_get_int (p));
			break;

		case DBUS_TYPE_BOOLEAN:
			hal_device_property_set_bool (
				target, key,
				hal_property_get_bool (p));
			break;

		case DBUS_TYPE_DOUBLE:
			hal_device_property_set_double (
				target, key,
				hal_property_get_double (p));
			break;

		default:
			HAL_WARNING (("Unknown property type %d", type));
			break;
		}
	}

	/* device_property_atomic_update_end (); */

	caps = hal_device_property_get_string (source, "info.capabilities");
	if (caps != NULL) {
		char **split_caps, **iter;

		split_caps = g_strsplit (caps, " ", 0);
		for (iter = split_caps; *iter != NULL; iter++) {
			if (!hal_device_has_capability (target, *iter))
				hal_device_add_capability (target, *iter);
		}

		g_strfreev (split_caps);
	}
}

gboolean
hal_device_matches (HalDevice *device1, HalDevice *device2,
		    const char *namespace)
{
	int len;
	GSList *iter;

	len = strlen (namespace);

	for (iter = device1->properties; iter != NULL; iter = iter->next) {
		HalProperty *p;
		const char *key;
		int type;

		p = (HalProperty *) iter->data;
		key = hal_property_get_key (p);
		type = hal_property_get_type (p);

		if (strncmp (key, namespace, len) != 0)
			continue;

		if (!hal_device_has_property (device2, key))
			return FALSE;

		switch (type) {

		case DBUS_TYPE_STRING:
			if (strcmp (hal_property_get_string (p),
				    hal_device_property_get_string (device2,
								    key)) != 0)
				return FALSE;
			break;

		case DBUS_TYPE_INT32:
			if (hal_property_get_int (p) !=
			    hal_device_property_get_int (device2, key))
				return FALSE;
			break;

		case DBUS_TYPE_BOOLEAN:
			if (hal_property_get_bool (p) !=
			    hal_device_property_get_bool (device2, key))
				return FALSE;
			break;

		case DBUS_TYPE_DOUBLE:
			if (hal_property_get_double (p) !=
			    hal_device_property_get_double (device2, key))
				return FALSE;
			break;

		default:
			HAL_WARNING (("Unknown property type %d", type));
			break;
		}
	}

	return TRUE;
}

const char *
hal_device_get_udi (HalDevice *device)
{
	return device->udi;
}

void
hal_device_set_udi (HalDevice *device, const char *udi)
{
	device->udi = g_strdup (udi);
}

void
hal_device_add_capability (HalDevice *device, const char *capability)
{
        const char *caps;

        caps = hal_device_property_get_string (device, "info.capabilities");

        if (caps == NULL) {
                hal_device_property_set_string (device, "info.capabilities",
                                                capability);
        } else {
                if (hal_device_has_capability (device, capability))
                        return;
                else {
			char *tmp;

			tmp = g_strconcat (caps, " ", capability, NULL);

                        hal_device_property_set_string (device,
                                                        "info.capabilities",
                                                        tmp);

			g_free (tmp);
                }
        }

	g_signal_emit (device, signals[CAPABILITY_ADDED], 0, capability);
}

gboolean
hal_device_has_capability (HalDevice *device, const char *capability)
{
	const char *caps;
	char **split_caps, **iter;
	gboolean matched = FALSE;

	caps = hal_device_property_get_string (device, "info.capabilities");

	if (caps == NULL)
		return FALSE;

	split_caps = g_strsplit (caps, " ", 0);
	for (iter = split_caps; *iter != NULL; iter++) {
		if (strcmp (*iter, capability) == 0) {
			matched = TRUE;
			break;
		}
	}

	g_strfreev (split_caps);

	return matched;
}

gboolean
hal_device_has_property (HalDevice *device, const char *key)
{
	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);

	return hal_device_property_find (device, key) != NULL;
}

int
hal_device_num_properties (HalDevice *device)
{
	g_return_val_if_fail (device != NULL, -1);

	return g_slist_length (device->properties);
}

HalProperty *
hal_device_property_find (HalDevice *device, const char *key)
{
	GSList *iter;

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	for (iter = device->properties; iter != NULL; iter = iter->next) {
		HalProperty *p = iter->data;

		if (strcmp (hal_property_get_key (p), key) == 0)
			return p;
	}

	return NULL;
}

void
hal_device_property_foreach (HalDevice *device,
			     HalDevicePropertyForeachFn callback,
			     gpointer user_data)
{
	GSList *iter;

	g_return_if_fail (device != NULL);
	g_return_if_fail (callback != NULL);

	for (iter = device->properties; iter != NULL; iter = iter->next) {
		HalProperty *p = iter->data;
		gboolean cont;

		cont = callback (device, p, user_data);

		if (cont == FALSE)
			return;
	}
}

int
hal_device_property_get_type (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, DBUS_TYPE_NIL);
	g_return_val_if_fail (key != NULL, DBUS_TYPE_NIL);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_type (prop);
	else
		return DBUS_TYPE_NIL;
}

const char *
hal_device_property_get_string (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_string (prop);
	else
		return NULL;
}

dbus_int32_t
hal_device_property_get_int (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, -1);
	g_return_val_if_fail (key != NULL, -1);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_int (prop);
	else
		return -1;
}

dbus_bool_t
hal_device_property_get_bool (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_bool (prop);
	else
		return -1;
}

double
hal_device_property_get_double (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, -1.0);
	g_return_val_if_fail (key != NULL, -1.0);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_double (prop);
	else
		return -1.0;
}

gboolean
hal_device_property_set_string (HalDevice *device, const char *key,
				const char *value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != DBUS_TYPE_STRING)
			return FALSE;

		/* don't bother setting the same value */
		if (value != NULL &&
		    strcmp (hal_property_get_string (prop), value) == 0)
			return TRUE;

		hal_property_set_string (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

		return TRUE;
	}

	prop = hal_property_new_string (key, value);

	device->properties = g_slist_prepend (device->properties, prop);

	g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
		       key, FALSE, TRUE);

	return TRUE;
}

gboolean
hal_device_property_set_int (HalDevice *device, const char *key,
			     dbus_int32_t value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != DBUS_TYPE_INT32)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_int (prop) == value)
			return TRUE;

		hal_property_set_int (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

		return TRUE;
	}

	prop = hal_property_new_int (key, value);

	device->properties = g_slist_prepend (device->properties, prop);

	g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
		       key, FALSE, TRUE);

	return TRUE;
}

gboolean
hal_device_property_set_bool (HalDevice *device, const char *key,
			     dbus_bool_t value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != DBUS_TYPE_BOOLEAN)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_bool (prop) == value)
			return TRUE;

		hal_property_set_bool (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

		return TRUE;
	}

	prop = hal_property_new_bool (key, value);

	device->properties = g_slist_prepend (device->properties, prop);

	g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
		       key, FALSE, TRUE);

	return TRUE;
}

gboolean
hal_device_property_set_double (HalDevice *device, const char *key,
				double value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != DBUS_TYPE_DOUBLE)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_double (prop) == value)
			return TRUE;

		hal_property_set_double (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

		return TRUE;
	}

	prop = hal_property_new_double (key, value);

	device->properties = g_slist_prepend (device->properties, prop);

	g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
		       key, FALSE, TRUE);

	return TRUE;
}

gboolean
hal_device_property_remove (HalDevice *device, const char *key)
{
	HalProperty *prop;

	prop = hal_device_property_find (device, key);

	if (prop == NULL)
		return FALSE;

	device->properties = g_slist_remove (device->properties, prop);

	hal_property_free (prop);

	g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
		       key, TRUE, FALSE);

	return TRUE;
}

void
hal_device_print (HalDevice *device)
{
	GSList *iter;

        printf ("device udi = %s\n", hal_device_get_udi (device));

	for (iter = device->properties; iter != NULL; iter = iter->next) {
		HalProperty *p = iter->data;
                int type;
                const char *key;

                key = hal_property_get_key (p);
                type = hal_property_get_type (p);

                switch (type) {
                case DBUS_TYPE_STRING:
                        printf ("  %s = '%s'  (string)\n", key,
                                hal_property_get_string (p));
                        break;
 
                case DBUS_TYPE_INT32:
                        printf ("  %s = %d  0x%x  (int)\n", key,
                                hal_property_get_int (p),
                                hal_property_get_int (p));
                        break;
 
                case DBUS_TYPE_DOUBLE:
                        printf ("  %s = %g  (double)\n", key,
                                hal_property_get_double (p));
                        break;
 
                case DBUS_TYPE_BOOLEAN:
                        printf ("  %s = %s  (bool)\n", key,
                                (hal_property_get_bool (p) ? "true" :
                                 "false"));
                        break;
 
                default:
                        HAL_WARNING (("Unknown property type %d", type));
                        break;
                }
        }
        printf ("\n");
}


typedef struct {
	char *key;
	HalDevice *device;
	HalDeviceAsyncCallback callback;
	gpointer user_data;

	guint prop_signal_id;
	guint timeout_id;
} AsyncMatchInfo;

static void
destroy_async_match_info (AsyncMatchInfo *ai)
{
	g_free (ai->key);
	g_signal_handler_disconnect (ai->device, ai->prop_signal_id);
	g_source_remove (ai->timeout_id);
	g_free (ai);
}

static void
prop_changed_cb (HalDevice *device, const char *key,
		 gboolean removed, gboolean added, gpointer user_data)
{
	AsyncMatchInfo *ai = user_data;

	if (strcmp (key, ai->key) != 0)
		return;

	/* the property is no longer there */
	if (removed)
		goto cleanup;


	ai->callback (ai->device, ai->user_data, TRUE);

cleanup:
	destroy_async_match_info (ai);
}


static gboolean
async_wait_timeout (gpointer user_data)
{
	AsyncMatchInfo *ai = (AsyncMatchInfo *) user_data;

	ai->callback (ai->device, ai->user_data, FALSE);

	destroy_async_match_info (ai);

	return FALSE;
}

void
hal_device_async_wait_property (HalDevice    *device,
				const char   *key,
				HalDeviceAsyncCallback callback,
				gpointer     user_data,
				int          timeout)
{
	HalProperty *prop;
	AsyncMatchInfo *ai;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL || timeout==0) {
		callback (device, user_data, prop != NULL);
		return;
	}

	ai = g_new0 (AsyncMatchInfo, 1);

	ai->device = device;
	ai->key = g_strdup (key);
	ai->callback = callback;
	ai->user_data = user_data;

	ai->prop_signal_id = g_signal_connect (device, "property_changed",
					       G_CALLBACK (prop_changed_cb),
					       ai);

	ai->timeout_id = g_timeout_add (timeout, async_wait_timeout, ai);
}
