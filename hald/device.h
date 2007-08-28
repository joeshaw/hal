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

#ifndef DEVICE_H
#define DEVICE_H

#ifndef DBUS_API_SUBJECT_TO_CHANGE
#define DBUS_API_SUBJECT_TO_CHANGE
#endif 

#include <glib-object.h>
#include <dbus/dbus.h>

struct _HalDevicePrivate;
typedef struct _HalDevicePrivate HalDevicePrivate;


typedef struct _HalDevice      HalDevice;
typedef struct _HalDeviceClass HalDeviceClass;

struct _HalDevice {
	GObject parent;
	HalDevicePrivate *private;
};

struct _HalDeviceClass {
	GObjectClass parent_class;

	/* signals */
	void (*property_changed) (HalDevice *device,
				  const char *key,
				  gboolean removed,
				  gboolean added);

	void (*capability_added) (HalDevice *device,
				  const char *capability);

	void (*lock_acquired) (HalDevice *device,
                               const char *lock_name,
                               const char *lock_owner);

	void (*lock_released) (HalDevice *device,
                               const char *lock_name,
                               const char *lock_owner);
};

#define HAL_TYPE_DEVICE             (hal_device_get_type ())
#define HAL_DEVICE(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                     HAL_TYPE_DEVICE, HalDevice))
#define HAL_DEVICE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), \
                                     HAL_TYPE_DEVICE, HalDeviceClass))
#define HAL_IS_DEVICE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
                                     HAL_TYPE_DEVICE))
#define HAL_IS_DEVICE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
                                     HAL_TYPE_DEVICE))

#define HAL_PROPERTY_TYPE_INVALID     DBUS_TYPE_INVALID
#define HAL_PROPERTY_TYPE_INT32       DBUS_TYPE_INT32
#define HAL_PROPERTY_TYPE_UINT64      DBUS_TYPE_UINT64
#define HAL_PROPERTY_TYPE_DOUBLE      DBUS_TYPE_DOUBLE
#define HAL_PROPERTY_TYPE_BOOLEAN     DBUS_TYPE_BOOLEAN
#define HAL_PROPERTY_TYPE_STRING      DBUS_TYPE_STRING
#define HAL_PROPERTY_TYPE_STRLIST     ((int) (DBUS_TYPE_STRING<<8)+('l'))


/* private; do not access; might change in the future */
typedef struct _HalDeviceStrListIter      HalDeviceStrListIter;
struct _HalDeviceStrListIter {
	GSList *i;
};


typedef void (*HalDevicePropertyForeachFn) (HalDevice *device,
					    const char *key,
					    gpointer user_data);

GType         hal_device_get_type            (void);

HalDevice    *hal_device_new                 (void);

void          hal_device_merge_with_rewrite  (HalDevice    *target,
					      HalDevice    *source,
					      const char   *target_namespace,
					      const char   *source_namespace);

const char   *hal_device_get_udi             (HalDevice    *device);
void          hal_device_set_udi             (HalDevice    *device,
					      const char   *udi);

void          hal_device_add_capability      (HalDevice    *device,
					      const char   *capability);
gboolean      hal_device_has_capability      (HalDevice    *device,
					      const char   *capability);

gboolean      hal_device_has_property        (HalDevice    *device,
					      const char   *key);

int           hal_device_num_properties      (HalDevice    *device);
char *        hal_device_property_to_string  (HalDevice    *device,
					      const char   *key);
void          hal_device_property_foreach    (HalDevice    *device,
					      HalDevicePropertyForeachFn callback,
					      gpointer      user_data);

int           hal_device_property_get_type   (HalDevice    *device,
					      const char   *key);
const char   *hal_device_property_get_as_string (HalDevice    *device,
						 const char   *key,
						 char *buf,
						 size_t bufsize);


const char   *hal_device_property_get_string (HalDevice    *device,
					      const char   *key);
dbus_int32_t  hal_device_property_get_int    (HalDevice    *device,
					      const char   *key);
dbus_uint64_t hal_device_property_get_uint64 (HalDevice    *device,
						  const char   *key);
dbus_bool_t   hal_device_property_get_bool   (HalDevice    *device,
					      const char   *key);
double        hal_device_property_get_double (HalDevice    *device,
					      const char   *key);
const char   *hal_device_property_get_strlist_elem (HalDevice    *device,
						    const char   *key,
						    guint index);
guint         hal_device_property_get_strlist_length (HalDevice    *device,
						      const char   *key);
gboolean      hal_device_property_strlist_contains (HalDevice    *device,
						    const char   *key,
						    const char   *value);
char        **hal_device_property_dup_strlist_as_strv (HalDevice    *device,
						       const char   *key);

void          hal_device_property_strlist_iter_init (HalDevice    *device,
						     const char   *key,
						     HalDeviceStrListIter *iter);
const char   *hal_device_property_strlist_iter_get_value (HalDeviceStrListIter *iter);
void          hal_device_property_strlist_iter_next (HalDeviceStrListIter *iter);
gboolean      hal_device_property_strlist_iter_is_valid (HalDeviceStrListIter *iter);



gboolean      hal_device_property_set_string (HalDevice    *device,
					      const char   *key,
					      const char   *value);
gboolean      hal_device_property_set_int    (HalDevice    *device,
					      const char   *key,
					      dbus_int32_t  value);
gboolean      hal_device_property_set_uint64 (HalDevice    *device,
					      const char   *key,
					      dbus_uint64_t value);
gboolean      hal_device_property_set_bool   (HalDevice    *device,
					      const char   *key,
					      dbus_bool_t   value);
gboolean      hal_device_property_set_double (HalDevice    *device,
					      const char   *key,
					      double        value);
gboolean      hal_device_property_set_strlist (HalDevice *device, 
					       const char *key,
                                 	       GSList *value);
gboolean      hal_device_property_strlist_append (HalDevice    *device,
						  const char   *key,
						  const char   *value,
						  gboolean     changeset);
gboolean      hal_device_property_strlist_append_finish_changeset (HalDevice    *device,
						  		   const char   *key,
						  		   gboolean     is_added);
gboolean      hal_device_property_strlist_prepend (HalDevice    *device,
						  const char   *key,
						  const char *value);
gboolean      hal_device_property_strlist_remove_elem (HalDevice    *device,
						       const char   *key,
						       guint index);
gboolean      hal_device_property_strlist_clear (HalDevice    *device,
						 const char   *key,
						 gboolean     changeset);
gboolean      hal_device_property_strlist_add (HalDevice    *device,
					       const char   *key,
					       const char *value);
gboolean      hal_device_property_strlist_remove (HalDevice    *device,
						  const char   *key,
						  const char *value);
gboolean      hal_device_property_strlist_is_empty (HalDevice    *device,
                                                    const char   *key);

gboolean      hal_device_property_remove     (HalDevice    *device,
					      const char   *key);


void          hal_device_print               (HalDevice    *device);

gboolean      hal_device_copy_property       (HalDevice *from_device, 
					      const char *from,
					      HalDevice *to_device,
					      const char *to);

void          hal_device_inc_num_addons (HalDevice *device);

gboolean      hal_device_inc_num_ready_addons (HalDevice *device);

gboolean      hal_device_are_all_addons_ready (HalDevice *device);

gboolean      hal_device_acquire_lock (HalDevice *device, const char *lock_name, gboolean exclusive, const char *sender);

gboolean      hal_device_release_lock (HalDevice *device, const char *lock_name, const char *sender);

gboolean      hal_device_is_lock_exclusive (HalDevice *device, const char *lock_name);

char        **hal_device_get_lock_holders (HalDevice *device, const char *lock_name);

int           hal_device_get_num_lock_holders (HalDevice *device, const char *lock_name);

/* static method */
void          hal_device_client_disconnected (const char *sender);

#endif /* DEVICE_H */
