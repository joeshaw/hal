/***************************************************************************
 * CVSID: $Id$
 *
 * device.c : HalDevice methods
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2004 Novell, Inc.
 *
 * Licensed under the Academic Free License version 2.1
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include "hald.h"
#include "device.h"
#include "hald_marshal.h"
#include "logger.h"
#include "hald_runner.h"

static GSList *locked_devices = NULL;

static void
add_to_locked_set (HalDevice *device)
{
        if (g_slist_find (locked_devices, device) != NULL)
                return;

        locked_devices = g_slist_prepend (locked_devices, device);
}


static void
remove_from_locked_set (HalDevice *device)
{
        locked_devices = g_slist_remove (locked_devices, device);
}

struct _HalProperty {
	int type;
	union {
		char *str_value;
		dbus_int32_t int_value;
 		dbus_uint64_t uint64_value;
		dbus_bool_t bool_value;
		double double_value;
		GSList *strlist_value;
	} v;
};
typedef struct _HalProperty HalProperty;

static inline void
hal_property_free (HalProperty *prop)
{
	if (prop->type == HAL_PROPERTY_TYPE_STRING) {
		g_free (prop->v.str_value);
	} else if (prop->type == HAL_PROPERTY_TYPE_STRLIST) {
		GSList *i;
		for (i = prop->v.strlist_value; i != NULL; i = g_slist_next (i)) {
			g_free (i->data);
		}
		g_slist_free (prop->v.strlist_value);
	}
	g_slice_free (HalProperty, prop);
}

static inline HalProperty *
hal_property_new (int type)
{
	HalProperty *prop;
	prop = g_slice_new0 (HalProperty);
	prop->type = type;
	return prop;
}

static inline int
hal_property_get_type (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, HAL_PROPERTY_TYPE_INVALID);
	return prop->type;
}

static inline const char *
hal_property_get_string (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, NULL);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRING, NULL);
	return prop->v.str_value;
}

static inline dbus_int32_t
hal_property_get_int (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, -1);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_INT32, -1);
	return prop->v.int_value;
}

static inline dbus_uint64_t
hal_property_get_uint64 (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, -1);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_UINT64, -1);
	return prop->v.uint64_value;
}

static inline dbus_bool_t
hal_property_get_bool (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_BOOLEAN, FALSE);
	return prop->v.bool_value;
}

static inline GSList *
hal_property_get_strlist (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, NULL);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, NULL);

	return prop->v.strlist_value;
}

static inline char *
hal_property_to_string (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, NULL);

	switch (prop->type) {
	case HAL_PROPERTY_TYPE_STRING:
		return g_strdup (prop->v.str_value);
	case HAL_PROPERTY_TYPE_INT32:
		return g_strdup_printf ("%d", prop->v.int_value);
	case HAL_PROPERTY_TYPE_UINT64:
		return g_strdup_printf ("%llu", (long long unsigned int) prop->v.uint64_value);
	case HAL_PROPERTY_TYPE_BOOLEAN:
		/* FIXME: Maybe use 1 and 0 here instead? */
		return g_strdup (prop->v.bool_value ? "true" : "false");
	case HAL_PROPERTY_TYPE_DOUBLE:
		return g_strdup_printf ("%f", prop->v.double_value);
	case HAL_PROPERTY_TYPE_STRLIST:
	{
		GSList *iter;
		GString *buf;

		buf = g_string_new ("");
		
		for (iter = hal_property_get_strlist (prop); iter != NULL;
		     iter = g_slist_next (iter)) {
			g_string_append (buf, (const char *) iter->data);
			
			if (g_slist_next (iter) != NULL) {
				g_string_append_c(buf, '\t');
			}
		}
		return g_string_free (buf, FALSE);
	}

	default:
		return NULL;
	}
}

static inline double
hal_property_get_double (HalProperty *prop)
{
	g_return_val_if_fail (prop != NULL, -1.0);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_DOUBLE, -1.0);

	return prop->v.double_value;
}

static inline void
hal_property_set_string (HalProperty *prop, const char *value)
{
	char *endchar;
	gboolean validated = TRUE;

	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == HAL_PROPERTY_TYPE_STRING ||
			  prop->type == HAL_PROPERTY_TYPE_INVALID);

	prop->type = HAL_PROPERTY_TYPE_STRING;
	if (prop->v.str_value != NULL)
		g_free (prop->v.str_value);
	prop->v.str_value = g_strdup (value != NULL ? value : "");

	while (!g_utf8_validate (prop->v.str_value, -1,
				 (const char **) &endchar)) {
		validated = FALSE;
		*endchar = '?';
	}

	if (!validated) {
		HAL_WARNING (("Property has invalid UTF-8 string '%s', it was changed to: '%s'", 
			      value, prop->v.str_value));
	}
}

static inline void
hal_property_set_int (HalProperty *prop, dbus_int32_t value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == HAL_PROPERTY_TYPE_INT32 ||
			  prop->type == HAL_PROPERTY_TYPE_INVALID);
	prop->type = HAL_PROPERTY_TYPE_INT32;
	prop->v.int_value = value;
}

static inline void
hal_property_set_uint64 (HalProperty *prop, dbus_uint64_t value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == HAL_PROPERTY_TYPE_UINT64 ||
			  prop->type == HAL_PROPERTY_TYPE_INVALID);
	prop->type = HAL_PROPERTY_TYPE_UINT64;
	prop->v.uint64_value = value;
}

static inline void
hal_property_set_bool (HalProperty *prop, dbus_bool_t value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == HAL_PROPERTY_TYPE_BOOLEAN ||
			  prop->type == HAL_PROPERTY_TYPE_INVALID);
	prop->type = HAL_PROPERTY_TYPE_BOOLEAN;
	prop->v.bool_value = value;
}

static inline void
hal_property_set_double (HalProperty *prop, double value)
{
	g_return_if_fail (prop != NULL);
	g_return_if_fail (prop->type == HAL_PROPERTY_TYPE_DOUBLE ||
			  prop->type == HAL_PROPERTY_TYPE_INVALID);
	prop->type = HAL_PROPERTY_TYPE_DOUBLE;
	prop->v.double_value = value;
}

static inline gboolean
hal_property_strlist_append (HalProperty *prop, const char *value)
{
	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, FALSE);

	prop->v.strlist_value = g_slist_append (prop->v.strlist_value, g_strdup (value));

	return TRUE;
}

static inline gboolean
hal_property_strlist_prepend (HalProperty *prop, const char *value)
{
	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, FALSE);

	prop->v.strlist_value = g_slist_prepend (prop->v.strlist_value, g_strdup (value));

	return TRUE;
}

static inline gboolean
hal_property_strlist_remove_elem (HalProperty *prop, guint index)
{
	GSList *elem;

	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, FALSE);

	if (prop->v.strlist_value == NULL)
		return FALSE;

	elem = g_slist_nth (prop->v.strlist_value, index);
	if (elem == NULL)
		return FALSE;

	g_free (elem->data);
	prop->v.strlist_value = g_slist_delete_link (prop->v.strlist_value, elem);
	return TRUE;
}


static inline gboolean 
hal_property_strlist_add (HalProperty  *prop, const char *value)
{
	GSList *elem;

	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, FALSE);

	for (elem = prop->v.strlist_value; elem != NULL; elem = g_slist_next (elem)) {
		if (strcmp (elem->data, value) == 0) {
			return FALSE;
		}
	}

	return hal_property_strlist_append (prop, value);
}

static inline gboolean 
hal_property_strlist_remove (HalProperty *prop, const char *value)
{
	guint i;
	GSList *elem;

	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, FALSE);

	for (elem = prop->v.strlist_value, i = 0; elem != NULL; elem = g_slist_next (elem), i++) {
		if (strcmp (elem->data, value) == 0) {
			return hal_property_strlist_remove_elem (prop, i);
		}
	}

	return FALSE;
}

static inline gboolean 
hal_property_strlist_clear (HalProperty *prop)
{
	GSList *elem;

	g_return_val_if_fail (prop != NULL, FALSE);
	g_return_val_if_fail (prop->type == HAL_PROPERTY_TYPE_STRLIST, FALSE);

	for (elem = prop->v.strlist_value; elem != NULL; elem = g_slist_next (elem)) {
		g_free (elem->data);
	}
	g_slist_free (prop->v.strlist_value);

	return TRUE;
}



/****************************************************************************************************************/

static GObjectClass *parent_class;

struct _HalDevicePrivate
{
	char *udi;	
	int num_addons;
	int num_addons_ready;

	GHashTable *props;
};

enum {
	PROPERTY_CHANGED,
	CAPABILITY_ADDED,
        LOCK_ACQUIRED,
        LOCK_RELEASED,
	PRE_PROPERTY_CHANGED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#ifdef HALD_MEMLEAK_DBG
int dbg_hal_device_object_delta = 0;
#endif

static void
hal_device_finalize (GObject *obj)
{
	HalDevice *device = HAL_DEVICE (obj);

	runner_device_finalized (device);

        remove_from_locked_set (device);

#ifdef HALD_MEMLEAK_DBG
	dbg_hal_device_object_delta--;
	printf ("************* in finalize for udi=%s\n", device->private->udi);
#endif

	g_free (device->private->udi);

	g_hash_table_destroy (device->private->props);

	g_free (device->private);

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

	signals[PRE_PROPERTY_CHANGED] =
		g_signal_new ("pre_property_changed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceClass,
					       property_changed),
			      NULL, NULL,
			      hald_marshal_VOID__STRING_BOOL,
			      G_TYPE_NONE, 2,
			      G_TYPE_STRING,
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

	signals[LOCK_ACQUIRED] =
		g_signal_new ("lock_acquired",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceClass,
					       lock_acquired),
			      NULL, NULL,
			      hald_marshal_VOID__STRING_STRING,
			      G_TYPE_NONE, 2,
			      G_TYPE_STRING,
			      G_TYPE_STRING);

	signals[LOCK_RELEASED] =
		g_signal_new ("lock_released",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceClass,
					       lock_released),
			      NULL, NULL,
			      hald_marshal_VOID__STRING_STRING,
			      G_TYPE_NONE, 2,
			      G_TYPE_STRING,
			      G_TYPE_STRING);
}

static void
hal_device_init (HalDevice *device)
{
	static int temp_device_counter = 0;

	device->private = g_new0 (HalDevicePrivate, 1);

	device->private->udi = g_strdup_printf ("/org/freedesktop/Hal/devices/temp/%d",
				       temp_device_counter++);
	device->private->num_addons = 0;
	device->private->num_addons_ready = 0;

	device->private->props = g_hash_table_new_full (g_direct_hash,
							g_direct_equal,
							NULL,
							(GDestroyNotify) hal_property_free);
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
			(GInstanceInitFunc) hal_device_init,
			NULL
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

	device = g_object_new (HAL_TYPE_DEVICE, NULL, NULL);

#ifdef HALD_MEMLEAK_DBG
	dbg_hal_device_object_delta++;
#endif
	return device;
}

static inline HalProperty *
hal_device_property_find (HalDevice *device, const char *key)
{
	GQuark quark = g_quark_try_string (key);

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	if (quark)
	    return g_hash_table_lookup (device->private->props, GINT_TO_POINTER(quark));
	else
	    return NULL;
}

typedef struct 
{
	HalDevice *target;
	const char *source_namespace;
	const char *target_namespace;
	size_t source_ns_len;
} merge_rewrite_ud_t;

static void
merge_device_rewrite_cb (HalDevice *source,
			 const char *key,
			 gpointer user_data)
{
	int type;
	merge_rewrite_ud_t *ud;
	HalProperty *p;
	int target_type;
	gchar *target_key;

	ud = (merge_rewrite_ud_t *) user_data;

	/* only care about properties that match source namespace */
	if (strncmp(key, ud->source_namespace, ud->source_ns_len) != 0)
		goto out;
	
	target_key = g_strdup_printf("%s%s", ud->target_namespace, key + ud->source_ns_len);

	type = hal_device_property_get_type (source, key);

	p = hal_device_property_find (source, key);

	/* only remove target if it exists with a different type */
	target_type = hal_device_property_get_type (ud->target, target_key);
	if (target_type != HAL_PROPERTY_TYPE_INVALID && target_type != type) {
		hal_device_property_remove (ud->target, target_key);
	}

	switch (type) {
	case HAL_PROPERTY_TYPE_STRING:
		hal_device_property_set_string (ud->target, target_key, hal_property_get_string (p));
		break;
		
	case HAL_PROPERTY_TYPE_INT32:
		hal_device_property_set_int (ud->target, target_key, hal_property_get_int (p));
		break;
		
	case HAL_PROPERTY_TYPE_UINT64:
		hal_device_property_set_uint64 (ud->target, target_key, hal_property_get_uint64 (p));
		break;
		
	case HAL_PROPERTY_TYPE_BOOLEAN:
		hal_device_property_set_bool (ud->target, target_key, hal_property_get_bool (p));
		break;
		
	case HAL_PROPERTY_TYPE_DOUBLE:
		hal_device_property_set_double (ud->target, target_key, hal_property_get_double (p));
		break;

	case HAL_PROPERTY_TYPE_STRLIST:
		{
			GSList *l;

			hal_device_property_strlist_clear (ud->target, target_key, FALSE);
			for (l = hal_property_get_strlist (p); l != NULL; l = l->next)
				hal_device_property_strlist_append (ud->target, target_key, l->data, FALSE);
		}
		break;

	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

	g_free (target_key);
out:
	;
}

/**  
 *  hal_device_merge_with_rewrite:
 *  @target:             Device to put properties onto
 *  @source:             Device to retrieve properties from
 *  @target_namespace:   Replace source namespace with this namespace
 *  @source_namespace:   Source namespace that property keys must match
 *
 *  Merge all properties from source where the key starts with 
 *  source_namespace and put them onto target replacing source_namespace
 *  with target_namespace
 */
void
hal_device_merge_with_rewrite  (HalDevice    *target,
				HalDevice    *source,
				const char   *target_namespace,
				const char   *source_namespace)
{
	merge_rewrite_ud_t ud;

	ud.target = target;
	ud.source_namespace = source_namespace;
	ud.target_namespace = target_namespace;
	ud.source_ns_len = strlen (source_namespace);

	/* doesn't handle info.capabilities - TODO: should use atomic update? */
	/* device_property_atomic_update_begin (); */

	hal_device_property_foreach (source, merge_device_rewrite_cb, &ud);

	/* device_property_atomic_update_end (); */
}

static inline GSList *
hal_device_property_get_strlist (HalDevice    *device, 
				 const char   *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	prop = hal_device_property_find (device, key);
	if (prop != NULL)
		return hal_property_get_strlist (prop);
	else
		return NULL;
}

guint
hal_device_property_get_strlist_length (HalDevice    *device,
					const char   *key)
{
	GSList *i;

	i = hal_device_property_get_strlist (device, key);
	if (i != NULL)
		return g_slist_length (i);
	else
		return 0;
}

gboolean
hal_device_property_strlist_contains (HalDevice  *device,
				      const char *key,
				      const char *value)
{
	GSList *i;
	GSList *elems;
	gboolean ret;

	ret = FALSE;

	elems = hal_device_property_get_strlist (device, key);
	for (i = elems; i != NULL; i = g_slist_next (i)) {
		if (strcmp (i->data, value) == 0) {
			ret = TRUE;
			break;
		}
	}

	return ret;
}


void
hal_device_property_strlist_iter_init (HalDevice    *device,
				       const char   *key,
				       HalDeviceStrListIter *iter)
{
	HalProperty *prop;

	g_return_if_fail (device != NULL);
	g_return_if_fail (key != NULL);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		iter->i = hal_property_get_strlist (prop);
	else
		iter->i = NULL;
}

const char *
hal_device_property_strlist_iter_get_value (HalDeviceStrListIter *iter)
{
	g_return_val_if_fail (iter != NULL, NULL);
	g_return_val_if_fail (iter->i != NULL, NULL);
	return iter->i->data;
}

void
hal_device_property_strlist_iter_next (HalDeviceStrListIter *iter)
{
	g_return_if_fail (iter != NULL);
	g_return_if_fail (iter->i != NULL);
	iter->i = g_slist_next (iter->i);
}

gboolean
hal_device_property_strlist_iter_is_valid (HalDeviceStrListIter *iter)
{
	g_return_val_if_fail (iter != NULL, FALSE);
	if (iter->i == NULL) {
		return FALSE;
	} else {
		return TRUE;
	}
}

char **
hal_device_property_dup_strlist_as_strv (HalDevice    *device,
					 const char   *key)
{
	guint j;
	guint len;
	gchar **strv;
	GSList *i;

	strv = NULL;

	i = hal_device_property_get_strlist (device, key);
	if (i == NULL)
		goto out;

	len = g_slist_length (i);
	if (len == 0)
		goto out;

	strv = g_new (char *, len + 1);

	for (j = 0; i != NULL; i = g_slist_next (i), j++) {
		strv[j] = g_strdup ((const gchar *) i->data);
	}
	strv[j] = NULL;

out:
	return strv;	
}

const char *
hal_device_get_udi (HalDevice *device)
{
	return device->private->udi;
}

void
hal_device_set_udi (HalDevice *device, const char *udi)
{
	if (device->private->udi != NULL)
		g_free (device->private->udi);
	device->private->udi = g_strdup (udi);
	hal_device_property_set_string (device, "info.udi", udi);
}

void
hal_device_add_capability (HalDevice *device, const char *capability)
{
	if (hal_device_property_strlist_add (device, "info.capabilities", capability))
		g_signal_emit (device, signals[CAPABILITY_ADDED], 0, capability);
}

gboolean
hal_device_has_capability (HalDevice *device, const char *capability)
{
	GSList *caps;
	GSList *iter;
	gboolean matched = FALSE;

	caps = hal_device_property_get_strlist (device, "info.capabilities");

	if (caps == NULL)
		return FALSE;

	for (iter = caps; iter != NULL; iter = iter->next) {
		if (strcmp (iter->data, capability) == 0) {
			matched = TRUE;
			break;
		}
	}

	return matched;
}

int
hal_device_num_properties (HalDevice *device)
{
	g_return_val_if_fail (device != NULL, -1);

	return g_hash_table_size (device->private->props);
}

gboolean
hal_device_has_property (HalDevice *device, const char *key)
{
	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	
	return hal_device_property_find (device, key) != NULL;
}

char *
hal_device_property_to_string (HalDevice *device, const char *key)
{
	HalProperty *prop;

	prop = hal_device_property_find (device, key);
	if (!prop)
		return NULL;

	return hal_property_to_string (prop);
}

typedef struct 
{
	HalDevice *device;
	HalDevicePropertyForeachFn callback;
	gpointer user_data;
} hdpfe_ud_t;

static void
hdpfe (gpointer key,
       gpointer value,
       gpointer user_data)
{
	hdpfe_ud_t *c;
	HalProperty *prop;
	const gchar *key_string = g_quark_to_string ((GQuark) GPOINTER_TO_UINT(key));

	c = (hdpfe_ud_t *) user_data;
	prop = (HalProperty *) value;

	c->callback (c->device, key_string, c->user_data);
}

void
hal_device_property_foreach (HalDevice *device,
			     HalDevicePropertyForeachFn callback,
			     gpointer user_data)
{
	hdpfe_ud_t c;

	g_return_if_fail (device != NULL);
	g_return_if_fail (callback != NULL);

	c.device = device;
	c.callback = callback;
	c.user_data = user_data;

	g_hash_table_foreach (device->private->props, hdpfe, &c);
}

int
hal_device_property_get_type (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, HAL_PROPERTY_TYPE_INVALID);
	g_return_val_if_fail (key != NULL, HAL_PROPERTY_TYPE_INVALID);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_type (prop);
	else
		return HAL_PROPERTY_TYPE_INVALID;
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

const char *
hal_device_property_get_as_string (HalDevice *device, const char *key, char *buf, size_t bufsize)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);
	g_return_val_if_fail (buf != NULL, NULL);

	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		switch (hal_property_get_type (prop)) {
		case HAL_PROPERTY_TYPE_STRING:
			strncpy (buf, hal_property_get_string (prop), bufsize);
			break;
		case HAL_PROPERTY_TYPE_INT32:
			snprintf (buf, bufsize, "%d", hal_property_get_int (prop));
			break;
		case HAL_PROPERTY_TYPE_UINT64:
			snprintf (buf, bufsize, "%llu", (long long unsigned int) hal_property_get_uint64 (prop));
			break;
		case HAL_PROPERTY_TYPE_DOUBLE:
			snprintf (buf, bufsize, "%f", hal_property_get_double (prop));
			break;
		case HAL_PROPERTY_TYPE_BOOLEAN:
			strncpy (buf, hal_property_get_bool (prop) ? "true" : "false", bufsize);
			break;

		case HAL_PROPERTY_TYPE_STRLIST:
			/* print out as "\tval1\tval2\val3\t" */
		        {
				GSList *iter;
				guint i;

				if (bufsize > 0)
					buf[0] = '\t';
				i = 1;
				for (iter = hal_property_get_strlist (prop); 
				     iter != NULL && i < bufsize; 
				     iter = g_slist_next (iter)) {
					guint len;
					const char *str;
					
					str = (const char *) iter->data;
					len = strlen (str);
					strncpy (buf + i, str, bufsize - i);
					i += len;

					if (i < bufsize) {
						buf[i] = '\t';
						i++;
					}
				}
			}
			break;
		}
		return buf;
	} else {
		buf[0] = '\0';
		return NULL;
	}
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

dbus_uint64_t
hal_device_property_get_uint64 (HalDevice *device, const char *key)
{
	HalProperty *prop;

	g_return_val_if_fail (device != NULL, -1);
	g_return_val_if_fail (key != NULL, -1);

	prop = hal_device_property_find (device, key);

	if (prop != NULL)
		return hal_property_get_uint64 (prop);
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
		return FALSE;
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
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRING)
			return FALSE;

		/* don't bother setting the same value */
		if (strcmp (hal_property_get_string (prop), value != NULL ? value : "") == 0)
			return TRUE;

		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, FALSE);

		hal_property_set_string (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRING);
		hal_property_set_string (prop, value);

		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

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
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_INT32)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_int (prop) == value)
			return TRUE;

		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, FALSE);

		hal_property_set_int (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_INT32);
		hal_property_set_int (prop, value);
		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

	return TRUE;
}

gboolean
hal_device_property_set_uint64 (HalDevice *device, const char *key,
			     dbus_uint64_t value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_UINT64)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_uint64 (prop) == value)
			return TRUE;

		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, FALSE);

		hal_property_set_uint64 (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_UINT64);
		hal_property_set_uint64 (prop, value);
		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

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
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_BOOLEAN)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_bool (prop) == value)
			return TRUE;

		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, FALSE);

		hal_property_set_bool (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_BOOLEAN);
		hal_property_set_bool (prop, value);
		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

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
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_DOUBLE)
			return FALSE;

		/* don't bother setting the same value */
		if (hal_property_get_double (prop) == value)
			return TRUE;

		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, FALSE);

		hal_property_set_double (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_DOUBLE);
		hal_property_set_double (prop, value);
		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

	return TRUE;
}

gboolean
hal_device_property_set_strlist (HalDevice *device, const char *key,
			 	 GSList *value)
{
	HalProperty *prop;
	GSList *l;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		GSList *current;

		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST) 
			return FALSE;

		/* don't bother setting the same value */
		current = hal_property_get_strlist (prop);
		if (g_slist_length (value) == g_slist_length (current)) {
			gboolean equal = TRUE;		
			for (l = value; l != NULL; l = l->next, current = current->next) {
				if (strcmp (l->data, current->data) != 0) {
 					equal = FALSE;
					break;
				} 
			}
			if (equal) return TRUE;
		}

		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, FALSE);

		/* TODO: check why  hal_property_strlist_clear (prop) not work and why
		 *       we need to remove the key and a new one to get this running:
		 *	 - multiple copy calls mixed with e.g. append rules   
		 */

		g_hash_table_remove (device->private->props, GINT_TO_POINTER(g_quark_from_string(key)));
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
		for (l = value ; l != NULL; l = l->next) {
			hal_property_strlist_append (prop, l->data);
		}
		g_hash_table_insert (device->private->props, GINT_TO_POINTER(g_quark_from_string (key)),
				     prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
		for (l = value ; l != NULL; l = l->next) {
			hal_property_strlist_append (prop, l->data);
		}
		g_hash_table_insert (device->private->props, GINT_TO_POINTER(g_quark_from_string (key)),
				     prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

	return TRUE;
}


gboolean
hal_device_copy_property (HalDevice *from_device, const char *from, HalDevice *to_device, const char *to)
{
	gboolean rc;

	rc = FALSE;

	if (hal_device_has_property (from_device, from)) {
		switch (hal_device_property_get_type (from_device, from)) {
		case HAL_PROPERTY_TYPE_STRING:
			rc = hal_device_property_set_string (
				to_device, to, hal_device_property_get_string (from_device, from));
			break;
		case HAL_PROPERTY_TYPE_INT32:
			rc = hal_device_property_set_int (
				to_device, to, hal_device_property_get_int (from_device, from));
			break;
		case HAL_PROPERTY_TYPE_UINT64:
			rc = hal_device_property_set_uint64 (
				to_device, to, hal_device_property_get_uint64 (from_device, from));
			break;
		case HAL_PROPERTY_TYPE_BOOLEAN:
			rc = hal_device_property_set_bool (
				to_device, to, hal_device_property_get_bool (from_device, from));
			break;
		case HAL_PROPERTY_TYPE_DOUBLE:
			rc = hal_device_property_set_double (
				to_device, to, hal_device_property_get_double (from_device, from));
			break;
		case HAL_PROPERTY_TYPE_STRLIST:
			rc = hal_device_property_set_strlist(
				to_device, to, hal_device_property_get_strlist (from_device, from));
			break;
		break;
		}
	}

	return rc;
}

gboolean
hal_device_property_remove (HalDevice *device, const char *key)
{
	GQuark quark = g_quark_try_string (key);
	if (quark && g_hash_table_lookup (device->private->props, GINT_TO_POINTER(quark))) {
		g_signal_emit (device, signals[PRE_PROPERTY_CHANGED], 0,
			       key, TRUE);

		if (g_hash_table_remove (device->private->props,
					 GINT_TO_POINTER(quark))) {
			g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
				       key, TRUE, FALSE);
			return TRUE;
		}
	}
	return FALSE;
}

static void
hal_device_print_foreach_cb (HalDevice *device,
			     const char *key,
			     gpointer user_data)
{
	int type;
	HalProperty *p;

	p = hal_device_property_find (device, key);
	if (p == NULL) {
		goto out;
	}

	type = hal_property_get_type (p);
	
	switch (type) {
	case HAL_PROPERTY_TYPE_STRING:
		fprintf (stderr, "  %s = '%s'  (string)\n", key,
			 hal_property_get_string (p));
		break;
		
	case HAL_PROPERTY_TYPE_INT32:
                        fprintf (stderr, "  %s = %d  0x%x  (int)\n", key,
				 hal_property_get_int (p),
				 hal_property_get_int (p));
                        break;
			
	case HAL_PROPERTY_TYPE_UINT64:
		fprintf (stderr, "  %s = %llu  0x%llx  (uint64)\n", key,
			 (long long unsigned int) hal_property_get_uint64 (p),
			 (long long unsigned int) hal_property_get_uint64 (p));
		break;
		
	case HAL_PROPERTY_TYPE_DOUBLE:
		fprintf (stderr, "  %s = %g  (double)\n", key,
			 hal_property_get_double (p));
		break;
		
	case HAL_PROPERTY_TYPE_BOOLEAN:
		fprintf (stderr, "  %s = %s  (bool)\n", key,
			 (hal_property_get_bool (p) ? "true" :
			  "false"));
		break;
		
		/* TODO: strlist */
		
	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

out:
	;
}

void
hal_device_print (HalDevice *device)
{
        fprintf (stderr, "device udi = %s\n", hal_device_get_udi (device));
	hal_device_property_foreach (device, hal_device_print_foreach_cb, NULL);
        fprintf (stderr, "\n");
}

const char *
hal_device_property_get_strlist_elem (HalDevice    *device,
				      const char   *key,
				      guint index)
{
	GSList *strlist;
	GSList *i;

	strlist = hal_device_property_get_strlist (device, key);
	if (strlist == NULL)
		return NULL;

	i = g_slist_nth (strlist, index);
	if (i == NULL)
		return NULL;

	return (const char *) i->data;
}

gboolean
hal_device_property_strlist_append (HalDevice    *device,
				    const char   *key,
				    const char   *value,
				    gboolean     changeset)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
			return FALSE;

		hal_property_strlist_append (prop, value);
		
		if (!changeset) 
			g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
				       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
		hal_property_strlist_append (prop, value);

		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		if (!changeset)
			g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
				       key, FALSE, TRUE);
	}

	return TRUE;
}

gboolean
hal_device_property_strlist_append_finish_changeset (HalDevice    *device,
                                                     const char   *key,
                                                     gboolean     is_added){

	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
			return FALSE;

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, is_added);

	} else {
		return FALSE;
	}

	return TRUE;
}


gboolean 
hal_device_property_strlist_prepend (HalDevice    *device,
				     const char   *key,
				     const char *value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
			return FALSE;

		hal_property_strlist_prepend (prop, value);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
		hal_property_strlist_prepend (prop, value);

		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);
	}

	return TRUE;
}

gboolean
hal_device_property_strlist_remove_elem (HalDevice    *device,
					 const char   *key,
					 guint index)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop == NULL)
		return FALSE;

	if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
		return FALSE;
	
	if (hal_property_strlist_remove_elem (prop, index)) {
		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);
		return TRUE;
	}
	
	return FALSE;
}

gboolean
hal_device_property_strlist_clear (HalDevice    *device,
				   const char   *key,
				   gboolean     changeset)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop == NULL) {
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		if (!changeset)
			g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
				       key, FALSE, TRUE);
		return TRUE;
	}

	if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
		return FALSE;
	
        /* TODO: check why  hal_property_strlist_clear (prop) not work */ 
	g_hash_table_remove (device->private->props, GINT_TO_POINTER(g_quark_from_string(key)));
	prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
	g_hash_table_insert (device->private->props, GINT_TO_POINTER(g_quark_from_string(key)), prop);

	if (!changeset) {
		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);
	}

	return TRUE;
}


gboolean
hal_device_property_strlist_add (HalDevice *device,
				 const char *key,
				 const char *value)
{
	HalProperty *prop;
	gboolean res;

	res = FALSE;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop != NULL) {
		if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
			goto out;

		res = hal_property_strlist_add (prop, value);
		if (res) {
			g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
				       key, FALSE, FALSE);
		}

	} else {
		prop = hal_property_new (HAL_PROPERTY_TYPE_STRLIST);
		hal_property_strlist_prepend (prop, value);

		g_hash_table_insert (device->private->props,
			GINT_TO_POINTER(g_quark_from_string (key)), prop);

		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, TRUE);

		res = TRUE;
	}

out:
	return res;
}

gboolean
hal_device_property_strlist_remove (HalDevice *device,
				    const char *key,
				    const char *value)
{
	HalProperty *prop;

	/* check if property already exists */
	prop = hal_device_property_find (device, key);

	if (prop == NULL)
		return FALSE;

	if (hal_property_get_type (prop) != HAL_PROPERTY_TYPE_STRLIST)
		return FALSE;
	
	if (hal_property_strlist_remove (prop, value)) {
		g_signal_emit (device, signals[PROPERTY_CHANGED], 0,
			       key, FALSE, FALSE);
	}
	
	return TRUE;
}

gboolean
hal_device_property_strlist_is_empty (HalDevice    *device,
				      const char   *key)
{
	GSList *strlist;

	if ( hal_device_has_property (device, key)) {
		strlist = hal_device_property_get_strlist (device, key);
		if (strlist == NULL ) 
			return TRUE;

		if (g_slist_length (strlist) > 0) 
			return FALSE;
		else 
			return TRUE;
	}
	return FALSE;
}

void
hal_device_inc_num_addons (HalDevice *device)
{
	device->private->num_addons++;
}

gboolean
hal_device_inc_num_ready_addons (HalDevice *device)
{
	if (hal_device_are_all_addons_ready (device)) {
		HAL_ERROR (("In hal_device_inc_num_ready_addons for udi=%s but all addons are already ready!", 
			    device->private->udi));
		return FALSE;
	}

	device->private->num_addons_ready++;
	return TRUE;
}

gboolean
hal_device_are_all_addons_ready (HalDevice *device)
{
	if (device->private->num_addons_ready == device->private->num_addons) {
		return TRUE;
	} else {
		return FALSE;
	}
}

/**
 * hal_device_acquire_lock:
 * @device: the device to acquire a lock on
 * @lock_name: name of the lock
 * @sender: the caller to acquire the lock
 *
 * Acquires a named lock on a device.
 *
 * Returns: FALSE if the caller already holds this lock. TRUE if the caller got the lock.
 */
gboolean
hal_device_acquire_lock (HalDevice *device, const char *lock_name, gboolean exclusive, const char *sender)
{
        gboolean ret;
        char buf[256];

        ret = FALSE;

	g_snprintf (buf, sizeof (buf), "info.named_locks.%s.exclusive", lock_name);
	if (hal_device_property_get_bool (device, buf) == TRUE) {
            /* exclusively locked */
            goto out;
        }
	hal_device_property_set_bool (device, buf, exclusive);

	g_snprintf (buf, sizeof (buf), "info.named_locks.%s.dbus_name", lock_name);
        if (exclusive && hal_device_has_property (device, buf)) {
                /* cannot obtain exclusive lock */
                goto out;
        }
	if (hal_device_property_strlist_contains (device, buf, sender)) {
                /* already locked */
                goto out;
        }

	hal_device_property_strlist_append (device, buf, sender, FALSE);
	g_snprintf (buf, sizeof (buf), "info.named_locks.%s.locked", lock_name);
	hal_device_property_set_bool (device, buf, TRUE);

	hal_device_property_strlist_add (device, "info.named_locks", lock_name);

        add_to_locked_set (device);

        g_signal_emit (device, signals[LOCK_ACQUIRED], 0, lock_name, sender);

        ret = TRUE;
out:
        return ret;
}

/**
 * hal_device_release_lock:
 * @device: the device to acquire a lock on
 * @lock_name: name of the lock
 * @sender: the caller to acquire the lock
 *
 * Releases a named lock on a device.
 *
 * Returns: FALSE if the caller didn't hold this lock. True if the lock was removed.
 */
gboolean
hal_device_release_lock (HalDevice *device, const char *lock_name, const char *sender)
{
        gboolean ret;
        char buf[256];

        ret = FALSE;

        HAL_INFO (("Removing lock '%s' from '%s'", lock_name, sender));

	g_snprintf (buf, sizeof (buf), "info.named_locks.%s.locked", lock_name);
	if (!hal_device_property_get_bool (device, buf)) {
                /* not locked */
                goto out;
	}

	g_snprintf (buf, sizeof (buf), "info.named_locks.%s.dbus_name", lock_name);
	if (!hal_device_property_strlist_contains (device, buf, sender)) {
                /* not locked by sender */
                goto out;
	}

	if (hal_device_property_get_strlist_length (device, buf) == 1) {
                /* last one to hold the lock */
		g_snprintf (buf, sizeof (buf), "info.named_locks.%s.exclusive", lock_name);
		hal_device_property_remove (device, buf);
		g_snprintf (buf, sizeof (buf), "info.named_locks.%s.locked", lock_name);
		hal_device_property_remove (device, buf);
		g_snprintf (buf, sizeof (buf), "info.named_locks.%s.dbus_name", lock_name);
		hal_device_property_remove (device, buf);

                if (hal_device_property_get_strlist_length (device, "info.named_locks") == 1) {
                        hal_device_property_remove (device, "info.named_locks");
                        remove_from_locked_set (device);
                } else {
                        hal_device_property_strlist_remove (device, "info.named_locks", lock_name);
                }
	} else {
                /* there are additional holders */
		hal_device_property_strlist_remove (device, buf, sender);
	}

        g_signal_emit (device, signals[LOCK_RELEASED], 0, lock_name, sender);

        ret = TRUE;

out:
        return ret;
}

/**
 * hal_device_get_lock_holders:
 * @device: the device to check for
 * @lock_name: the lock name
 *
 * Get the lock holders on a device.
 *
 * Returns: NULL if there are no lock holders; otherwise a
 * NULL-terminated array of strings with the dbus names of the
 * holders. Caller must free this with g_strfreev().
 */
char **
hal_device_get_lock_holders (HalDevice *device, const char *lock_name)
{
        char **ret;
        char buf[256];
        
        g_snprintf (buf, sizeof (buf), "info.named_locks.%s.dbus_name", lock_name);
        ret = hal_device_property_dup_strlist_as_strv (device, buf);
        return ret;
}

/**
 * hal_device_get_lock_holders:
 * @device: the device to check for
 * @lock_name: the lock name
 *
 * Get the number of lock holders on a device.
 *
 * Returns: Number of callers holding the given lock.
 */
int 
hal_device_get_num_lock_holders (HalDevice *device, const char *lock_name)
{
        int num;
        char **holders;

        num = 0;
        holders = hal_device_get_lock_holders (device, lock_name);
        if (holders == NULL)
                goto out;

        num = g_strv_length (holders);
        g_strfreev (holders);
out:
        return num;
}

/**
 * hal_device_client_disconnected:
 * @sender: the client that disconnected from the bus
 *
 * Will remove locks held by this client on locked devices. This is a
 * static class method that affects all devices that are locked.
 *
 */
void
hal_device_client_disconnected (const char *sender)
{
        GSList *i;

        HAL_INFO (("Removing locks from '%s'", sender));

        for (i = locked_devices; i != NULL; i = g_slist_next (i)) {
                HalDevice *device = i->data;
                char **locks;
                int n;

                HAL_INFO (("Looking at udi '%s'", device->private->udi));

                locks = hal_device_property_dup_strlist_as_strv (device, "info.named_locks");
                if (locks != NULL) {
                        for (n = 0; locks[n] != NULL; n++) {
                                char *lock_name = locks[n];
                                HAL_INFO (("Lock '%s'", lock_name));
                                hal_device_release_lock (device, lock_name, sender);
                        }
                        g_strfreev (locks);
                }
        }
}

gboolean 
hal_device_is_lock_exclusive (HalDevice *device, const char *lock_name)
{
        char buf[256];
	g_snprintf (buf, sizeof (buf), "info.named_locks.%s.exclusive", lock_name);
	return hal_device_property_get_bool (device, buf);
}

