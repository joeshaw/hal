/***************************************************************************
 * CVSID: $Id$
 *
 * main.c : main() for HAL daemon
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
#include <dbus/dbus-glib.h>

#include <libhal/libhal.h>   /* for common defines etc. */

#include "logger.h"
#include "device_store.h"
#include "device_info.h"

/* We need somewhere to define these groups somewhere */

/**
 * @defgroup HalAgents HAL agents
 * @brief HAL agents are responsible for interfacing with the operating
 *        system. An agent can be invoked to probe a bus and it can be
 *        invoked when a device is hotplugged. 
 *
 *        When the HAL daemon is started it invokes the HAL agents listed
 *        in the hald.conf configuration with the single option
 *        <tt>--probe</tt>. On GNU/Linux, when a device is hotplugged the
 *        appropriate HAL agent is invoked by the linux-hotplug subsystem.
 */

/**
 * @defgroup HalAgentsLinux Linux
 * @ingroup HalAgents
 * @brief HAL agents for GNU/Linux
 */




/**
 * @defgroup HalDaemon HAL daemon
 * @brief The HAL daemon manages persistent device objects available through
 *        a D-BUS network API
 */

/** D-Bus connection object for the HAL service */
static DBusConnection* dbus_connection;

/**
 * @defgroup DaemonErrors Error conditions
 * @ingroup HalDaemon
 * @brief Various error messages the HAL daemon can raise
 * @{
 */

/** Raise the org.freedesktop.Hal.NoSuchDevice error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  udi                 Unique device id given
 */
static void raise_no_such_device(DBusConnection* connection,
                                 DBusMessage* in_reply_to,
                                 const char* udi)
{
    char buf[512];
    DBusMessage *reply;

    snprintf(buf, 511, "No device with id %s", udi);
    LOG_WARNING((buf));
    reply = dbus_message_new_error(in_reply_to, 
                                   "org.freedesktop.Hal.NoSuchDevice", 
                                   buf);
    if( reply==NULL )
        DIE(("No memory"));
    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
    dbus_message_unref(reply);
}

/** Raise the org.freedesktop.Hal.WrongState error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  udi                 Unique device id given
 */
static void raise_wrong_state(DBusConnection* connection,
                              DBusMessage* in_reply_to,
                              const char* udi)
{
    char buf[512];
    DBusMessage *reply;

    snprintf(buf, 511, "No device with id %s", udi);
    LOG_WARNING((buf));
    reply = dbus_message_new_error(in_reply_to, 
                                   "org.freedesktop.Hal.WrongState", 
                                   buf);
    if( reply==NULL )
        DIE(("No memory"));
    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
    dbus_message_unref(reply);
}

/** Raise the org.freedesktop.Hal.UdiInUse error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  udi                 Unique device id that is already in use
 */
static void raise_udi_in_use(DBusConnection* connection,
                             DBusMessage* in_reply_to,
                             const char* udi)
{
    char buf[512];
    DBusMessage *reply;

    snprintf(buf, 511, "Unique device id '%s' is already in use", udi);
    LOG_WARNING((buf));
    reply = dbus_message_new_error(in_reply_to, 
                                   "org.freedesktop.Hal.UdiInUse",
                                   buf);
    if( reply==NULL )
        DIE(("No memory"));
    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
    dbus_message_unref(reply);
}

/** Raise the org.freedesktop.Hal.NoSuchProperty error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property that didn't exist
 */
static void raise_no_such_property(DBusConnection* connection,
                                 DBusMessage* in_reply_to,
                                 const char* device_id, 
                                 const char* key)
{
    char buf[512];
    DBusMessage *reply;

    snprintf(buf, 511, "No property %s on device with id %s", key, device_id);
    LOG_WARNING((buf));
    reply = dbus_message_new_error(in_reply_to, 
                                   "org.freedesktop.Hal.NoSuchProperty", 
                                   buf);
    if( reply==NULL )
        DIE(("No memory"));
    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
    dbus_message_unref(reply);
}

/** Raise the org.freedesktop.Hal.TypeMismatch error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property
 */
static void raise_property_type_error(DBusConnection* connection,
                                    DBusMessage* in_reply_to,
                                    const char* device_id, 
                                    const char* key)
{
    char buf[512];
    DBusMessage *reply;

    snprintf(buf, 511, 
             "Type mismatch setting property %s on device with id %s", 
             key, device_id);
    LOG_WARNING((buf));
    reply = dbus_message_new_error(in_reply_to, 
                                   "org.freedesktop.Hal.TypeMismatch", 
                                   buf);
    if( reply==NULL )
        DIE(("No memory"));
    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
    dbus_message_unref(reply);
}

/** Raise the org.freedesktop.Hal.SyntaxError error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  method_name         Name of the method that was invoked with
 *                              the wrong signature
 */
static void raise_syntax(DBusConnection* connection,
                       DBusMessage* in_reply_to,
                       const char* method_name)
{
    char buf[512];
    DBusMessage *reply;

    snprintf(buf, 511, 
             "There is a syntax error in the invocation of "
             "the method %s", method_name);
    LOG_WARNING((buf));
    reply = dbus_message_new_error(in_reply_to, 
                                   "org.freedesktop.Hal.SyntaxError", 
                                   buf);
    if( reply==NULL )
        DIE(("No memory"));
    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
    dbus_message_unref(reply);
}

/** @} */

/**
 * @defgroup ManagerInterface D-BUS interface org.freedesktop.Hal.Manager
 * @ingroup HalDaemon
 * @brief D-BUS interface for querying device objects
 *
 * @{
 */

/** Get all devices.
 *
 *  <pre>
 *  array{object_reference} Manager.GetAllDevices()
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult manager_get_all_devices(DBusConnection* connection,
                                                 DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusMessageIter iter_array;
    HalDevice* d;
    const char* udi;
    HalDeviceIterator iter_device;

    LOG_TRACE(("entering"));

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_array(&iter, &iter_array, DBUS_TYPE_STRING);
   
    for(ds_device_iter_begin(&iter_device);
        ds_device_iter_has_more(&iter_device);
        ds_device_iter_next(&iter_device))
    {
        d = ds_device_iter_get(&iter_device);
        /* only return devices in the GDL */
        if( d->in_gdl )
        {
            udi = ds_device_get_udi(d);
            dbus_message_iter_append_string(&iter_array, udi);
        }
    }

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref (reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Find devices in the GDL where a single string property matches a given
 *  value.
 *
 *  <pre>
 *  array{object_reference} Manager.FindDeviceStringMatch(string key,
 *                                                        string value)
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult manager_find_device_string_match(
    DBusConnection* connection,
    DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusMessageIter iter_array;
    DBusError error;
    const char* key;
    const char* value;
    HalDeviceIterator iter_device;
    int type;
    HalDevice* device;

    LOG_TRACE(("entering"));

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &key,
                               DBUS_TYPE_STRING, &value,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "Manager.FindDeviceStringMatch");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_array(&iter, &iter_array, DBUS_TYPE_STRING);

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
                dbus_message_iter_append_string(&iter_array, device->udi);
        }
    }


    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref (reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Find devices in the GDL with a given capability.
 *
 *  <pre>
 *  array{object_reference} Manager.FindDeviceByCapability(string capability)
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult manager_find_device_by_capability(
    DBusConnection* connection,
    DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusMessageIter iter_array;
    DBusError error;
    const char* capability;
    HalDeviceIterator iter_device;
    int type;
    HalDevice* device;

    LOG_TRACE(("entering"));

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &capability,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "Manager.FindDeviceByCapability");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_array(&iter, &iter_array, DBUS_TYPE_STRING);

    for(ds_device_iter_begin(&iter_device);
        ds_device_iter_has_more(&iter_device);
        ds_device_iter_next(&iter_device))
    {
        device = ds_device_iter_get(&iter_device);

        if( !device->in_gdl )
            continue;

        type = ds_property_get_type(device, "Capabilities");
        if( type==DBUS_TYPE_STRING )
        {
            if( strstr(ds_property_get_string(device, "Capabilities"),
                       capability)!=NULL )
                dbus_message_iter_append_string(&iter_array, device->udi);
        }
    }

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref (reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Determine if a device exists.
 *
 *  <pre>
 *  bool Manager.DeviceExists(string udi)
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult manager_device_exists(DBusConnection* connection,
                                               DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusError error;
    HalDevice* d;
    const char* udi;

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &udi,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "Manager.DeviceExists");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    LOG_TRACE(("entering, udi=%s", udi));

    d = ds_device_find(udi);

    reply = dbus_message_new_method_return(message);
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_boolean(&iter, d!=NULL);

    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Send signal DeviceAdded(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 *  @param  udi                 Unique Device Id
 */
static void manager_send_signal_device_added(const char* udi)
{
    DBusMessage* message;
    DBusMessageIter iter;

    LOG_TRACE(("entering, udi=%s", udi));

    message = dbus_message_new_signal("/org/freedesktop/Hal/Manager", 
                                      "org.freedesktop.Hal.Manager",
                                      "DeviceAdded");

    dbus_message_iter_init(message, &iter);
    dbus_message_iter_append_string(&iter, udi);

    if( !dbus_connection_send(dbus_connection,message, NULL) )
        DIE(("error broadcasting message"));

    dbus_message_unref(message);
}

/** Send signal DeviceRemoved(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 *  @param  udi                 Unique Device Id
 */
static void manager_send_signal_device_removed(const char* udi)
{
    DBusMessage* message;
    DBusMessageIter iter;

    LOG_TRACE(("entering, udi=%s", udi));

    message = dbus_message_new_signal("/org/freedesktop/Hal/Manager", 
                                      "org.freedesktop.Hal.Manager",
                                      "DeviceRemoved");

    dbus_message_iter_init(message, &iter);
    dbus_message_iter_append_string(&iter, udi);

    if( !dbus_connection_send(dbus_connection,message, NULL) )
        DIE(("error broadcasting message"));

    dbus_message_unref(message);
}

/** Send signal NewCapability(string udi, string capability) on the 
 *  org.freedesktop.Hal.Manager interface on the object 
 *  /org/freedesktop/Hal/Manager.
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability
 */
static void manager_send_signal_new_capability(const char* udi, 
                                               const char* capability)
{
    DBusMessage* message;
    DBusMessageIter iter;

    LOG_TRACE(("entering, udi=%s, cap=%s", udi, cap));

    message = dbus_message_new_signal("/org/freedesktop/Hal/Manager", 
                                      "org.freedesktop.Hal.Manager",
                                      "NewCapability");

    dbus_message_iter_init(message, &iter);
    dbus_message_iter_append_string(&iter, udi);
    dbus_message_iter_append_string(&iter, capability);

    if( !dbus_connection_send(dbus_connection,message, NULL) )
        DIE(("error broadcasting message"));

    dbus_message_unref(message);
}

/** @} */

/**
 * @defgroup DeviceInterface D-BUS interface org.freedesktop.Hal.Device
 * @ingroup HalDaemon
 * @brief D-BUS interface for generic device operations
 * @{
 */

/** Get all properties on a device.
 *
 *  <pre>
 *  map{string, any} Device.GetAllProperties()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_get_all_properties(DBusConnection* connection,
                                                   DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusMessageIter iter_dict;
    HalDevice* d;
    const char* udi;
    HalPropertyIterator iter_prop;

    udi = dbus_message_get_path(message);

    LOG_TRACE(("entering, udi=%s", udi));

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_dict(&iter, &iter_dict);

    for(ds_property_iter_begin(d, &iter_prop);
        ds_property_iter_has_more(&iter_prop);
        ds_property_iter_next(&iter_prop))
    {
        int type;
        HalProperty* p;
        const char* key;

        p = ds_property_iter_get(&iter_prop);
        key = ds_property_iter_get_key(p);
        type = ds_property_iter_get_type(p);

        dbus_message_iter_append_dict_key(&iter_dict, key);

        switch( type )
        {
        case DBUS_TYPE_STRING:
            dbus_message_iter_append_string(&iter_dict, 
                    ds_property_iter_get_string(p));
            break;
        case DBUS_TYPE_INT32:
            dbus_message_iter_append_int32(&iter_dict, 
                    ds_property_iter_get_int(p));
            break;
        case DBUS_TYPE_DOUBLE:
            dbus_message_iter_append_double(&iter_dict, 
                    ds_property_iter_get_double(p));
            break;
        case DBUS_TYPE_BOOLEAN:
            dbus_message_iter_append_boolean(&iter_dict, 
                    ds_property_iter_get_bool(p));
            break;

        default:
            LOG_WARNING(("Unknown property type %d", type));
            break;
        }
    }

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Get a property on a device.
 *
 *  <pre>
 *  any Device.GetProperty(string key)
 *  string Device.GetPropertyString(string key)
 *  int Device.GetPropertyInteger(string key)
 *  bool Device.GetPropertyBoolean(string key)
 *  double Device.GetPropertyDouble(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_get_property(DBusConnection* connection,
                                             DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusError error;
    HalDevice* d;
    const char* udi;
    char* key;
    int type;
    HalProperty* p;

    udi = dbus_message_get_path(message);

    LOG_TRACE(("entering, udi=%s", udi));

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &key,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "GetProperty");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    p = ds_property_find(d, key);
    if( p==NULL )
    {
        raise_no_such_property(connection, message, udi, key);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);

    type = ds_property_iter_get_type(p);
    switch( type )
    {
    case DBUS_TYPE_STRING:
        dbus_message_iter_append_string(&iter, 
                                        ds_property_iter_get_string(p));
        break;
    case DBUS_TYPE_INT32:
        dbus_message_iter_append_int32(&iter, 
                                       ds_property_iter_get_int(p));
        break;
    case DBUS_TYPE_DOUBLE:
        dbus_message_iter_append_double(&iter, 
                                        ds_property_iter_get_double(p));
        break;
    case DBUS_TYPE_BOOLEAN:
        dbus_message_iter_append_boolean(&iter, 
                                         ds_property_iter_get_bool(p));
        break;
        
    default:
        LOG_WARNING(("Unknown property type %d", type));
        break;
    }

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Get the type of a property on a device.
 *
 *  <pre>
 *  int Device.GetPropertyType(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_get_property_type(DBusConnection* connection,
                                                  DBusMessage* message)
{
    DBusMessage* reply;
    DBusMessageIter iter;
    DBusError error;
    HalDevice* d;
    const char* udi;
    char* key;
    HalProperty* p;

    udi = dbus_message_get_path(message);

    LOG_TRACE(("entering, udi=%s", udi));

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &key,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "GetPropertyType");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    p = ds_property_find(d, key);
    if( p==NULL )
    {
        raise_no_such_property(connection, message, udi, key);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_int32(&iter, ds_property_iter_get_type(p));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}



/** Set a property on a device.
 *
 *  <pre>
 *  void Device.SetProperty(string key, any value)
 *  void Device.SetPropertyString(string key, string value)
 *  void Device.SetPropertyInteger(string key, int value)
 *  void Device.SetPropertyBoolean(string key, bool value)
 *  void Device.SetPropertyDouble(string key, double value)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *           org.freedesktop.Hal.TypeMismatch
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_set_property(DBusConnection* connection,
                                             DBusMessage* message)
{
    const char* udi;
    char* key;
    int type;
    dbus_bool_t rc;
    HalDevice* device;
    DBusMessageIter iter;
    DBusMessage *reply;

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    dbus_message_iter_init(message, &iter);
    type = dbus_message_iter_get_arg_type(&iter);
    if( type!=DBUS_TYPE_STRING )
    {
        raise_syntax(connection, message, "SetProperty");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    key = dbus_message_iter_get_string(&iter);

    LOG_DEBUG(("udi=%s, key=%s", udi, key));

    device = ds_device_find(udi);
    if( device==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_message_iter_next(&iter);

    /** @todo check permissions of the sender vs property to be modified */

    type = dbus_message_iter_get_arg_type(&iter);
    rc = FALSE;
    switch( type )
    {
    case DBUS_TYPE_STRING:
        rc = ds_property_set_string(device, key, 
                                    dbus_message_iter_get_string(&iter));
        break;
    case DBUS_TYPE_INT32:
        rc = ds_property_set_int(device, key, 
                                 dbus_message_iter_get_int32(&iter));
        break;
    case DBUS_TYPE_DOUBLE:
        rc = ds_property_set_double(device, key, 
                                    dbus_message_iter_get_double(&iter));
        break;
    case DBUS_TYPE_BOOLEAN:
        rc = ds_property_set_bool(device, key, 
                                  dbus_message_iter_get_boolean(&iter));
        break;

    default:
        LOG_WARNING(("Unsupported property type %d", type));
        break;
    }

    if( !rc )
    {
        raise_property_type_error(connection, message, udi, key);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Maximum string length for capabilities; quite a hack :-/ */
#define MAX_CAP_SIZE 2048

/** This function is used to modify the Capabilities property. The reason
 *  for having a dedicated function is that the HAL daemon will broadcast
 *  a signal on the Manager interface to tell applications that the device
 *  have got a new capability.
 *
 *  This is useful as capabilities can be merged after the device is created.
 *  One example of this is networking cards under Linux 2.6; the net.ethernet
 *  capability is not merged when the device is initially found by looking in 
 *  /sys/devices; it is merged when the /sys/classes tree is searched.
 *
 *  Note that the signal is emitted every time this method is invoked even
 *  though the capability already existed. This is useful in the above
 *  scenario when the PCI class says ethernet networking card but we yet
 *  don't have enough information to fill in the net.* and net.ethernet.*
 *  fields since this only happens when we visit the /sys/classes tree.
 *
 *  <pre>
 *  void Device.AddCapability(string capability)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_add_capability(DBusConnection* connection,
                                               DBusMessage* message)
{
    const char* udi;
    const char* capability;
    const char* caps;
    HalDevice* d;
    DBusMessage *reply;
    DBusError error;
    char buf[MAX_CAP_SIZE];

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &capability,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "AddCapability");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }


    caps = ds_property_get_string(d, "Capabilities");
    if( caps==NULL )
    {
        ds_property_set_string(d, "Capabilities", capability);
    }
    else
    {
        if( strstr(caps, capability)==NULL )
        {
            snprintf(buf, MAX_CAP_SIZE, "%s %s", caps, capability);
            ds_property_set_string(d, "Capabilities", buf);
        }
    }

    if( d->in_gdl )
    {
        manager_send_signal_new_capability(udi, capability);
    }


    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}



/** Remove a property on a device.
 *
 *  <pre>
 *  void Device.RemoveProperty(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_remove_property(DBusConnection* connection,
                                                DBusMessage* message)
{
    const char* udi;
    char* key;
    HalDevice* d;
    DBusMessage *reply;
    DBusError error;

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &key,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "RemoveProperty");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if( !ds_property_remove(d, key) )
    {
        raise_no_such_property(connection, message, udi, key);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }


    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Determine if a property exists
 *
 *  <pre>
 *  bool Device.PropertyExists(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_property_exists(DBusConnection* connection,
                                                DBusMessage* message)
{
    const char* udi;
    char* key;
    HalDevice* d;
    DBusMessage *reply;
    DBusError error;
    DBusMessageIter iter;

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &key,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "RemoveProperty");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_boolean(&iter, ds_property_exists(d, key));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Determine if a device got a capability
 *
 *  <pre>
 *  bool Device.QueryCapability(string capability_name)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_query_capability(DBusConnection* connection,
                                                 DBusMessage* message)
{
    dbus_bool_t rc;
    const char* udi;
    const char* caps;
    char* capability;
    HalDevice* d;
    DBusMessage *reply;
    DBusError error;
    DBusMessageIter iter;

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &capability,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "QueryCapability");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    rc = FALSE;
    caps = ds_property_get_string(d, "Capabilities");
    if( caps!=NULL )
    {
        if( strstr(caps, capability)!=NULL )
            rc = TRUE;
    }

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_boolean(&iter, rc);

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** Enable a device.
 *
 *  <pre>
 *  void Device.Enable()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *           org.freedesktop.Hal.WrongState
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_enable(DBusConnection* connection,
                                       DBusMessage* message)
{
    const char* udi;
    HalDevice* d;
    DBusMessage *reply;
    DBusMessageIter iter;
    dbus_bool_t ok_to_enable;
    int device_state;

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    ok_to_enable = FALSE;
    if( ds_property_exists(d, "State") )
    {
        device_state = ds_property_get_int(d, "State");
        if( device_state==HAL_STATE_ENABLED ||
            device_state==HAL_STATE_UNPLUGGED )
            ok_to_enable = FALSE;
        else
            ok_to_enable = TRUE;
    }

    if( !ok_to_enable )
    {
        raise_wrong_state(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* Now enable device */
    LOG_INFO(("Enabling device %s", udi));
    
    /* First, make sure we got an .fdi file */
    if( (!ds_property_exists(d, "GotDeviceInfo")) || 
        (!ds_property_get_bool(d, "GotDeviceInfo")) )
    {
        /* need to search for an .fdi file */
        if( di_search_and_merge(d) )
        {
            /* Found .fdi file */

            /** @todo Check for required properties */

            ds_property_set_bool(d, "GotDeviceInfo", TRUE);
        }
        else
            ds_property_set_bool(d, "GotDeviceInfo", FALSE);
    }



    if( ds_property_get_bool(d, "GotDeviceInfo") )
    {
        /** @todo boot device */
        LOG_INFO(("Now booting device"));

        ds_property_set_int(d, "State", HAL_STATE_ENABLED);        
    }
    else
    {
        /* giving up */
        ds_property_set_int(d, "State", HAL_STATE_NEED_DEVICE_INFO);
    }


    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_boolean(&iter, ok_to_enable);

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));

    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Disable a device.
 *
 *  <pre>
 *  void Device.Disable()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *           org.freedesktop.Hal.WrongState
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult device_disable(DBusConnection* connection,
                                       DBusMessage* message)
{
    const char* udi;
    HalDevice* d;
    DBusMessage *reply;
    DBusMessageIter iter;
    dbus_bool_t ok_to_disable;
    int device_state;

    LOG_TRACE(("entering"));

    udi = dbus_message_get_path(message);

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    ok_to_disable = FALSE;
    if( ds_property_exists(d, "State") )
    {
        device_state = ds_property_get_int(d, "State");
        if( device_state==HAL_STATE_ENABLED )
            ok_to_disable = FALSE;
        else
            ok_to_disable = TRUE;
    }

    if( !ok_to_disable )
    {
        raise_wrong_state(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /** @todo: disable device */
    LOG_INFO(("Disabling device %s", udi));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_boolean(&iter, ok_to_disable);

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));

    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** @} */


/**
 * @defgroup AgentManagerInterface D-BUS interface org.freedesktop.Hal.AgentManager
 * @ingroup HalDaemon
 * @brief D-BUS interface for creating/destroying device objects
 *
 * @{
 */

/** Create a new device object which will be hidden from applications
 *  until the CommitToGdl() method is called. Returns an object that implements
 *  the org.freedesktop.Hal.Device interface.
 *
 *  <pre>
 *  object_reference AgentManager.NewDevice()
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult agent_manager_new_device(DBusConnection* connection,
                                                  DBusMessage* message)
{
    HalDevice* d;
    DBusMessage* reply;
    DBusMessageIter iter;
    const char* udi;

    LOG_TRACE(("entering"));

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    d = ds_device_new();
    udi = ds_device_get_udi(d);

    LOG_TRACE(("udi=%s", udi));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_string(&iter, udi);

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref (reply);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** When a hidden device have been built (using the Device interface),
 *  a HAL agent can commit it to the global device list using this method.
 *
 *  This means that the device object will be visible to desktop applications
 *  and the HAL daemon will possibly attempt to boot the device (depending
 *  on the property RequireEnable).
 *
 *  The parameter given is the new unique-device-id, which should determined
 *  from bus-specific information.
 *
 *  <pre>
 *  void AgentManager.CommitToGdl(object_reference device, 
 *                                string new_object_name)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *    raises org.freedesktop.Hal.UdiInUse
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult agent_manager_commit_to_gdl(
    DBusConnection* connection,
    DBusMessage* message)
{
    DBusMessage* reply;
    DBusError error;
    HalDevice* d;
    const char* old_udi;
    const char* new_udi;

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &old_udi,
                               DBUS_TYPE_STRING, &new_udi,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "CommitToGdl");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    LOG_TRACE(("entering, old_udi=%s, new_udi=%s", old_udi, new_udi));

    d = ds_device_find(old_udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, old_udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if( !ds_device_set_udi(d, new_udi) )
    {
        raise_udi_in_use(connection, message, new_udi);        
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* add to the GDL */
    ds_gdl_add(d);

    /* Ok, send out a signal on the Manager interface that we added
     * this device to the gdl */
    manager_send_signal_device_added(new_udi);

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** If a HAL agent determines that a device have been removed, this method
 *  can be called such that the HAL daemon can shutdown and possibly remove
 *  the device from the global device list (depending on the property
 *  Persistent).
 *
 *  <pre>
 *  void AgentManager.Remove(object_reference device,
 *                           string new_object_name)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult agent_manager_remove(DBusConnection* connection,
                                              DBusMessage* message)
{
    DBusMessage* reply;
    DBusError error;
    HalDevice* d;
    const char* udi;

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &udi,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "Remove");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    LOG_INFO(("entering, udi=%s", udi));

    d = ds_device_find(udi);
    if( d==NULL )
    {
        raise_no_such_device(connection, message, udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    ds_device_destroy(d);

    /* Ok, send out a signal on the Manager interface that we removed
     * this device from the gdl */
    manager_send_signal_device_removed(udi);

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}



/** Merge properties one device to another.
 *
 *  <pre>
 *  void AgentManager.MergeProperties(object_reference target,
 *                                    object_reference source)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult agent_merge_properties(DBusConnection* connection,
                                                DBusMessage* message)
{
    DBusMessage* reply;
    DBusError error;
    HalDevice* target_d;
    HalDevice* source_d;
    const char* target_udi;
    const char* source_udi;

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &target_udi,
                               DBUS_TYPE_STRING, &source_udi,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "MergeProperties");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    LOG_TRACE(("entering, target_udi=%s, source_udi=%s", 
               target_udi, source_udi));

    target_d = ds_device_find(target_udi);
    if( target_d==NULL )
    {
        raise_no_such_device(connection, message, target_udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    source_d = ds_device_find(source_udi);
    if( source_d==NULL )
    {
        raise_no_such_device(connection, message, source_udi);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    ds_device_merge(target_d, source_d);

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
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
 *  <pre>
 *  void AgentManager.DeviceMatches(object_reference device1,
 *                                  object_reference device,
 *                                  string namespace)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult agent_device_matches(DBusConnection* connection,
                                              DBusMessage* message)
{
    DBusMessage* reply;
    DBusError error;
    HalDevice* d1;
    HalDevice* d2;
    const char* udi1;
    const char* udi2;
    const char* namespace;
    dbus_bool_t rc;
    DBusMessageIter iter;

    dbus_error_init(&error);
    if( !dbus_message_get_args(message, &error, 
                               DBUS_TYPE_STRING, &udi1,
                               DBUS_TYPE_STRING, &udi2,
                               DBUS_TYPE_STRING, &namespace,
                               DBUS_TYPE_INVALID) )
    {
        raise_syntax(connection, message, "DeviceMatches");
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    LOG_TRACE(("entering, udi1=%s, udi2=%s, namespace=%s", 
               udi1, udi2, namespace));

    d1 = ds_device_find(udi1);
    if( d1==NULL )
    {
        raise_no_such_device(connection, message, udi1);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    d2 = ds_device_find(udi2);
    if( d2==NULL )
    {
        raise_no_such_device(connection, message, udi2);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    rc = ds_device_matches(d1, d2, namespace);

    reply = dbus_message_new_method_return(message);
    if( reply==NULL )
        DIE(("No memory"));

    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_append_boolean(&iter, rc);

    if( !dbus_connection_send(connection, reply, NULL) )
        DIE(("No memory"));
  
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/** @} */


/**
 * @defgroup MainDaemon Basic functions
 * @ingroup HalDaemon
 * @brief Basic functions in the HAL daemon
 * @{
 */


/** Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @param  user_data           User data
 *  @return                     What to do with the message
 */
static DBusHandlerResult filter_function(DBusConnection* connection,
                                         DBusMessage* message,
                                         void* user_data)
{
/*
    LOG_INFO(("obj_path=%s interface=%s method=%s", 
              dbus_message_get_path(message), 
              dbus_message_get_interface(message),
              dbus_message_get_member(message)));
*/

    if( dbus_message_is_method_call(message,
                                    "org.freedesktop.Hal.Manager",
                                    "GetAllDevices") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return manager_get_all_devices(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Manager",
                                         "DeviceExists") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return manager_device_exists(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Manager",
                                         "FindDeviceStringMatch") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return manager_find_device_string_match(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Manager",
                                         "FindDeviceByCapability") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return manager_find_device_by_capability(connection, message);
    }

    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.AgentManager",
                                         "NewDevice") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return agent_manager_new_device(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.AgentManager",
                                         "CommitToGdl") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return agent_manager_commit_to_gdl(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.AgentManager",
                                         "Remove") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return agent_manager_remove(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.AgentManager",
                                         "MergeProperties") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return agent_merge_properties(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.AgentManager",
                                         "DeviceMatches") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Manager")==0 )
    {
        return agent_device_matches(connection, message);
    }



    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "Enable") )
    {
        return device_enable(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "Disable") )
    {
        return device_disable(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetAllProperties") )
    {
        return device_get_all_properties(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetProperty") )
    {
        return device_get_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetPropertyString") )
    {
        return device_get_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetPropertyInteger") )
    {
        return device_get_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetPropertyBoolean") )
    {
        return device_get_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetPropertyDouble") )
    {
        return device_get_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "SetProperty") )
    {
        return device_set_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "SetPropertyString") )
    {
        return device_set_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "SetPropertyInteger") )
    {
        return device_set_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "SetPropertyBoolean") )
    {
        return device_set_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "SetPropertyDouble") )
    {
        return device_set_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "RemoveProperty") )
    {
        return device_remove_property(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "GetPropertyType") )
    {
        return device_get_property_type(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "PropertyExists") )
    {
        return device_property_exists(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "AddCapability") )
    {
        return device_add_capability(connection, message);
    }
    else if( dbus_message_is_method_call(message,
                                         "org.freedesktop.Hal.Device",
                                         "QueryCapability") )
    {
        return device_query_capability(connection, message);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Function in the HAL daemon that is called whenever a property on a device
 *  is changed. Will broadcast the changes using D-BUS signals.
 *
 *  @param  device              #HalDevice object
 *  @param  key                 Property that has changed
 *  @param  in_gdl              True iff the device object in visible in the
 *                              global device list
 *  @param  removed             True iff the property was removed
 *  @param  added               True iff the property was added
 */
static void property_changed(HalDevice* device,
                             const char* key, 
                             dbus_bool_t in_gdl, 
                             dbus_bool_t removed,
                             dbus_bool_t added)
{
    DBusMessage* message;
    DBusMessageIter iter;
    const char* signal_name;

    LOG_INFO(("Entering, udi=%s, key=%s, in_gdl=%s, removed=%s added=%s",
              device->udi, key, 
              in_gdl ? "true" : "false",
              removed ? "true" : "false",
              added ? "true" : "false"));

    if( !in_gdl )
        return;

    if( removed )
        signal_name = "PropertyRemoved";
    else if( added )
        signal_name = "PropertyAdded";
    else
        signal_name = "PropertyChanged";

    message = dbus_message_new_signal(device->udi, 
                                      "org.freedesktop.Hal.Device",
                                      signal_name);

    dbus_message_iter_init(message, &iter);
    dbus_message_iter_append_string(&iter, key);

    if( !dbus_connection_send(dbus_connection,message, NULL) )
        DIE(("error broadcasting message"));

    dbus_message_unref(message);
}


/** Entry point for HAL daemon
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @return                     Exit code
 */
int main(int argc, char* argv[])
{
    GMainLoop* loop;
    DBusError dbus_error;

    fprintf(stderr, "hald version " PACKAGE_VERSION "\r\n");

    // initialize the device store
    ds_init(property_changed);

    loop = g_main_loop_new (NULL, FALSE);

    dbus_connection_set_change_sigpipe(TRUE);

    dbus_error_init(&dbus_error);
    dbus_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_error);
    if( dbus_connection==NULL )
    {
        LOG_ERROR(("dbus_bus_get(): '%s'", dbus_error.message));
        exit(1);
    }

    dbus_connection_setup_with_g_main(dbus_connection, NULL);

    dbus_bus_acquire_service(dbus_connection, "org.freedesktop.Hal",
                             0, &dbus_error);
    if( dbus_error_is_set(&dbus_error) )
    {
        LOG_ERROR(("dbus_bus_acquire_service(): '%s'", dbus_error.message));
        exit(1);
    }

    dbus_connection_add_filter(dbus_connection, filter_function, NULL, NULL);

    // run the main loop
    g_main_loop_run (loop);

    return 0;
}

/** @} */

