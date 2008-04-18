/***************************************************************************
 * CVSID: $Id$
 *
 * device_store.c : HalDeviceStore methods
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

#include "device_store.h"
#include "hald_marshal.h"
#include "logger.h"

static GObjectClass *parent_class;

enum {
	STORE_CHANGED,
	DEVICE_PROPERTY_CHANGED,
	DEVICE_CAPABILITY_ADDED,
        DEVICE_LOCK_ACQUIRED,
        DEVICE_LOCK_RELEASED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
hal_device_store_finalize (GObject *obj)
{
	HalDeviceStore *store = HAL_DEVICE_STORE (obj);

	g_slist_foreach (store->devices, (GFunc) g_object_unref, NULL);

	if (parent_class->finalize)
		parent_class->finalize (obj);
}

static void
hal_device_store_class_init (HalDeviceStoreClass *klass)
{
	GObjectClass *obj_class = (GObjectClass *) klass;

	parent_class = g_type_class_peek_parent (klass);

	obj_class->finalize = hal_device_store_finalize;

	signals[STORE_CHANGED] =
		g_signal_new ("store_changed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceStoreClass,
					       store_changed),
			      NULL, NULL,
			      hald_marshal_VOID__OBJECT_BOOL,
			      G_TYPE_NONE, 2,
			      G_TYPE_OBJECT,
			      G_TYPE_BOOLEAN);

	signals[DEVICE_PROPERTY_CHANGED] =
		g_signal_new ("device_property_changed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceStoreClass,
					       device_property_changed),
			      NULL, NULL,
			      hald_marshal_VOID__OBJECT_STRING_BOOL_BOOL,
			      G_TYPE_NONE, 4,
			      G_TYPE_OBJECT,
			      G_TYPE_STRING,
			      G_TYPE_BOOLEAN,
			      G_TYPE_BOOLEAN);

	signals[DEVICE_CAPABILITY_ADDED] =
		g_signal_new ("device_capability_added",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceStoreClass,
					       device_capability_added),
			      NULL, NULL,
			      hald_marshal_VOID__OBJECT_STRING,
			      G_TYPE_NONE, 2,
			      G_TYPE_OBJECT,
			      G_TYPE_STRING);

	signals[DEVICE_LOCK_ACQUIRED] =
		g_signal_new ("device_lock_acquired",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceStoreClass,
					       device_lock_acquired),
			      NULL, NULL,
			      hald_marshal_VOID__OBJECT_STRING_STRING,
			      G_TYPE_NONE, 3,
			      G_TYPE_OBJECT,
			      G_TYPE_STRING,
			      G_TYPE_STRING);

	signals[DEVICE_LOCK_RELEASED] =
		g_signal_new ("device_lock_released",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (HalDeviceStoreClass,
					       device_lock_released),
			      NULL, NULL,
			      hald_marshal_VOID__OBJECT_STRING_STRING,
			      G_TYPE_NONE, 3,
			      G_TYPE_OBJECT,
			      G_TYPE_STRING,
			      G_TYPE_STRING);

}

static void
hal_device_store_init (HalDeviceStore *device)
{
	device->property_index = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

GType
hal_device_store_get_type (void)
{
	static GType type = 0;
	
	if (!type) {
		static GTypeInfo type_info = {
			sizeof (HalDeviceStoreClass),
			NULL, NULL,
			(GClassInitFunc) hal_device_store_class_init,
			NULL, NULL,
			sizeof (HalDeviceStore),
			0,
			(GInstanceInitFunc) hal_device_store_init
		};

		type = g_type_register_static (G_TYPE_OBJECT,
					       "HalDeviceStore",
					       &type_info,
					       0);
	}

	return type;
}

HalDeviceStore *
hal_device_store_new (void)
{
	HalDeviceStore *store;

	store = g_object_new (HAL_TYPE_DEVICE_STORE, NULL, NULL);

	return store;
}

static void
property_index_check_all (HalDeviceStore *store, HalDevice *device, gboolean add);

static void
property_index_modify_string (HalDeviceStore *store, HalDevice *device,
			      const char *key, gboolean added);

static void
device_pre_property_changed (HalDevice *device,
			      const char *key,
			      gboolean removed,
			      gpointer data)
{
	HalDeviceStore *store = HAL_DEVICE_STORE (data);

	if (hal_device_property_get_type (device, key) == HAL_PROPERTY_TYPE_STRING) {
		property_index_modify_string(store, device, key, FALSE);
	}
}


static void
emit_device_property_changed (HalDevice *device,
			      const char *key,
			      gboolean added,
			      gboolean removed,
			      gpointer data)
{
	HalDeviceStore *store = HAL_DEVICE_STORE (data);

	if (hal_device_property_get_type (device, key) == HAL_PROPERTY_TYPE_STRING) {
		property_index_modify_string(store, device, key, TRUE);
	}

	g_signal_emit (store, signals[DEVICE_PROPERTY_CHANGED], 0,
		       device, key, added, removed);
}

static void
emit_device_capability_added (HalDevice *device,
			      const char *capability,
			      gpointer data)
{
	HalDeviceStore *store = HAL_DEVICE_STORE (data);

	g_signal_emit (store, signals[DEVICE_CAPABILITY_ADDED], 0,
		       device, capability);
}

static void
emit_device_lock_acquired (HalDevice *device,
                           const char *lock_name,
                           const char *lock_owner,
                           gpointer data)
{
	HalDeviceStore *store = HAL_DEVICE_STORE (data);

	g_signal_emit (store, signals[DEVICE_LOCK_ACQUIRED], 0,
		       device, lock_name, lock_owner);
}

static void
emit_device_lock_released (HalDevice *device,
                           const char *lock_name,
                           const char *lock_owner,
                           gpointer data)
{
	HalDeviceStore *store = HAL_DEVICE_STORE (data);

	g_signal_emit (store, signals[DEVICE_LOCK_RELEASED], 0,
		       device, lock_name, lock_owner);
}

void
hal_device_store_add (HalDeviceStore *store, HalDevice *device)
{
	const char buf[] = "/org/freedesktop/Hal/devices/";

	if (strncmp(hal_device_get_udi (device), buf, sizeof (buf) - 1) != 0) {
		
		HAL_ERROR(("Can't add HalDevice with incorrect UDI. Valid "
			   "UDI must start with '/org/freedesktop/Hal/devices/'"));
		goto out;
	}
	store->devices = g_slist_prepend (store->devices,
					  g_object_ref (device));

	g_signal_connect (device, "property_changed",
			  G_CALLBACK (emit_device_property_changed), store);
	g_signal_connect (device, "pre_property_changed",
			  G_CALLBACK (device_pre_property_changed), store);
	g_signal_connect (device, "capability_added",
			  G_CALLBACK (emit_device_capability_added), store);
	g_signal_connect (device, "lock_acquired",
			  G_CALLBACK (emit_device_lock_acquired), store);
	g_signal_connect (device, "lock_released",
			  G_CALLBACK (emit_device_lock_released), store);

	property_index_check_all (store, device, TRUE);
	g_signal_emit (store, signals[STORE_CHANGED], 0, device, TRUE);

out:
	;
}

gboolean
hal_device_store_remove (HalDeviceStore *store, HalDevice *device)
{
	if (!g_slist_find (store->devices, device))
		return FALSE;

	store->devices = g_slist_remove (store->devices, device);

	g_signal_handlers_disconnect_by_func (device,
					      (gpointer)emit_device_property_changed,
					      store);
	g_signal_handlers_disconnect_by_func (device,
					      (gpointer)device_pre_property_changed,
					      store);
	g_signal_handlers_disconnect_by_func (device,
					      (gpointer)emit_device_capability_added,
					      store);
	g_signal_handlers_disconnect_by_func (device,
					      (gpointer)emit_device_lock_acquired,
					      store);
	g_signal_handlers_disconnect_by_func (device,
					      (gpointer)emit_device_lock_released,
					      store);

	property_index_check_all (store, device, FALSE);

	g_signal_emit (store, signals[STORE_CHANGED], 0, device, FALSE);

	g_object_unref (device);

	return TRUE;
}

HalDevice *
hal_device_store_find (HalDeviceStore *store, const char *udi)
{
	GSList *iter;

	for (iter = store->devices; iter != NULL; iter = iter->next) {
		HalDevice *d = iter->data;

		if (strcmp (hal_device_get_udi (d), udi) == 0)
			return d;
	}

	return NULL;
}

void
hal_device_store_foreach (HalDeviceStore *store,
			  HalDeviceStoreForeachFn callback,
			  gpointer user_data)
{
	GSList *iter;

	g_return_if_fail (store != NULL);
	g_return_if_fail (callback != NULL);

	for (iter = store->devices; iter != NULL; iter = iter->next) {
		HalDevice *d = HAL_DEVICE (iter->data);
		gboolean cont;

		cont = callback (store, d, user_data);

		if (cont == FALSE)
			return;
	}
}

static gboolean
hal_device_store_print_foreach_fn (HalDeviceStore *store,
				   HalDevice *device,
				   gpointer user_data)
{
	fprintf (stderr, "----\n");
	hal_device_print (device);
	fprintf (stderr, "----\n");
	return TRUE;
}

void 
hal_device_store_print (HalDeviceStore *store)
{
	fprintf (stderr, "===============================================\n");
        fprintf (stderr, "Dumping %u devices\n", 
		 g_slist_length (store->devices));
	fprintf (stderr, "===============================================\n");
	hal_device_store_foreach (store, 
				  hal_device_store_print_foreach_fn, 
				  NULL);
	fprintf (stderr, "===============================================\n");
}

HalDevice *
hal_device_store_match_key_value_string (HalDeviceStore *store,
					 const char *key,
					 const char *value)
{
	GSList *iter;
	GSList *devices;
	GHashTable *index;

	g_return_val_if_fail (store != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);
	g_return_val_if_fail (value != NULL, NULL);

	index = g_hash_table_lookup (store->property_index, key);

	if (index) {
		devices = g_hash_table_lookup (index, value);
		if (devices)
			return (HalDevice*) devices->data;
		else
			return NULL;
	} else {
		for (iter = store->devices; iter != NULL; iter = iter->next) {
			HalDevice *d = HAL_DEVICE (iter->data);
			int type;

			if (!hal_device_has_property (d, key))
				continue;

			type = hal_device_property_get_type (d, key);
			if (type != HAL_PROPERTY_TYPE_STRING)
				continue;

			if (strcmp (hal_device_property_get_string (d, key),
				    value) == 0)
				return d;
		}
	}

	return NULL;
}

HalDevice *
hal_device_store_match_key_value_int (HalDeviceStore *store,
				      const char *key,
				      int value)
{
	GSList *iter;

	g_return_val_if_fail (store != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	for (iter = store->devices; iter != NULL; iter = iter->next) {
		HalDevice *d = HAL_DEVICE (iter->data);
		int type;

		if (!hal_device_has_property (d, key))
			continue;

		type = hal_device_property_get_type (d, key);
		if (type != HAL_PROPERTY_TYPE_INT32)
			continue;

		if (hal_device_property_get_int (d, key) == value)
			return d;
	}

	return NULL;
}

GSList *
hal_device_store_match_multiple_key_value_string (HalDeviceStore *store,
						  const char *key,
						  const char *value)
{
	GSList *iter;
	GSList *matches = NULL;
	GSList *devices;
	GHashTable *index;

	g_return_val_if_fail (store != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);
	g_return_val_if_fail (value != NULL, NULL);

	index = g_hash_table_lookup (store->property_index, key);

	if (index) {
		devices = g_hash_table_lookup (index, value);
		return g_slist_copy(devices);
	} else {
		for (iter = store->devices; iter != NULL; iter = iter->next) {
			HalDevice *d = HAL_DEVICE (iter->data);
			int type;

			if (!hal_device_has_property (d, key))
				continue;

			type = hal_device_property_get_type (d, key);
			if (type != HAL_PROPERTY_TYPE_STRING)
				continue;

			if (strcmp (hal_device_property_get_string (d, key),
				    value) == 0)
				matches = g_slist_prepend (matches, d);
		}
	}

	return matches;
}


void
hal_device_store_index_property (HalDeviceStore *store, const char *key)
{
	GHashTable *index;

	index = g_hash_table_lookup (store->property_index, key);

	if (!index) {
		index = g_hash_table_new (g_str_hash, g_str_equal);
		g_hash_table_insert (store->property_index, g_strdup (key), index);
	}
}

static void
property_index_modify_string (HalDeviceStore *store, HalDevice *device,
			      const char *key, gboolean added)
{
	GHashTable *index;
	const char *value;
	GSList *devices;

	value = hal_device_property_get_string (device, key);
	index = g_hash_table_lookup (store->property_index, key);

	if (!index) return;

	devices = g_hash_table_lookup (index, value);

	if (added) { /*add*/
		HAL_DEBUG (("adding %p to (%s,%s)", device, key, value));
		devices = g_slist_prepend (devices, device);
	} else { /*remove*/
		HAL_DEBUG (("removing %p from (%s,%s)", device, key, value));
		devices = g_slist_remove_all (devices, device);
	}
	g_hash_table_insert (index, (gpointer) value, devices);
}

#if GLIB_CHECK_VERSION (2,14,0)
        /* Nothing */
#else
inline static void
list_keys (gpointer key, gpointer value, GList **keys)
{
        *keys = g_list_append (*keys, key);
}

inline static GList*
g_hash_table_get_keys (GHashTable *hash)
{
        GList *keys = NULL;
        g_hash_table_foreach (hash, (GHFunc)list_keys, &keys);
        return keys;
}
#endif

static void
property_index_check_all (HalDeviceStore *store, HalDevice *device, gboolean added)
{
	GList *indexed_properties, *lp;

	indexed_properties = g_hash_table_get_keys (store->property_index);
	for (lp = indexed_properties; lp; lp = g_list_next (lp)) {
		if (hal_device_property_get_type (device, lp->data) == HAL_PROPERTY_TYPE_STRING) {
			property_index_modify_string (store, device, lp->data, added);
		}
	}
	g_list_free (indexed_properties);
}

