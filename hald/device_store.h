/***************************************************************************
 * CVSID: $Id$
 *
 * device_store.h : device store interface
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#ifndef DEVICE_STORE_H
#define DEVICE_STORE_H

#include <stdarg.h>
#include <stdint.h>
#include <dbus/dbus.h>



/**************************************************************************/

/* NOTE: Right now this implementation is quite naive - should be replaced
   with something like hashtables etc. */

/** HalProperty internals; private
 */
typedef struct HalProperty_s
{
    int type;                     /**< One of #DBUS_TYPE_STRING, 
                                   * #DBUS_TYPE_INT32, #DBUS_TYPE_BOOL, 
                                   * #DBUS_TYPE_DOUBLE */
    char* key;                    /**< ASCII string */

    /** Union of possible values */
    union
    {
        char* str_value;          /**< UTF-8 string */
        dbus_int32_t int_value;   /**< Signed 32-bit integer */
        dbus_bool_t bool_value;   /**< Boolean value */
        double double_value;      /**< IEEE754 double precision floating 
                                   *   point number */
    };
    struct HalProperty_s* prev;   /**< Linked list; prev element or #NULL */
    struct HalProperty_s* next;   /**< Linked list; next element or #NULL */
} HalProperty;

/** HalDevice internals; private
 */
typedef struct HalDevice_s
{
    char* udi;                       /**< Unique device id */
    dbus_bool_t in_gdl;              /**< True iff device is in the global
                                      *   device list */
    int num_properties;              /**< Number of properties */
    HalProperty* prop_head;          /**< Properties head */
    struct HalDevice_s* prev;        /**< Linked list; prev element or #NULL */
    struct HalDevice_s* next;        /**< Linked list; next element or #NULL */
} HalDevice;


/** Iterator for properties; private
 */
typedef struct HalPropertyIterator_s
{
    HalDevice* device;            /**< The device we are iterating over */
    HalProperty* cursor;          /**< Cursor position */
} HalPropertyIterator;

/** Iterator for global device list; private
 */
typedef struct HalDeviceIterator_s
{
    HalDevice* cursor;            /**< Cursor position */
    unsigned int position;        /**< Which number are we iterating over */
} HalDeviceIterator;

/** Signature for callback function when a property is changed, added
 *  or removed.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @param  in_gdl              Device is in global device list
 *  @param  removed             True iff property was removed
 *  @param  added               True iff property was added
 */
typedef void (*HalDevicePropertyChangedCallback)(HalDevice* device,
                                                 const char* key, 
                                                 dbus_bool_t in_gdl, 
                                                 dbus_bool_t removed,
                                                 dbus_bool_t added);

void ds_init(HalDevicePropertyChangedCallback property_changed_cb);
void ds_shutdown();
void ds_print(HalDevice* device);

/**************************************************************************/

HalDevice* ds_device_find(const char* udi);
HalDevice* ds_device_new();
void ds_device_destroy(HalDevice* device);

void ds_device_merge(HalDevice* target, HalDevice* source);

dbus_bool_t ds_device_matches(HalDevice* device1, HalDevice* device, 
                              const char* namespace);

/**************************************************************************/

void ds_gdl_add(HalDevice* device);

unsigned int ds_device_size();
void ds_device_iter_begin(HalDeviceIterator* iterator);
dbus_bool_t ds_device_iter_has_more(HalDeviceIterator* iterator);
void ds_device_iter_next(HalDeviceIterator* iterator);
HalDevice* ds_device_iter_get(HalDeviceIterator* iterator);

const char* ds_device_get_udi(HalDevice* device);
dbus_bool_t ds_device_set_udi(HalDevice* device, const char* udi);


/**************************************************************************/

unsigned int ds_properties_size(HalDevice* device);
dbus_bool_t ds_property_exists(HalDevice* device, const char* key);
HalProperty* ds_property_find(HalDevice* device, const char* key);
void ds_property_iter_begin(HalDevice* device, HalPropertyIterator* iterator);
dbus_bool_t ds_property_iter_has_more(HalPropertyIterator* iterator);
void ds_property_iter_next(HalPropertyIterator* iterator);
HalProperty* ds_property_iter_get(HalPropertyIterator* iterator);

dbus_bool_t ds_property_set_string(HalDevice* device, const char* key, 
                                   const char* value);
dbus_bool_t ds_property_set_int(HalDevice* device, const char* key, 
                                dbus_int32_t value);
dbus_bool_t ds_property_set_bool(HalDevice* device, const char* key, 
                                 dbus_bool_t value);
dbus_bool_t ds_property_set_double(HalDevice* device, const char* key, 
                                   double value);
dbus_bool_t ds_property_remove(HalDevice* device, const char* key);

/**************************************************************************/

const char* ds_property_iter_get_key(HalProperty* property);
int ds_property_iter_get_type(HalProperty* property);
const char* ds_property_iter_get_string(HalProperty* property);
dbus_int32_t ds_property_iter_get_int(HalProperty* property);
dbus_bool_t ds_property_iter_get_bool(HalProperty* property);
double ds_property_iter_get_double(HalProperty* property);


int ds_property_get_type(HalDevice* device, const char* key);
const char* ds_property_get_string(HalDevice* device, const char* key);
dbus_int32_t ds_property_get_int(HalDevice* device, const char* key);
dbus_bool_t ds_property_get_bool(HalDevice* device, const char* key);
double ds_property_get_double(HalDevice* device, const char* key);





#endif /* DEVICE_STORE_H */
