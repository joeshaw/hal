/***************************************************************************
 * CVSID: $Id$
 *
 * device_store.c : device store interface
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


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include "logger.h"
#include "device_store.h"

/**
 * @defgroup DeviceStore HAL device store
 * @ingroup HalDaemon
 * @brief The device store is where the HAL daemon device objects are stored
 * @{
 */


/** property changed callback */
static HalDevicePropertyChangedCallback property_changed_cb = NULL;

/** Number of devices */
static unsigned int device_list_num = 0;

/** Counter used for generating random device names*/
unsigned int temp_device_counter = 0;

/** Head of device list */
static HalDevice* device_list_head = NULL;

/** Memory allocation; aborts if no memory.
 *
 *  @param  how_much            Number of bytes to allocated
 *  @return                     Pointer to allocated storage
 */
static void* xmalloc(unsigned int how_much)
{
    void* p = malloc(how_much);
    if( !p )
        DIE(("Unable to allocate %d bytes of memory", how_much));
    return p;
}

/** Initialize the device store.
 *
 *  @param  _property_changed_cb  Function to invoke whenever a property
 *                                is changed
 */
void ds_init(HalDevicePropertyChangedCallback _property_changed_cb)
{
    device_list_num = 0;
    device_list_head = NULL;
    temp_device_counter = 0;
    property_changed_cb = _property_changed_cb;
}

/** Shut down the device store.
 */
void ds_shutdown()
{
}

/** Dump a textual representation of a device to stdout
 *
 *  @param  device              A pointer to a #HalDevice object
 */
void ds_print(HalDevice* device)
{
    HalPropertyIterator iter;

    printf("device udi = %s    (%s)\n", ds_device_get_udi(device),
           device->in_gdl ? "in GDL" : "NOT in GDL");

    for(ds_property_iter_begin(device, &iter);
        ds_property_iter_has_more(&iter);
        ds_property_iter_next(&iter))
    {
        int type;
        HalProperty* p;
        const char* key;

        p = ds_property_iter_get(&iter);

        key = ds_property_iter_get_key(p);
        type = ds_property_iter_get_type(p);

        switch( type )
        {
        case DBUS_TYPE_STRING:
            printf("  %s = '%s'  (string)\n", key, 
                   ds_property_iter_get_string(p));
            break;

        case DBUS_TYPE_INT32:
            printf("  %s = %d  0x%x  (int)\n", key, 
                   ds_property_iter_get_int(p),
                   ds_property_iter_get_int(p));
            break;

        case DBUS_TYPE_DOUBLE:
            printf("  %s = %g  (double)\n", key, 
                   ds_property_iter_get_double(p));
            break;

        case DBUS_TYPE_BOOLEAN:
            printf("  %s = %s  (bool)\n", key, 
                   (ds_property_iter_get_bool(p) ? "true" : "false"));
            break;

        default:
            LOG_WARNING(("Unknown property type %d", type));
            break;
        }
    }
    printf("\n");
}

/**************************************************************************/

/** Find a device; it doesn't have to be in the global device list.
 *
 *  @param  udi                 Unique device id
 *  @return                     #HalDevice object or #NULL if the device
 *                              doesn't exist
 */
HalDevice* ds_device_find(const char* udi)
{
    HalDevice* it;

    for(it=device_list_head; it!=NULL; it=it->next)
    {
        if( strcmp(it->udi, udi)==0 )
            return it;
    }
    return NULL;
}

/** Create a new device; it will be added to the global device list
 *  and will have a randomly generated unique device id. It will not
 *  be in the global device list.
 *
 *  @return                     A #HalDevice object
 */
HalDevice* ds_device_new()
{
    char buf[128];
    HalDevice* obj;

    snprintf(buf, 128, "/org/freedesktop/Hal/devices/temp/%d", 
             temp_device_counter++);

    obj = (HalDevice*) xmalloc(sizeof(HalDevice));

    obj->udi            = strdup(buf);
    obj->in_gdl         = FALSE;
    obj->num_properties = 0;
    obj->prop_head      = NULL;

    obj->prev = NULL;
    obj->next = device_list_head;
    if( obj->next!=NULL )
        obj->next->prev = obj;

    device_list_head = obj;
    device_list_num++;

    return obj;
}

/** Destroy a device; works both if the device is in the global device list
 *  or not.
 *
 *  @param  device              A pointer to a #HalDevice object
 */
void ds_device_destroy(HalDevice* device)
{
    HalProperty* prop;
    HalProperty* prop_next;

    // remove device from list
    if( device->next!=NULL )
        device->next->prev = device->prev;
    if( device->prev!=NULL )
        device->prev->next = device->next;
    if( device_list_head==device )
        device_list_head = device->next;
    --device_list_num;

    // free device
    for(prop=device->prop_head; prop!=NULL; prop=prop_next)
    {
        prop_next = prop->next;
        free(prop->key);
        if( prop->type==DBUS_TYPE_STRING )
            free(prop->str_value);
        free(prop);
    }
    free(device->udi);
    free(device);
}

/**************************************************************************/

/** Add a device to the global device list
 *
 *  @param  device              A #HalDevice object
 */
void ds_gdl_add(HalDevice* device)
{
    device->in_gdl = TRUE;
}

/** Get number of devices
 *
 *  @return                     Number of devices
 */
unsigned int ds_device_size()
{
    return device_list_num;
}

/** Get an iterator pointing to the beginning of the device list
 *
 *  @param  iterator            Iterator object
 */
void ds_device_iter_begin(HalDeviceIterator* iterator)
{
    iterator->position = 0;
    iterator->cursor = device_list_head;
}

/** Determine if there are more devices to iterate over.
 *
 *  @param  iterator            Iterator object
 *  @return                     #FALSE if there isn't any properties left
 */
dbus_bool_t ds_device_iter_has_more(HalDeviceIterator* iterator)
{
    return iterator->position<device_list_num;
}

/** Advance the iterator to the next position.
 *
 *  @param  iterator            Iterator object
 *  @return                     #FALSE if there isn't any devices left
 *                              left
 */
void ds_device_iter_next(HalDeviceIterator* iterator)
{
    iterator->position++;
    iterator->cursor = iterator->cursor->next;
}

/** Get the device that this iterator represents
 *
 *  @param  iterator            Iterator object
 *  @return                     The #HalDevice object the iterator represents
 */
HalDevice* ds_device_iter_get(HalDeviceIterator* iterator)
{
    return iterator->cursor;
}

/**************************************************************************/

/** Get device unique id.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @retur                      The device unique id
 */
const char* ds_device_get_udi(HalDevice* device)
{
    return device->udi;
}

/** Set unique device id.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  udi                 Unique device id
 *  @return                     #FALSE if the unique device id was in use
 *                              by another application
 */
dbus_bool_t ds_device_set_udi(HalDevice* device, const char* udi)
{
    if( ds_device_find(udi)!=NULL )
        return FALSE;
    free(device->udi);
    device->udi = strdup(udi);
    return TRUE;
}


/**************************************************************************/

/** Get number of properties.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @return                     Number of properties
 */
unsigned int ds_properties_size(HalDevice* device)
{
    return device->num_properties;
}

/** Determine if a property exists.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     #TRUE if the property exists, otherwise #FALSE
 */
dbus_bool_t ds_property_exists(HalDevice* device, const char* key)
{
    return ds_property_find(device, key)!=NULL;
}

/** Find a device property.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     #HalProperty object or #NULL if the property
 *                              doesn't exist
 */
HalProperty* ds_property_find(HalDevice* device, const char* key)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            return prop;
        }
    }

    return NULL;
}

/** Get an iterator pointing to the beginning of the properties of a device.
 *
 *  @param  device              Device to get properties from
 *  @param  iterator            Iterator object
 */
void ds_property_iter_begin(HalDevice* device, HalPropertyIterator* iterator)
{
    iterator->device = device;
    iterator->cursor = device->prop_head;
}

/** Determine if there are more properties to iterate over.
 *
 *  @param  iterator            Iterator object
 *  @return                     #FALSE if there isn't any properties left
 */
dbus_bool_t ds_property_iter_has_more(HalPropertyIterator* iterator)
{
    return iterator->cursor!=NULL;
}

/** Advance the property iterator to the next position.
 *
 *  @param  iterator            Iterator object
 */
void ds_property_iter_next(HalPropertyIterator* iterator)
{
    iterator->cursor = iterator->cursor->next;
}

/** Get the property that this iterator represents
 *
 *  @param  iterator            Iterator object
 *  @return                     The #HalProperty object the iterator represents
 */
HalProperty* ds_property_iter_get(HalPropertyIterator* iterator)
{
    return iterator->cursor;
}

/** Set the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @param  value               A zero-terminated UTF-8 string
 *  @return                     #FALSE if the property already exists and
 *                              isn't of type string
 */
dbus_bool_t ds_property_set_string(HalDevice* device, const char* key, 
                                   const char* value)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_STRING )
                return FALSE;

            // don't bother setting the same value
            if( value!=NULL && strcmp(prop->str_value, value)==0 )
            {
                return TRUE;
            }

            free(prop->str_value);
            prop->str_value = (char*) strdup(value);
            if( prop->str_value == NULL )
                return FALSE;

            // callback that the property changed
            if( property_changed_cb!=NULL )
                property_changed_cb(device,
                                    key,
                                    device->in_gdl, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) strdup(key);
    if( prop->key==NULL )
        return FALSE;
    prop->str_value = (char*) strdup(value);
    if( prop->str_value==NULL )
        return FALSE;

    prop->type = DBUS_TYPE_STRING;

    prop->next        = device->prop_head;
    prop->prev        = NULL;
    device->prop_head = prop;
    device->num_properties++;
    if( prop->next!=NULL )
        prop->next->prev = prop;

    // callback that the property have been added
    if( property_changed_cb!=NULL )
        property_changed_cb(device,
                            prop->key,
                            device->in_gdl, FALSE);

    return TRUE;
}

/** Set the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @param  value               A zero-terminated UTF-8 string
 *  @return                     #FALSE if the property already exists and
 *                              isn't of type int
 */
dbus_bool_t ds_property_set_int(HalDevice* device, const char* key, 
                                dbus_int32_t value)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_INT32 )
                return FALSE;

            // don't bother setting the same value
            if( prop->int_value==value )
                return TRUE;

            prop->int_value = value;

            // callback that the property changed
            if( property_changed_cb!=NULL )
                property_changed_cb(device,
                                    key,
                                    device->in_gdl, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) strdup(key);
    if( prop->key==NULL )
        return FALSE;
    prop->int_value = value;

    prop->type = DBUS_TYPE_INT32;

    prop->next        = device->prop_head;
    prop->prev        = NULL;
    device->prop_head = prop;
    device->num_properties++;
    if( prop->next!=NULL )
        prop->next->prev = prop;

    // callback that the property have been added
    if( property_changed_cb!=NULL )
        property_changed_cb(device,
                            prop->key,
                            device->in_gdl, FALSE);

    return TRUE;
}

/** Set the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @param  value               A zero-terminated UTF-8 string
 *  @return                     #FALSE if the property already exists and
 *                              isn't of type boolean
 */
dbus_bool_t ds_property_set_bool(HalDevice* device, const char* key, 
                                 dbus_bool_t value)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_BOOLEAN )
                return FALSE;

            // don't bother setting the same value
            if( prop->bool_value==value )
                return TRUE;

            prop->bool_value = value;

            // callback that the property changed
            if( property_changed_cb!=NULL )
                property_changed_cb(device,
                                    key,
                                    device->in_gdl, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) strdup(key);
    if( prop->key==NULL )
        return FALSE;
    prop->bool_value = value;

    prop->type = DBUS_TYPE_BOOLEAN;

    prop->next        = device->prop_head;
    prop->prev        = NULL;
    device->prop_head = prop;
    device->num_properties++;
    if( prop->next!=NULL )
        prop->next->prev = prop;

    // callback that the property have been added
    if( property_changed_cb!=NULL )
        property_changed_cb(device,
                            prop->key,
                            device->in_gdl, FALSE);

    return TRUE;
}

/** Set the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @param  value               A zero-terminated UTF-8 string
 *  @return                     #FALSE if the property already exists and
 *                              isn't of type double
 */
dbus_bool_t ds_property_set_double(HalDevice* device, const char* key, 
                                   double value)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_DOUBLE )
                return FALSE;

            // don't bother setting the same value
            if( prop->double_value==value )
                return TRUE;

            prop->double_value = value;

            // callback that the property changed
            if( property_changed_cb!=NULL )
                property_changed_cb(device,
                                    key,
                                    device->in_gdl, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) strdup(key);
    if( prop->key==NULL )
        return FALSE;
    prop->double_value = value;

    prop->type = DBUS_TYPE_DOUBLE;

    prop->next        = device->prop_head;
    prop->prev        = NULL;
    device->prop_head = prop;
    device->num_properties++;
    if( prop->next!=NULL )
        prop->next->prev = prop;

    // callback that the property have been added
    if( property_changed_cb!=NULL )
        property_changed_cb(device,
                            prop->key,
                            device->in_gdl, FALSE);

    return TRUE;
}

/** Remove a property.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     #FALSE if property didn't exist
 */
dbus_bool_t ds_property_remove(HalDevice* device, const char* key)
{
    int i;
    HalProperty* prop;

    i=0;
    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next, i++)
    {
        if( strcmp(prop->key, key)==0 )
        {
            switch( prop->type )
            {
            case DBUS_TYPE_STRING:
                free( prop->str_value );
                break;
            case DBUS_TYPE_INT32:
                break;
            case DBUS_TYPE_DOUBLE:
                break;
            case DBUS_TYPE_BOOLEAN:
                break;
            }

            free(prop->key);
                
            if( prop->prev==NULL )
            {
                device->prop_head = prop->next;
                if( prop->next!=NULL )
                    prop->next->prev = NULL;
            }
            else
            {
                prop->prev->next = prop->next;
                if( prop->next!=NULL )
                    prop->next->prev = prop->prev;
            }
            free(prop);
            device->num_properties--;

            // callback that the property have been removed
            if( property_changed_cb!=NULL )
                property_changed_cb(device,
                                    key,
                                    device->in_gdl, TRUE);
            
            return TRUE;
        }
    }

    return FALSE;
}

/**************************************************************************/

/** Get the key of a property.
 *
 *  @param  property            A pointer to a #HalProperty object
 *  @return                     Key of the property
 */
const char* ds_property_iter_get_key(HalProperty* property)
{
    return property->key;
}

/** Get the type of the value of a property.
 *
 *  @param  property            A pointer to a #HalProperty object
 *  @return                     One of #DBUS_TYPE_STRING, #DBUS_TYPE_INT32,
 *                              #DBUS_TYPE_BOOL, #DBUS_TYPE_DOUBLE
 */
int ds_property_iter_get_type(HalProperty* property)
{
    return property->type;
}


/** Get the value of a property
 *
 *  @param  property            A pointer to a #HalProperty object
 *  @return                     A zero-terminated UTF-8 string or #NULL
 *                              if property didn't exist or wasn't a string
 */
const char* ds_property_iter_get_string(HalProperty* property)
{
    if( property->type!=DBUS_TYPE_STRING )
        return NULL;
    return property->str_value;
}

/** Get the value of a property
 *
 *  @param  property            A pointer to a #HalProperty object
 *  @return                     A 32-bit signed integer
 */
dbus_int32_t ds_property_iter_get_int(HalProperty* property)
{
    return property->int_value;
}

/** Get the value of a property
 *
 *  @param  property            A pointer to a #HalProperty object
 *  @return                     #TRUE or #FALSE
 */
dbus_bool_t ds_property_iter_get_bool(HalProperty* property)
{
    return property->bool_value;
}

/** Get the value of a property
 *
 *  @param  property            A pointer to a #HalProperty object
 *  @return                     IEEE754 double precision floating point number
 */
double ds_property_iter_get_double(HalProperty* property)
{
    return property->double_value;
}

/** Merge properties from one device to another. <p>
 *
 *  @param  target              Target device receiving properties
 *  @param  source              Source device contributing properties
 */
void ds_device_merge(HalDevice* target, HalDevice* source)
{
    HalPropertyIterator iter;

    for(ds_property_iter_begin(source, &iter);
        ds_property_iter_has_more(&iter);
        ds_property_iter_next(&iter))
    {
        int type;
        const char* key;
        HalProperty* p;

        p = ds_property_iter_get(&iter);
        key = ds_property_iter_get_key(p);
        type = ds_property_iter_get_type(p);

        ds_property_remove(target, key);

        switch( type )
        {
        case DBUS_TYPE_STRING:
            ds_property_set_string(target, key,ds_property_iter_get_string(p));
            break;
        case DBUS_TYPE_INT32:
            ds_property_set_int(target, key, ds_property_iter_get_int(p));
            break;
        case DBUS_TYPE_DOUBLE:
            ds_property_set_double(target, key,ds_property_iter_get_double(p));
            break;
        case DBUS_TYPE_BOOLEAN:
            ds_property_set_bool(target, key, ds_property_iter_get_bool(p));
            break;
        default:
            LOG_WARNING(("Unknown property type %d", type));
            break;
        }
    }    
}

/** Get the type of the value of a property.
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     One of #DBUS_TYPE_STRING, #DBUS_TYPE_INT32,
 *                              #DBUS_TYPE_BOOL, #DBUS_TYPE_DOUBLE or 
 *                              #DBUS_TYPE_NIL if the property didn't exist
 */
int ds_property_get_type(HalDevice* device, const char* key)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            return prop->type;
        }
    }
    return DBUS_TYPE_NIL;
}

/** Get the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     A zero-terminated UTF-8 string or #NULL
 *                              if property didn't exist or wasn't a string
 */
const char* ds_property_get_string(HalDevice* device, const char* key)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_STRING )
                return NULL;
            return prop->str_value;
        }
    }
    return NULL;
}

/** Get the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     A 32-bit signed integer
 */
dbus_int32_t ds_property_get_int(HalDevice* device, const char* key)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_INT32 )
                return -1;
            return prop->int_value;
        }
    }
    return -1;
}

/** Get the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     #TRUE or #FALSE
 */
dbus_bool_t ds_property_get_bool(HalDevice* device, const char* key)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_BOOLEAN )
                return -1;
            return prop->bool_value;
        }
    }
    return -1;
}

/** Get the value of a property
 *
 *  @param  device              A pointer to a #HalDevice object
 *  @param  key                 The key of the property
 *  @return                     IEEE754 double precision floating point number
 */
double ds_property_get_double(HalDevice* device, const char* key)
{
    HalProperty* prop;

    // check if property already exist
    for(prop=device->prop_head; prop!=NULL; prop=prop->next)
    {
        if( strcmp(prop->key, key)==0 )
        {
            if( prop->type!=DBUS_TYPE_DOUBLE )
                return -1.0;
            return prop->double_value;
        }
    }
    return -1.0;
}


/** Check a set of properties for two devices matches. 
 *
 *  Checks that all properties where keys, starting with a given value
 *  (namespace), of the first device is in the second device and that
 *  they got the same value and type. 
 *
 *  Note that the other inclusion isn't tested, so there could be properties
 *  (from the given namespace) in the second device not present in the 
 *  first device.
 *
 *  @param  device1             Unique Device Id for device 1
 *  @param  device2             Unique Device Id for device 2
 *  @param  namespace           Namespace for set of devices, e.g. "usb"
 *  @return                     #TRUE if all properties starting
 *                              with the given namespace parameter
 *                              from one device is in the other and 
 *                              have the same value.
 */
dbus_bool_t ds_device_matches(HalDevice* device1, HalDevice* device2, 
                              const char* namespace)
{
    int len;
    HalPropertyIterator iter;

    len = strlen(namespace);

    for(ds_property_iter_begin(device1, &iter);
        ds_property_iter_has_more(&iter);
        ds_property_iter_next(&iter))
    {
        int type;
        const char* key;
        HalProperty* p;

        p = ds_property_iter_get(&iter);
        key = ds_property_iter_get_key(p);
        type = ds_property_iter_get_type(p);

        if( strncmp(key, namespace, len)!=0 )
            continue;

        if( !ds_property_exists(device2, key) )
            return FALSE;

        switch( type )
        {
        case DBUS_TYPE_STRING:
            if( strcmp(ds_property_iter_get_string(p),
                       ds_property_get_string(device2, key))!=0 )
                return FALSE;
            break;
        case DBUS_TYPE_INT32:
            if( ds_property_iter_get_int(p) !=
                ds_property_get_int(device2, key) )
                return FALSE;
            break;
        case DBUS_TYPE_DOUBLE:
            if( ds_property_iter_get_double(p) !=
                ds_property_get_double(device2, key) )
                return FALSE;
            break;
        case DBUS_TYPE_BOOLEAN:
            if( ds_property_iter_get_bool(p) !=
                ds_property_get_bool(device2, key) )
                return FALSE;
            break;
        default:
            LOG_WARNING(("Unknown property type %d", type));
            break;
        }
    }

    return TRUE;
}

/** @} */
