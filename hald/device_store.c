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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

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

/** new capability callback */
static HalDeviceGDLChangedCallback gdl_changed_cb = NULL;

/** new capability callback */
static HalDeviceNewCapabilityCallback new_capability_cb = NULL;

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

/** String duplication; aborts if no memory.
 *
 *  @param  how_much            Number of bytes to allocated
 *  @return                     Pointer to allocated storage
 */
static char* xstrdup(const char* str)
{
    char* p = strdup(str);
    if( !p )
        DIE(("Unable to duplicate string '%s'", str));
    return p;
}

/** Initialize the device store.
 *
 *  @param  _property_changed_cb  Function to invoke whenever a property
 *                                is changed
 */
void ds_init(HalDevicePropertyChangedCallback _property_changed_cb,
             HalDeviceGDLChangedCallback _gdl_changed_cb,
             HalDeviceNewCapabilityCallback _new_capability_cb)
{
    device_list_num = 0;
    device_list_head = NULL;
    temp_device_counter = 0;
    property_changed_cb = _property_changed_cb;
    gdl_changed_cb = _gdl_changed_cb;
    new_capability_cb = _new_capability_cb;
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

/** search structure for async find operations */
typedef struct DSDeviceAsyncFindStruct_s
{
    char* key;                    /**< key to search for, allocated by us */
    char* value;                  /**< value that key must assume, allocated
                                   *   by us */
    DSAsyncFindDeviceCB callback; /**< callback to caller */
    void* data1;                  /**< caller data, opaque pointer */
    void* data2;                  /**< caller data, opaque pointer */
    guint timeout_id;             /**< Timeout ID */

    struct DSDeviceAsyncFindStruct_s* next; /**< next element in list*/
} DSDeviceAsyncFindStruct;

/** head of list of outstanding async find requests */
static DSDeviceAsyncFindStruct* async_find_outstanding_head = NULL;

/** Callback for when an async timeout happens 
 *
 *  @param  data                User provided data
 *  @return                     #TRUE to preserve the timer, #FALSE otherwise
 */
static gboolean async_find_timeout_fn(gpointer data)
{
    void* data1;
    void* data2;
    DSAsyncFindDeviceCB callback;
    DSDeviceAsyncFindStruct* i;
    DSDeviceAsyncFindStruct* prev;
    DSDeviceAsyncFindStruct* afs = (DSDeviceAsyncFindStruct*) data;

    /* Check if this is still in list */
    for(i=async_find_outstanding_head, prev=NULL; i!=NULL; i=i->next)
    {
        if( i==afs )
        {
            data1 = i->data1;
            data2 = i->data2;
            callback = i->callback;
            
            /* Remove element in list */
            if( prev!=NULL )
            {
                prev->next = i->next;
            }
            else
            {
                async_find_outstanding_head = i->next;
            }

            /* Clean up element */
            free(i->key);
            free(i->value);
            free(i);

            /* We need to be careful to remove the element prior to this
             * invocation as our use of it triggers a device addition and
             * thus an indirect invocation of code looking at this list
             */
            (*callback)(NULL, data1, data2);
            return FALSE; /* cancel timeout */
        }

        prev = i;
    }
    return FALSE; /* cancel timeout */
}

/** This function is called by #ds_gdl_add() when a new device is added
 *  to the global device list.
 *
 *  We use it to check if there is someone having an asynchronous wait out
 *  for the this device.
 *
 *  @param  device              Device just added
 */
static void async_find_check_new_addition(HalDevice* device)
{
    int type;
    void* data1;
    void* data2;
    DSAsyncFindDeviceCB callback;
    DSDeviceAsyncFindStruct* i;
    DSDeviceAsyncFindStruct* prev;

    /* Check if this is still in list */
    for(i=async_find_outstanding_head, prev=NULL; i!=NULL; i=i->next)
    {
        type = ds_property_get_type(device, i->key);
        if( type==DBUS_TYPE_STRING )
        {
            if( strcmp(ds_property_get_string(device, i->key), i->value)==0 )
            {
                /* Yay, a match! */
                /*printf("new_add_match 0x%08x\n", i);*/

                data1 = i->data1;
                data2 = i->data2;
                callback = i->callback;
            
                /* Remove element in list */
                if( prev!=NULL )
                {
                    prev->next = i->next;
                }
                else
                {
                    async_find_outstanding_head = i->next;
                }

                /* Clean up element */
                free(i->key);
                free(i->value);
                free(i);

                /* We need to be careful to remove the element prior to this
                 * invocation as our use of it triggers a device addition and
                 * thus an indirect invocation of code looking at this list
                 */
                (*callback)(device, data1, data2);
                return;
            }
        }
        prev = i;
    }
}

/** Find a device by requiring a specific key to assume string value. If 
 *  multiple devices meet this criteria then the result is undefined. Only
 *  devices in the GDL is searched.
 *
 *  This is an asynchronous version of #ds_device_find_by_key_value_string.
 *  The result is delivered to the specified callback function. If the
 *  device is there the callback is invoked immedieately and thus
 *  before this functions returns.
 *
 *  The caller can specify a timeout in milliseconds on how long he is
 *  willing to wait. A value of zero means don't wait at all.
 *
 *  @param  key                 key of the property
 *  @param  value               value of the property
 *  @param  data1               The 1st parameter passed to the callback
 *                              function for this callback. This is used to
 *                              uniqely identify the origin/context for the
 *                              caller
 *  @param  data2               The 2nd parameter passed to the callback
 *                              function
 *  @param  timeout             Timeout in milliseconds
 *  @return                     #HalDevice object or #NULL if no such device
 *                              exist
 */
void ds_device_async_find_by_key_value_string(const char* key, 
                                              const char* value,
                                              DSAsyncFindDeviceCB callback, 
                                              void* data1, 
                                              void* data2, 
                                              int timeout)
{
    HalDevice* device;
    DSDeviceAsyncFindStruct* afs;

    assert( callback!=NULL );

    /* first, try to see if we can find it right away */
    device = ds_device_find_by_key_value_string(key, value);
    if( device!=NULL )
    {
        /* yay! */
        (*callback)(device, data1, data2 );
        return;
    }

    if( timeout==0 )
    {
        (*callback)(NULL, data1, data2 );
        return;
    }

    /* Nope; First, create a request with the neccessary data */
    afs = xmalloc(sizeof(DSDeviceAsyncFindStruct));
    afs->key   = xstrdup(key);
    afs->value = xstrdup(value);
    afs->callback = callback;
    afs->data1 = data1;
    afs->data2 = data2;

    /* Second, setup a function to fire at timeout */
    afs->timeout_id = g_timeout_add(timeout, 
                                    async_find_timeout_fn, (gpointer)afs);

    afs->next = async_find_outstanding_head;
    async_find_outstanding_head = afs;

}

/** Find a device by requiring a specific key to assume string value. If 
 *  multiple devices meet this criteria then the result is undefined. Only
 *  devices in the GDL is searched.
 *
 *  @param  key                 key of the property
 *  @param  value               value of the property
 *  @return                     #HalDevice object or #NULL if no such device
 *                              exist
 */
HalDevice* ds_device_find_by_key_value_string(const char* key, 
                                              const char* value)
{
    int type;
    HalDevice* device;
    HalDeviceIterator iter_device;

    for(ds_device_iter_begin(&iter_device);
        ds_device_iter_has_more(&iter_device);
        ds_device_iter_next(&iter_device))
    {
        device = ds_device_iter_get(&iter_device);

        if( !device->in_gdl )
            continue;

        type = ds_property_get_type(device, key);
        if( type==DBUS_TYPE_STRING )
        {
            if( strcmp(ds_property_get_string(device, key),
                       value)==0 )
                return device;
        }
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

    obj->udi            = xstrdup(buf);
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

    if( gdl_changed_cb!=NULL )
        (*gdl_changed_cb)(device, FALSE);

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

    if( gdl_changed_cb!=NULL )
        (*gdl_changed_cb)(device, TRUE);

    /* Ah, now check for the asynchronous outstanding finds whether this
     * device was the one someone is searching for
     */
    async_find_check_new_addition(device);
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
    device->udi = xstrdup(udi);
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
            prop->str_value = (char*) xstrdup(value);

            // callback that the property changed
            if( property_changed_cb!=NULL )
                property_changed_cb(device,
                                    key,
                                    device->in_gdl, FALSE, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) xstrdup(key);
    prop->str_value = (char*) xstrdup(value);

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
                            device->in_gdl, FALSE, TRUE);

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
                                    device->in_gdl, FALSE, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) xstrdup(key);
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
                            device->in_gdl, FALSE, TRUE);

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
                                    device->in_gdl, FALSE, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) xstrdup(key);
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
                            device->in_gdl, FALSE, TRUE);

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
                                    device->in_gdl, FALSE, FALSE);
            return TRUE;
        }
    }

    // nope, have to create it
    //
    prop = (HalProperty*) malloc(sizeof(HalProperty));
    if( prop==NULL )
        return FALSE;
    prop->key = (char*) xstrdup(key);
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
                            device->in_gdl, FALSE, TRUE);

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
                                    device->in_gdl, TRUE, FALSE);
            
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


/** Maximum string length for capabilities; quite a hack :-/ */
#define MAX_CAP_SIZE 2048

/** Add a capability to a device
 *
 *  @param  device              HalDevice to add capability to
 *  @param  capability          Capability to add
 */
void ds_add_capability(HalDevice* device, const char* capability)
{
    const char* caps;
    char buf[MAX_CAP_SIZE];

    caps = ds_property_get_string(device, "Capabilities");
    if( caps==NULL )
    {
        ds_property_set_string(device, "Capabilities", capability);
    }
    else
    {
        if( strstr(caps, capability)==NULL )
        {
            snprintf(buf, MAX_CAP_SIZE, "%s %s", caps, capability);
            ds_property_set_string(device, "Capabilities", buf);
        }
    }


    if( new_capability_cb!=NULL )
        (*new_capability_cb)(device, capability, device->in_gdl);
}

/** @} */
