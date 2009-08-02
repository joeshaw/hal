/***************************************************************************
 * CVSID: $Id$
 *
 * dbus.c : D-BUS interface of HAL daemon
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2007 Codethink Ltd. Author Rob Taylor <rob.taylor@codethink.co.uk>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/time.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "hald.h"
#include "hald_dbus.h"
#include "device.h"
#include "device_store.h"
#include "device_info.h"
#include "logger.h"
#include "osspec.h"
#include "util.h"
#include "hald_runner.h"
#include "ci-tracker.h"
#include "access-check.h"

#ifdef HAVE_CONKIT
#include "ck-tracker.h"
#endif

#define HALD_DBUS_ADDRESS "unix:tmpdir=" HALD_SOCKET_DIR

static DBusConnection *dbus_connection = NULL;
static CITracker *ci_tracker = NULL;
#ifdef HAVE_CONKIT
static CKTracker *ck_tracker = NULL;
#endif
static GHashTable *singletons = NULL;

static void foreach_property_append (HalDevice *device, 
                                     const char *key,
                                     gpointer user_data);

static void
raise_error (DBusConnection *connection,
	     DBusMessage *in_reply_to,
	     const char *error_name,
	     char *format, ...) __attribute__((format (printf, 4, 5)));

/** 
 *  raise_error:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @error_name:         D-Bus error name
 *  @format:             printf-style format for error message
 *
 *  Raise HAL error  
 */
static void
raise_error (DBusConnection *connection,
	     DBusMessage *in_reply_to,
	     const char *error_name,
	     char *format, ...)
{
	char buf[512];
	DBusMessage *reply;

	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof buf, format, args);
	va_end(args);

	/*HAL_WARNING ((buf));*/
	reply = dbus_message_new_error (in_reply_to, error_name, buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** 
 *  raise_no_such_device:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @udi:                Unique device id given
 *
 *  Raise the org.freedesktop.Hal.NoSuchDevice error 
 */
static void
raise_no_such_device (DBusConnection *connection,
		      DBusMessage *in_reply_to, const char *udi)
{
	raise_error (
		connection, in_reply_to,
		"org.freedesktop.Hal.NoSuchDevice",
		"No device with id %s",
		udi
	);
}

/** 
 *  raise_no_such_property:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @device_id:          Id of the device
 *  @key:                Key of the property that didn't exist
 *
 *  Raise the org.freedesktop.Hal.NoSuchProperty error 
 */
static void
raise_no_such_property (DBusConnection *connection,
			DBusMessage *in_reply_to,
			const char *device_id, const char *key)
{
	raise_error (
		connection, in_reply_to,
		"org.freedesktop.Hal.NoSuchProperty",
		"No property %s on device with id %s",
		key, device_id
	);
}

/** 
 *  raise_property_type_error:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @device_id:          Id of the device
 *  @key:                Key of the property
 *
 *  Raise the org.freedesktop.Hal.TypeMismatch error 
 */
static void
raise_property_type_error (DBusConnection *connection,
			   DBusMessage *in_reply_to,
			   const char *device_id, const char *key)
{
	raise_error (
		connection, in_reply_to,
		"org.freedesktop.Hal.TypeMismatch",
		"Type mismatch setting property %s on device with id %s",
		key, device_id
	);
}

/** 
 *  raise_syntax:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @method_name:        Name of the method that was invoked with
 *                       the wrong signature
 *
 *  Raise the org.freedesktop.Hal.SyntaxError error 
 */
static void
raise_syntax (DBusConnection *connection,
	      DBusMessage *in_reply_to, const char *method_name)
{
	raise_error (
		connection, in_reply_to,
		"org.freedesktop.Hal.SyntaxError",
		"There is a syntax error in the invocation of the method %s",
		method_name
	);
}

/** 
 *  raise_device_not_locked:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @device:             device which isn't locked
 *
 *  Raise the org.freedesktop.Hal.DeviceNotLocked error 
 */
static void
raise_device_not_locked (DBusConnection *connection,
			 DBusMessage    *in_reply_to,
			 HalDevice      *device)
{
	raise_error (
		connection, in_reply_to,
		"org.freedesktop.Hal.DeviceNotLocked",
		"The device %s is not locked",
		 hal_device_get_udi (device)
	);
}

/** 
 *  raise_device_already_locked:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @device:             device which isn't locked
 *
 *  Raise the org.freedesktop.Hal.DeviceAlreadyLocked error 
 */
static void
raise_device_already_locked (DBusConnection *connection,
			     DBusMessage    *in_reply_to,
			     HalDevice      *device)
{
	DBusMessage *reply;
	const char *reason;

	reason = hal_device_property_get_string (device, "info.locked.reason");
        if (reason != NULL) {
                HAL_WARNING (("Device %s is already locked: %s", hal_device_get_udi (device), reason));
        } else {
                HAL_WARNING (("Device %s is already locked", hal_device_get_udi (device)));
        }


	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.DeviceAlreadyLocked",
					reason != NULL ? reason : "Device is already locked");

	if (reply == NULL || !dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
}

/**
 *  raise_permission_denied:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @message:            what you're not allowed to do
 *
 *  Raise the org.freedesktop.Hal.PermissionDenied error 
 */
static void
raise_permission_denied (DBusConnection *connection,
			 DBusMessage    *in_reply_to,
			 const char     *reason)
{
	raise_error (
		connection, in_reply_to,
		"org.freedesktop.Hal.PermissionDenied",
		"Permission denied: %s",
		 reason
	);
}


/**
 *  raise_interface_locked:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @interface:          the interface that is locked
 *
 *  Raise the org.freedesktop.Hal.Device.InterfaceLocked error 
 */
static void
raise_interface_locked (DBusConnection *connection,
                        DBusMessage    *in_reply_to,
                        const char     *interface)
{
	raise_error (connection, in_reply_to,
                     "org.freedesktop.Hal.Device.InterfaceLocked",
                     "The interface %s is locked by someone else",
                     interface);
}

/**
 *  raise_interface_locked:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @interface:          the interface that is locked
 *
 *  Raise the org.freedesktop.Hal.Device.InterfaceLocked error 
 */
static void
raise_interface_already_locked (DBusConnection *connection,
                                DBusMessage    *in_reply_to,
                                const char     *interface)
{
	raise_error (connection, in_reply_to,
                     "org.freedesktop.Hal.Device.InterfaceAlreadyLocked",
                     "The interface %s is already exclusively locked either by someone else or it's already locked by yourself",
                     interface);
}

/**
 *  raise_interface_locked:
 *  @connection:         D-Bus connection
 *  @in_reply_to:        message to report error on
 *  @interface:          the interface that is locked
 *
 *  Raise the org.freedesktop.Hal.Device.InterfaceLocked error 
 */
static void
raise_interface_not_locked (DBusConnection *connection,
                                DBusMessage    *in_reply_to,
                                const char     *interface)
{
	raise_error (connection, in_reply_to,
                     "org.freedesktop.Hal.Device.InterfaceNotLocked",
                     "The interface %s is not locked by you",
                     interface);
}


static gboolean
foreach_device_get_udi (HalDeviceStore *store, HalDevice *device,
			gpointer user_data)
{
	DBusMessageIter *iter = user_data;
	const char *udi;

	udi = hal_device_get_udi (device);
	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &udi);

	return TRUE;
}

static gboolean
foreach_device_get_udi_with_properties (HalDeviceStore *store, HalDevice *device, gpointer user_data)
{
	DBusMessageIter *iter = user_data;
	DBusMessageIter iter_struct;
	DBusMessageIter iter_dict;
	const char *udi;

        dbus_message_iter_open_container (iter,
                                          DBUS_TYPE_STRUCT,
                                          NULL,
                                          &iter_struct);

	udi = hal_device_get_udi (device);
        dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &udi);

	dbus_message_iter_open_container (&iter_struct, 
					  DBUS_TYPE_ARRAY,
					  DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					  DBUS_TYPE_STRING_AS_STRING
					  DBUS_TYPE_VARIANT_AS_STRING
					  DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					  &iter_dict);

	hal_device_property_foreach (device, foreach_property_append, &iter_dict);

	dbus_message_iter_close_container (&iter_struct, &iter_dict);
        dbus_message_iter_close_container (iter, &iter_struct);
	return TRUE;
}

/** 
 *  manager_get_all_devices_with_properties:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Get all devices.
 *
 *  <pre>
 *  array{struct {object_reference, map{string, any}}} Manager.GetAllDevicesWithProperties()
 *  </pre>
 *
 */
DBusHandlerResult
manager_get_all_devices_with_properties (DBusConnection * connection,
                                         DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_open_container (&iter, 
					  DBUS_TYPE_ARRAY,
                                          "(sa{sv})",
					  &iter_array);

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_get_udi_with_properties,
				  &iter_array);

	dbus_message_iter_close_container (&iter, &iter_array);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

/** 
 *  manager_get_all_devices: 
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Get all devices.
 *
 *  <pre>
 *  array{object_reference} Manager.GetAllDevices()
 *  </pre>
 *
 */
DBusHandlerResult
manager_get_all_devices (DBusConnection * connection,
			 DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_open_container (&iter, 
					  DBUS_TYPE_ARRAY,
					  DBUS_TYPE_STRING_AS_STRING,
					  &iter_array);

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_get_udi,
				  &iter_array);

	dbus_message_iter_close_container (&iter, &iter_array);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct {
	const char *key;
	const char *value;
	DBusMessageIter *iter;
} DeviceMatchInfo;

static gboolean
foreach_device_match_get_udi (HalDeviceStore *store, HalDevice *device,
			      gpointer user_data)
{
	DeviceMatchInfo *info = user_data;
	const char *dev_value;

	if (hal_device_property_get_type (device,
					  info->key) != DBUS_TYPE_STRING)
		return TRUE;

	dev_value = hal_device_property_get_string (device, info->key);

	if (dev_value != NULL && strcmp (dev_value, info->value) == 0) {
		const char *udi;
		udi =  hal_device_get_udi (device);
		dbus_message_iter_append_basic  (info->iter,
						 DBUS_TYPE_STRING,
						 &udi);
	}

	return TRUE;
}

static gboolean
foreach_device_match_get_udi_tdl (HalDeviceStore *store, HalDevice *device,
				  gpointer user_data)
{
	DeviceMatchInfo *info = user_data;
	const char *dev_value;

	/* skip devices in the TDL that hasn't got a real UDI yet */
	if (strncmp (hal_device_get_udi (device), "/org/freedesktop/Hal/devices/temp",
		     sizeof ("/org/freedesktop/Hal/devices/temp")) == 0)
		return TRUE;

	if (hal_device_property_get_type (device,
					  info->key) != DBUS_TYPE_STRING)
		return TRUE;

	dev_value = hal_device_property_get_string (device, info->key);

	if (dev_value != NULL && strcmp (dev_value, info->value) == 0) {
		const char *udi;
		udi = hal_device_get_udi (device);

		dbus_message_iter_append_basic (info->iter,
						DBUS_TYPE_STRING,
						&udi);
	}

	return TRUE;
}

/**  
 *  manager_find_device_string_match:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *  
 *  Returns:             What to do with the message
 *
 *  Find devices in the GDL where a single string property matches a given
 *  value. Also returns devices in the TDL that has a non-tmp UDI.
 *
 *  <pre>
 *  array{object_reference} Manager.FindDeviceStringMatch(string key,
 *                                                        string value)
 *  </pre>
 *
 */
DBusHandlerResult
manager_find_device_string_match (DBusConnection * connection,
				  DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;
	DBusError error;
	const char *key;
	const char *value;
	DeviceMatchInfo info;

	HAL_TRACE (("entering"));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &value,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Manager.FindDeviceStringMatch");
		dbus_error_free (&error);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_open_container (&iter, 
					  DBUS_TYPE_ARRAY,
					  DBUS_TYPE_STRING_AS_STRING,
					  &iter_array);

	info.key = key;
	info.value = value;
	info.iter = &iter_array;

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_match_get_udi,
				  &info);

	/* Also returns devices in the TDL that has a non-tmp UDI */
	hal_device_store_foreach (hald_get_tdl (),
				  foreach_device_match_get_udi_tdl,
				  &info);

	dbus_message_iter_close_container (&iter, &iter_array);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct {
	const char *capability;
	DBusMessageIter *iter;
} DeviceCapabilityInfo;

static gboolean
foreach_device_by_capability (HalDeviceStore *store, HalDevice *device, gpointer user_data)
{
	DeviceCapabilityInfo *info = (DeviceCapabilityInfo *) user_data;

	if (hal_device_has_capability (device, info->capability)) {
		const char *udi;
		udi = hal_device_get_udi (device);
		dbus_message_iter_append_basic (info->iter,
						DBUS_TYPE_STRING,
						&udi);
	}

	return TRUE;
}

/**  
 *  manager_find_device_by_capability:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Find devices in the GDL with a given capability.
 *
 *  <pre>
 *  array{object_reference} Manager.FindDeviceByCapability(string capability)
 *  </pre>
 *
 */
DBusHandlerResult
manager_find_device_by_capability (DBusConnection * connection,
				   DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;
	DBusError error;
	const char *capability;
	DeviceCapabilityInfo info;

	HAL_TRACE (("entering"));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Manager.FindDeviceByCapability");
		dbus_error_free (&error);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_open_container (&iter, 
					  DBUS_TYPE_ARRAY,
					  DBUS_TYPE_STRING_AS_STRING,
					  &iter_array);

	info.capability = capability;
	info.iter = &iter_array;

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_by_capability,
				  &info);

	dbus_message_iter_close_container (&iter, &iter_array);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/**  
 *  manager_device_exists:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Determine if a device exists.
 *
 *  <pre>
 *  bool Manager.DeviceExists(string udi)
 *  </pre>
 *
 */
DBusHandlerResult
manager_device_exists (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;
	dbus_bool_t b;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Manager.DeviceExists");
		
		dbus_error_free (&error);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);

	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	reply = dbus_message_new_method_return (message);
	dbus_message_iter_init_append (reply, &iter);
	b = d != NULL;
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &b);

	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** 
 *  manager_send_signal_device_added:
 *  @device:             The HalDevice added
 *
 *  Send signal DeviceAdded(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 */
void
manager_send_signal_device_added (HalDevice *device)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

	HAL_TRACE (("entering, udi=%s", udi));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "DeviceAdded");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);

out:
	;
}

/** 
 *  manager_send_signal_device_removed:
 *  @device:             The HalDevice removed
 *
 *  Send signal DeviceRemoved(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 */
void
manager_send_signal_device_removed (HalDevice *device)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

	HAL_TRACE (("entering, udi=%s", udi));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "DeviceRemoved");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	;
}

/**  
 *  manager_send_signal_new_capability:
 *  @device:             HAL device
 *  @capability:         Capability
 *
 *  Send signal NewCapability(string udi, string capability) on the 
 *  org.freedesktop.Hal.Manager interface on the object 
 *  /org/freedesktop/Hal/Manager.
 *
 */
void
manager_send_signal_new_capability (HalDevice *device,
				    const char *capability)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

	HAL_TRACE (("entering, udi=%s, cap=%s", udi, capability));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "NewCapability");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &capability);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	;
}

void
manager_send_signal_interface_lock_acquired (const char *interface_name, const char *sender)
{
        const char *udi;
        char *lock_name;
        int num_locks;
        HalDevice *d;
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

	udi = "/org/freedesktop/Hal/devices/computer";
	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);
        if (d == NULL)
                goto out;

        lock_name = g_strdup_printf ("Global.%s", interface_name);
        num_locks = hal_device_get_num_lock_holders (d, lock_name);
        g_free (lock_name);

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "GlobalInterfaceLockAcquired");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &interface_name);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &sender);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &num_locks);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	;
}

void
manager_send_signal_interface_lock_released (const char *interface_name, const char *sender)
{
        const char *udi;
        char *lock_name;
        int num_locks;
        HalDevice *d;
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

	udi = "/org/freedesktop/Hal/devices/computer";
	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);
        if (d == NULL)
                goto out;

        lock_name = g_strdup_printf ("Global.%s", interface_name);
        num_locks = hal_device_get_num_lock_holders (d, lock_name);
        g_free (lock_name);

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "GlobalInterfaceLockReleased");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &interface_name);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &sender);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &num_locks);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	;
}

void
device_send_signal_interface_lock_acquired (HalDevice *device, const char *interface_name, const char *sender)
{
        int num_locks;
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

        num_locks = hal_device_get_num_lock_holders (device, interface_name);

	message = dbus_message_new_signal (hal_device_get_udi (device),
					   "org.freedesktop.Hal.Device",
					   "InterfaceLockAcquired");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &interface_name);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &sender);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &num_locks);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	;
}

void
device_send_signal_interface_lock_released (HalDevice *device, const char *interface_name, const char *sender)
{
        int num_locks;
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

        num_locks = hal_device_get_num_lock_holders (device, interface_name);

	message = dbus_message_new_signal (hal_device_get_udi (device),
					   "org.freedesktop.Hal.Device",
					   "InterfaceLockReleased");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &interface_name);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &sender);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &num_locks);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	;
}

static void
foreach_property_append (HalDevice *device, 
			 const char *key,
			 gpointer user_data)
{
	DBusMessageIter *iter;
	DBusMessageIter iter_dict_entry;
	int type;

	iter = (DBusMessageIter *)user_data;

	dbus_message_iter_open_container (iter,
					  DBUS_TYPE_DICT_ENTRY,
					  NULL,
					  &iter_dict_entry);

	type = hal_device_property_get_type (device, key);

	dbus_message_iter_append_basic (&iter_dict_entry, DBUS_TYPE_STRING, &key);

	switch (type) {
	case HAL_PROPERTY_TYPE_STRING:
	{
		DBusMessageIter iter_var;
		const char *v;

		v = hal_device_property_get_string (device, key);

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_var);

		dbus_message_iter_append_basic (&iter_var, 
						DBUS_TYPE_STRING,
						&v);

		dbus_message_iter_close_container (&iter_dict_entry,
						   &iter_var);
		break;
	}
	case HAL_PROPERTY_TYPE_INT32:
	{
		DBusMessageIter iter_var;
		dbus_int32_t v;

		v = hal_device_property_get_int (device, key);

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_INT32_AS_STRING,
						  &iter_var);

		dbus_message_iter_append_basic (&iter_var, 
						DBUS_TYPE_INT32,
						&v);

		dbus_message_iter_close_container (&iter_dict_entry,
						   &iter_var);
		break;
	}
	case HAL_PROPERTY_TYPE_UINT64:
	{
		DBusMessageIter iter_var;
		dbus_uint64_t v;

		v = hal_device_property_get_uint64 (device, key);

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_UINT64_AS_STRING,
						  &iter_var);

		dbus_message_iter_append_basic (&iter_var, 
						DBUS_TYPE_UINT64,
						&v);

		dbus_message_iter_close_container (&iter_dict_entry,
						   &iter_var);
		break;
	}
	case HAL_PROPERTY_TYPE_DOUBLE:
	{
		DBusMessageIter iter_var;
		double v;

		v = hal_device_property_get_double (device, key);

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_DOUBLE_AS_STRING,
						  &iter_var);

		dbus_message_iter_append_basic (&iter_var, 
						DBUS_TYPE_DOUBLE,
						&v);

		dbus_message_iter_close_container (&iter_dict_entry,
						   &iter_var);
		break;
	}
	case HAL_PROPERTY_TYPE_BOOLEAN:
	{
		DBusMessageIter iter_var;
		dbus_bool_t v;

		v = hal_device_property_get_bool (device, key);

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_BOOLEAN_AS_STRING,
						  &iter_var);

		dbus_message_iter_append_basic (&iter_var, 
						DBUS_TYPE_BOOLEAN,
						&v);

		dbus_message_iter_close_container (&iter_dict_entry,
						   &iter_var);
		break;
	}
	case HAL_PROPERTY_TYPE_STRLIST:
	{
		DBusMessageIter iter_var, iter_array;
		HalDeviceStrListIter hd_iter;

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_ARRAY_AS_STRING
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_var);

		dbus_message_iter_open_container (&iter_var,
						  DBUS_TYPE_ARRAY,
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_array);

		for (hal_device_property_strlist_iter_init (device, key, &hd_iter);
		     hal_device_property_strlist_iter_is_valid (&hd_iter);
		     hal_device_property_strlist_iter_next (&hd_iter)) {
			const char *v;
			v = hal_device_property_strlist_iter_get_value (&hd_iter);

			dbus_message_iter_append_basic (&iter_array, 
							DBUS_TYPE_STRING,
							&v);
		}
		
		dbus_message_iter_close_container (&iter_var,
						   &iter_array);

		dbus_message_iter_close_container (&iter_dict_entry,
						   &iter_var);
		break;
	}
		
	default:
		HAL_WARNING (("Unknown property type 0x%04x", type));
		break;
	}

	dbus_message_iter_close_container (iter, &iter_dict_entry);
}
		
	
	
/**  
 *  device_get_all_properties:
 *  @connection:         D-BUS connection
 *  @message:            D-Bus Message
 *
 *  Returns:             What to do with the message
 *
 *  Get all properties on a device.
 *
 *  <pre>
 *  map{string, any} Device.GetAllProperties()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 */
DBusHandlerResult
device_get_all_properties (DBusConnection * connection,
			   DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_dict;
	HalDevice *d;
	const char *udi;

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);

	dbus_message_iter_open_container (&iter, 
					  DBUS_TYPE_ARRAY,
					  DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					  DBUS_TYPE_STRING_AS_STRING
					  DBUS_TYPE_VARIANT_AS_STRING
					  DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					  &iter_dict);

	hal_device_property_foreach (d,
				     foreach_property_append,
				     &iter_dict);

	dbus_message_iter_close_container (&iter, &iter_dict);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 *  device_set_multiple_properties:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Set multiple properties on a device in an atomic fashion.
 *
 *  <pre>
 *  Device.GetAllProperties(map{string, any} properties)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice
 *  </pre>
 *
 */
static DBusHandlerResult
device_set_multiple_properties (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict_iter;
	HalDevice *d;
	const char *udi;

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "SetProperty: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_message_iter_init (message, &iter);

	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY  &&
	    dbus_message_iter_get_element_type (&iter) != DBUS_TYPE_DICT_ENTRY) {
		HAL_ERROR (("error, expecting an array of dict entries", __FILE__, __LINE__));
		raise_syntax (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_message_iter_recurse (&iter, &dict_iter);

	/* update atomically */
	device_property_atomic_update_begin ();

	while (dbus_message_iter_get_arg_type (&dict_iter) == DBUS_TYPE_DICT_ENTRY)
	{
		DBusMessageIter dict_entry_iter, var_iter, array_iter;
		const char *key;
		int change_type;
		dbus_bool_t rc;

		dbus_message_iter_recurse (&dict_iter, &dict_entry_iter);
		dbus_message_iter_get_basic (&dict_entry_iter, &key);

		dbus_message_iter_next (&dict_entry_iter);
		dbus_message_iter_recurse (&dict_entry_iter, &var_iter);
		change_type = dbus_message_iter_get_arg_type (&var_iter);

		rc = FALSE;

		switch (change_type) {
		case DBUS_TYPE_ARRAY:
		{
			gboolean _error;
			gboolean _added;

			_error = FALSE;
			_added = TRUE;

			if (dbus_message_iter_get_element_type (&var_iter) != DBUS_TYPE_STRING) {
				/* TODO: error */
			}
			dbus_message_iter_recurse (&var_iter, &array_iter);

			if (hal_device_has_property(d, key)) {
		 		if (hal_device_property_get_type(d, key) != HAL_PROPERTY_TYPE_STRLIST) {
					_error = TRUE;
				}
				_added = FALSE;
			} 

			hal_device_property_strlist_clear (d, key, TRUE);

			while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRING) {
				const char *v;
				dbus_message_iter_get_basic (&array_iter, &v);
				HAL_INFO ((" strlist elem %s -> %s", key, v));
				rc = hal_device_property_strlist_append (d, key, v, TRUE);
				dbus_message_iter_next (&array_iter);
			}
			
			if (!_error)
				hal_device_property_strlist_append_finish_changeset(d, key, _added);

			break;
		}
		case DBUS_TYPE_STRING:
		{
			const char *v;
			dbus_message_iter_get_basic (&var_iter, &v);
			HAL_INFO (("%s -> %s", key, v));
			rc = hal_device_property_set_string (d, key, v);
			break;
		}
		case DBUS_TYPE_INT32:
		{
			dbus_int32_t v;			
			dbus_message_iter_get_basic (&var_iter, &v);
			HAL_INFO (("%s -> %d", key, v));
			rc = hal_device_property_set_int (d, key, v);
			break;
		}
		case DBUS_TYPE_UINT64:
		{
			dbus_uint64_t v;
			dbus_message_iter_get_basic (&var_iter, &v);
			HAL_INFO (("%s -> %lld", key, v));
			rc = hal_device_property_set_uint64 (d, key, v);
			break;
		}
		case DBUS_TYPE_DOUBLE:
		{
			double v;
			dbus_message_iter_get_basic (&var_iter, &v);
			HAL_INFO (("%s -> %g", key, v));
			rc = hal_device_property_set_double (d, key, v);
			break;
		}
		case DBUS_TYPE_BOOLEAN:
		{
			gboolean v;
			dbus_message_iter_get_basic (&var_iter, &v);
			HAL_INFO (("%s -> %s", key, v ? "True" : "False"));
			rc = hal_device_property_set_bool (d, key, v);
			break;
		}
		default:
			/* TODO: error */
			break;
		}

		/* TODO: error out on rc==FALSE? */

		dbus_message_iter_next (&dict_iter);
	}

	device_property_atomic_update_end ();


	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/** 
 *  device_get_property:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Get a property on a device.
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
 */
DBusHandlerResult
device_get_property (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;
	char *key;
	int type;

	udi = dbus_message_get_path (message);

     	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "GetProperty");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!hal_device_has_property (d, key)) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);

	type = hal_device_property_get_type (d, key);
	switch (type) {
	case HAL_PROPERTY_TYPE_STRING:
	{
		const char *s;
		s = hal_device_property_get_string (d, key);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &s);
		break;
	}
	case HAL_PROPERTY_TYPE_INT32:
	{
		dbus_int32_t i;
		i = hal_device_property_get_int (d, key);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &i);
		break;
	}
	case HAL_PROPERTY_TYPE_UINT64:
	{
		dbus_uint64_t ul;
		ul = hal_device_property_get_uint64 (d, key);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT64, &ul);
		break;
	}
	case HAL_PROPERTY_TYPE_DOUBLE:
	{
		double vd;
		vd = hal_device_property_get_double (d, key);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_DOUBLE, &vd);
		break;
	}
	case HAL_PROPERTY_TYPE_BOOLEAN:
	{
		dbus_bool_t b;
		b = hal_device_property_get_bool (d, key);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &b);
		break;
	}
	case HAL_PROPERTY_TYPE_STRLIST:
	{
		HalDeviceStrListIter striter;
		DBusMessageIter iter_array;

		dbus_message_iter_open_container (&iter, 
						  DBUS_TYPE_ARRAY, 
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_array);

		for (hal_device_property_strlist_iter_init (d, key, &striter);
		     hal_device_property_strlist_iter_is_valid (&striter);
		     hal_device_property_strlist_iter_next (&striter)) {
			const char *v;
			v = hal_device_property_strlist_iter_get_value (&striter);
			dbus_message_iter_append_basic (&iter_array, DBUS_TYPE_STRING, &v);
		}

		dbus_message_iter_close_container (&iter, &iter_array);
	}
		break;

	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/** 
 *  device_get_property_type: 
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Get the type of a property on a device.
 *
 *  <pre>
 *  int Device.GetPropertyType(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *  </pre>
 *
 */
DBusHandlerResult
device_get_property_type (DBusConnection * connection,
			  DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;
	char *key;
	dbus_int32_t i;

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "GetPropertyType");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!hal_device_has_property (d, key)) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	i = hal_device_property_get_type (d, key);
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &i);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 *  device_set_property:  
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 * Set a property on a device.
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
 */
DBusHandlerResult
device_set_property (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	char *key;
	int type;
	dbus_bool_t rc;
	HalDevice *device;
	DBusMessageIter iter;
	DBusMessage *reply;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	dbus_message_iter_init (message, &iter);
	type = dbus_message_iter_get_arg_type (&iter);
	if (type != DBUS_TYPE_STRING) {
		raise_syntax (connection, message, "SetProperty");
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	dbus_message_iter_get_basic (&iter, &key);

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "SetProperty: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s, key=%s", udi, key));

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	dbus_message_iter_next (&iter);

	/** @todo check permissions of the sender vs property to be modified */

	type = dbus_message_iter_get_arg_type (&iter);
	rc = FALSE;

	switch (type) {
	case DBUS_TYPE_STRING:
	{
		const char *v;
		dbus_message_iter_get_basic (&iter, &v);
		rc = hal_device_property_set_string (device, key, v);
		break;
	}
	case DBUS_TYPE_INT32:
	{
		dbus_int32_t v;
		dbus_message_iter_get_basic (&iter, &v);
		rc = hal_device_property_set_int (device, key, v);
		break;
	}
	case DBUS_TYPE_UINT64:
	{
		dbus_uint64_t v;
		dbus_message_iter_get_basic (&iter, &v);
		rc = hal_device_property_set_uint64 (device, key, v);
		break;
	}
	case DBUS_TYPE_DOUBLE:
	{
		double v;
		dbus_message_iter_get_basic (&iter, &v);
		rc = hal_device_property_set_double (device, key, v);
		break;
	}
	case DBUS_TYPE_BOOLEAN:
	{
		dbus_bool_t v;
		dbus_message_iter_get_basic (&iter, &v);
		rc = hal_device_property_set_bool (device, key, v);
		break;
	}
	default:
		HAL_WARNING (("Unsupported property type %d", type));
		break;
	}

	if (!rc) {
		raise_property_type_error (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** 
 *  device_add_capability: 
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  This function is used to modify the Capabilities property. The reason
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
 *    raises org.freedesktop.Hal.PermissionDenied, 
 *  </pre>
 */
DBusHandlerResult
device_add_capability (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	const char *capability;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;

	HAL_TRACE (("entering"));

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "AddCapability: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "AddCapability");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}


	hal_device_add_capability (d, capability);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/* TODO: docs */
static DBusHandlerResult
device_string_list_append_prepend (DBusConnection * connection, DBusMessage * message, dbus_bool_t do_prepend)
{
	const char *udi;
	const char *key;
	const char *value;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	gboolean ret;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &value,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, do_prepend ? "StringListPrepend" : "StringListAppend");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (do_prepend)
		ret = hal_device_property_strlist_prepend (d, key, value);
	else
		ret = hal_device_property_strlist_append (d, key, value, FALSE);
	if (!ret) {
		raise_property_type_error (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/* TODO: docs */
static DBusHandlerResult
device_string_list_remove (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	const char *key;
	const char *value;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	gboolean ret;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &value,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "StringListRemove");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	ret = hal_device_property_strlist_remove (d, key, value);
	if (!ret) {
		raise_property_type_error (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));
	
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	
	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}



/**  
 *  device_remove_property:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Remove a property on a device.
 *
 *  <pre>
 *  void Device.RemoveProperty(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *           org.freedesktop.Hal.PermissionDenied
 *  </pre>
 */
DBusHandlerResult
device_remove_property (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	char *key;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "RemoveProperty: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "RemoveProperty");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!hal_device_property_remove (d, key)) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}


	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/** 
 *  device_property_exists:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Determine if a property exists
 *
 *  <pre>
 *  bool Device.PropertyExists(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 */
DBusHandlerResult
device_property_exists (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	char *key;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter iter;
	dbus_bool_t b;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "RemoveProperty");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	b =  hal_device_has_property (d, key);
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &b);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


/**  
 *  device_query_capability:
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Determine if a device has a capability
 *
 *  <pre>
 *  bool Device.QueryCapability(string capability_name)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *  </pre>
 *
 */
DBusHandlerResult
device_query_capability (DBusConnection * connection,
			 DBusMessage * message)
{
	dbus_bool_t rc;
	const char *udi;
	HalDeviceStrListIter striter;
	char *capability;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "QueryCapability");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	rc = FALSE;
	for (hal_device_property_strlist_iter_init (d, "info.capabilities", &striter);
	     hal_device_property_strlist_iter_is_valid (&striter);
	     hal_device_property_strlist_iter_next (&striter)) {
		if (strcmp (hal_device_property_strlist_iter_get_value (&striter), capability) == 0) {
			rc = TRUE;
			break;
		}
	}

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &rc);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/*------------------------------------------------------------------------*/
static DBusHandlerResult
device_acquire_interface_lock (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	char *interface_name;
	const char *sender;
        gboolean exclusive;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_BOOLEAN, &exclusive,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "AqcuireInterfaceLock");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

        if (!local_interface) {
                if (!access_check_caller_have_access_to_device (ci_tracker, d, "org.freedesktop.hal.lock", sender, NULL)) {
                        raise_permission_denied (connection, message, "AcquireInterfaceLock: no access to device");
                        return DBUS_HANDLER_RESULT_HANDLED;
                }
        }

        if (!hal_device_acquire_lock (d, interface_name, exclusive, sender)) {
		raise_interface_already_locked (connection, message, interface_name);
		return DBUS_HANDLER_RESULT_HANDLED;
        }

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
device_release_interface_lock (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	const char *sender;
	char *interface_name;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "ReleaseInterfaceLock");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

        /* don't require privileges to _release_ a lock */

        if (!hal_device_release_lock (d, interface_name, sender)) {
		raise_interface_not_locked (connection, message, interface_name);
		return DBUS_HANDLER_RESULT_HANDLED;
        }

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
device_is_caller_locked_out (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	const char *sender;
	char *interface_name;
	char *caller_sysbus_name;
        dbus_bool_t result;
        DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

        /* only allow HAL helpers / privileged users to ask this question */
        if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
                raise_permission_denied (connection, message, "IsCallerLockedOut: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
        }

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_STRING, &caller_sysbus_name,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "IsCallerLockedOut");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

        result = access_check_caller_locked_out (ci_tracker, d, caller_sysbus_name, interface_name);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &result);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/*------------------------------------------------------------------------*/

#ifdef HAVE_POLKIT
static DBusHandlerResult
device_is_caller_privileged (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	const char *sender;
	char *action;
	char *caller_sysbus_name;
        int polkit_result;
        const char *result;
        DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

        if (strcmp (dbus_message_get_signature (message), "ss") != 0) {
 		raise_syntax (connection, message, "IsCallerPrivileged");
		return DBUS_HANDLER_RESULT_HANDLED;
        }

        dbus_message_iter_init (message, &iter);
        dbus_message_iter_get_basic (&iter, &action);
        dbus_message_iter_next (&iter);
        dbus_message_iter_get_basic (&iter, &caller_sysbus_name);

        /* check whether we want to answer this question... */
        if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
                CICallerInfo *ci_sender;
                CICallerInfo *ci_target;

                if ((ci_sender = ci_tracker_get_info (ci_tracker, sender)) == NULL) {
                        raise_error (connection, message, 
                                     "org.freedesktop.Hal.Error", 
                                     "Could not determine caller info for sender");
                        return DBUS_HANDLER_RESULT_HANDLED;
                }
                if ((ci_target = ci_tracker_get_info (ci_tracker, caller_sysbus_name)) == NULL) {
                        raise_error (connection, message, 
                                     "org.freedesktop.Hal.Error", 
                                     "Could not determine caller info for target");
                        return DBUS_HANDLER_RESULT_HANDLED;
                }

                /* now check that sender uid(name) == uid(caller_sysbus_name)... */
                if (ci_tracker_caller_get_uid (ci_sender) != ci_tracker_caller_get_uid (ci_target)) {
                        raise_permission_denied (connection, message, 
                                                 "IsCallerPrivileged: not privileged/authorized to know");
                        return DBUS_HANDLER_RESULT_HANDLED;
                }
        }


        polkit_result = -1;
        access_check_caller_have_access_to_device (
                ci_tracker, d, action, caller_sysbus_name, &polkit_result);
        result = polkit_result_to_string_representation (polkit_result);

        if (polkit_result < 0 || result == NULL) {
		raise_error (connection, message, 
                             "org.freedesktop.Hal.Error", 
                             "Could not determine whether caller is privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
        }

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &result);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}
#else /* HAVE_POLKIT */
static DBusHandlerResult
device_is_caller_privileged (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
        /* we don't have PolicyKit */
        raise_error (connection, message, 
                     "org.freedesktop.Hal.Device.Error", 
                     "HAL is not built with PolicyKit support");
        return DBUS_HANDLER_RESULT_HANDLED;
}
#endif /* HAVE_POLKIT */

/*------------------------------------------------------------------------*/

static DBusHandlerResult
device_is_locked_by_others (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	const char *sender;
	char *interface_name;
        dbus_bool_t result;
        DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

        /* anyone can ask this question */

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "IsLockedByOthers");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

        result = access_check_locked_by_others (ci_tracker, d, sender, interface_name);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &result);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/*------------------------------------------------------------------------*/

static DBusHandlerResult
device_acquire_global_interface_lock (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	char *interface_name;
	char *lock_name;
	const char *sender;
        gboolean exclusive;

	HAL_TRACE (("entering"));

	udi = "/org/freedesktop/Hal/devices/computer";
	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

        /* no security checks; global locks are special; anyone can
         * take them but it might not have any effect... a global lock
         * will only have effect if a lock owner have access to the
         * device - see the function
         *
         *  hald/access-check.c:access_check_caller_locked_out()
         *
         * for details.
         */

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_BOOLEAN, &exclusive,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "AqcuireGlobalInterfaceLock");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

        lock_name = g_strdup_printf ("Global.%s", interface_name);
        if (!hal_device_acquire_lock (d, lock_name, exclusive, sender)) {
		raise_interface_already_locked (connection, message, interface_name);
                g_free (lock_name);
		return DBUS_HANDLER_RESULT_HANDLED;
        }
        g_free (lock_name);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
device_release_global_interface_lock (DBusConnection *connection, DBusMessage *message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	const char *sender;
	char *interface_name;
        char *lock_name;

	HAL_TRACE (("entering"));

	udi = "/org/freedesktop/Hal/devices/computer";
	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

        /* no security checks; global locks are special; anyone can
         * take them but it might not have any effect... a global lock
         * will only have effect if a lock owner have access to the
         * device - see the function
         *
         *  hald/access-check.c:access_check_caller_locked_out()
         *
         * for details.
         */

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "ReleaseGlobalInterfaceLock");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

        lock_name = g_strdup_printf ("Global.%s", interface_name);
        if (!hal_device_release_lock (d, lock_name, sender)) {
		raise_interface_not_locked (connection, message, interface_name);
                g_free (lock_name);
		return DBUS_HANDLER_RESULT_HANDLED;
        }
        g_free (lock_name);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/*------------------------------------------------------------------------*/

static GHashTable *services_with_locks = NULL;

static void
services_with_locks_add_lock (const char* lock_owner, HalDevice *device) {

	GSList *devices;

	devices = g_hash_table_lookup (services_with_locks, lock_owner);

        devices = g_slist_prepend (devices, device);

	g_hash_table_insert (services_with_locks, strdup(lock_owner), devices);
}

static void 
services_with_locks_remove_lock (const char* lock_owner, HalDevice *device) {
	
	GSList *devices;

	devices = g_hash_table_lookup (services_with_locks, lock_owner);

	devices = g_slist_remove (devices, device);

	g_hash_table_insert (services_with_locks, strdup(lock_owner), devices);
}

static void 
services_with_locks_remove_lockowner (const char* lock_owner) {
	
	GSList *devices;

	devices = g_hash_table_lookup (services_with_locks, lock_owner);

        if (devices != NULL) {
		GSList *iter;

		for (iter = devices; iter != NULL; iter = iter->next) {
			HalDevice *d = iter->data;
			hal_device_property_remove (d, "info.locked");
			hal_device_property_remove (d, "info.locked.reason");
			hal_device_property_remove (d, "info.locked.dbus_name");
		}
	}
	g_hash_table_remove (services_with_locks, lock_owner);		
}


/**  
 *  device_lock:
 *  @connection:         D-BUS connection
 *  @message:            Message
 * 
 *  Returns:             What to do with the message
 *
 *  Grab an advisory lock on a device.
 *
 *  <pre>
 *  bool Device.Lock(string reason)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.DeviceAlreadyLocked
 *  </pre>
 */
DBusHandlerResult
device_lock (DBusConnection * connection,
	     DBusMessage * message)
{
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	dbus_bool_t already_locked;
	DBusError error;
	char *reason;
	const char *sender;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	already_locked = hal_device_property_get_bool (d, "info.locked");

	if (already_locked) {
		raise_device_already_locked (connection, message, d);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &reason,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Lock");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	sender = dbus_message_get_sender (message);

	hal_device_property_set_bool (d, "info.locked", TRUE);
	hal_device_property_set_string (d, "info.locked.reason", reason);
	hal_device_property_set_string (d, "info.locked.dbus_name",
					sender);

	if (services_with_locks == NULL) {
		services_with_locks =
			g_hash_table_new_full (g_str_hash,
					       g_str_equal,
					       g_free,
					       g_object_unref);
	}

	services_with_locks_add_lock (sender, d);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** 
 *  device_unlock: 
 *  @connection:         D-BUS connection
 *  @message:            Message
 *
 *  Returns:             What to do with the message
 *
 *  Release an advisory lock on a device.
 *
 *  </pre>
 *  bool Device.Unlock()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.DeviceNotLocked,
 *           org.freedesktop.Hal.PermissionDenied
 *  </pre>
 */
DBusHandlerResult
device_unlock (DBusConnection *connection, 
 	       DBusMessage *message)
{
	dbus_bool_t rc;
	const char *udi;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	const char *sender;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi);

	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Unlock");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	rc = hal_device_property_get_bool (d, "info.locked");

	if (!rc) {
		raise_device_not_locked (connection, message, d);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	sender = dbus_message_get_sender (message);

	if (strcmp (sender, hal_device_property_get_string (
			    d, "info.locked.dbus_name")) != 0) {
		char *reason;

		reason = g_strdup_printf ("Service '%s' does not own the "
					  "lock on %s", sender,
					  hal_device_get_udi (d));

		raise_permission_denied (connection, message, reason);

		g_free (reason);

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (g_hash_table_lookup (services_with_locks, sender)) {
		services_with_locks_remove_lock (sender, d);
	} else {
		HAL_WARNING (("Service '%s' was not in the list of services "
			      "with locks!", sender));
	}

	hal_device_property_remove (d, "info.locked");
	hal_device_property_remove (d, "info.locked.reason");
	hal_device_property_remove (d, "info.locked.dbus_name");

	/* FIXME?  Pointless? */
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** Counter for atomic updating */
static int atomic_count = 0;

/** Number of updates pending */
static int num_pending_updates = 0;

/** Structure for queing updates */
typedef struct PendingUpdate_s {
	char *udi;                    /**< udi of device */
	char *key;                    /**< key of property; free when done */
	dbus_bool_t removed;          /**< true iff property was removed */
	dbus_bool_t added;            /**< true iff property was added */
	struct PendingUpdate_s *next; /**< next update or #NULL */
} PendingUpdate;

static PendingUpdate *pending_updates_head = NULL;

/** 
 *  device_property_atomic_update_begin:
 *
 *  Begin an atomic update - this is useful for updating several properties
 *  in one go.
 *
 *  Note that an atomic update is recursive - use with caution!
 */
void
device_property_atomic_update_begin (void)
{
	atomic_count++;
}

/** 
 *  device_property_atomic_update_end:
 *
 *  End an atomic update.
 *
 *  Note that an atomic update is recursive - use with caution!
 */
void
device_property_atomic_update_end (void)
{
	PendingUpdate *pu_iter = NULL;
	PendingUpdate *pu_iter_next = NULL;
	PendingUpdate *pu_iter2 = NULL;

	--atomic_count;

	if (atomic_count < 0) {
		HAL_WARNING (("*** atomic_count = %d < 0 !!", atomic_count));
		atomic_count = 0;
	}

	if (atomic_count == 0 && num_pending_updates > 0) {
		DBusMessage *message;
		DBusMessageIter iter;
		DBusMessageIter iter_array;

		for (pu_iter = pending_updates_head;
		     pu_iter != NULL; pu_iter = pu_iter_next) {
			int num_updates_this;

			pu_iter_next = pu_iter->next;

			/* see if we've already processed this */
			if (pu_iter->udi == NULL)
				goto already_processed;

			/* count number of updates for this device */
			num_updates_this = 0;
			for (pu_iter2 = pu_iter; pu_iter2 != NULL; pu_iter2 = pu_iter2->next) {
				if (strcmp (pu_iter2->udi, pu_iter->udi) == 0)
					num_updates_this++;
			}

			/* prepare message */
			message = dbus_message_new_signal (pu_iter->udi,
							   "org.freedesktop.Hal.Device",
							   "PropertyModified");
			dbus_message_iter_init_append (message, &iter);
			dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32,
							&num_updates_this);

			dbus_message_iter_open_container (&iter, 
							  DBUS_TYPE_ARRAY,
							  DBUS_STRUCT_BEGIN_CHAR_AS_STRING
							  DBUS_TYPE_STRING_AS_STRING
							  DBUS_TYPE_BOOLEAN_AS_STRING
							  DBUS_TYPE_BOOLEAN_AS_STRING
							  DBUS_STRUCT_END_CHAR_AS_STRING,
							  &iter_array);

			for (pu_iter2 = pu_iter; pu_iter2 != NULL;
			     pu_iter2 = pu_iter2->next) {
				if (strcmp (pu_iter2->udi, pu_iter->udi) == 0) {
					DBusMessageIter iter_struct;
					dbus_message_iter_open_container (&iter_array,
									  DBUS_TYPE_STRUCT,
									  NULL,
									  &iter_struct);
					dbus_message_iter_append_basic
					    (&iter_struct, 
					     DBUS_TYPE_STRING,
					     &(pu_iter2->key));
					dbus_message_iter_append_basic
					    (&iter_struct,
					     DBUS_TYPE_BOOLEAN,
					     &(pu_iter2->removed));
					dbus_message_iter_append_basic
					    (&iter_struct, 
					     DBUS_TYPE_BOOLEAN,
					     &(pu_iter2->added));

					dbus_message_iter_close_container (&iter_array, &iter_struct);

					/* signal this is already processed */
					g_free (pu_iter2->key);
					if (pu_iter2 != pu_iter) {
						g_free (pu_iter2->udi);
						pu_iter2->udi = NULL;
					}
				}
			}

			g_free (pu_iter->udi);
			dbus_message_iter_close_container (&iter, &iter_array);

			if (dbus_connection != NULL) {
				if (!dbus_connection_send (dbus_connection, message, NULL))
					DIE (("error broadcasting message"));
			}

			dbus_message_unref (message);

		already_processed:
			g_free (pu_iter);

		} /* for all updates */

		num_pending_updates = 0;
		pending_updates_head = NULL;
	}
}



void
device_send_signal_property_modified (HalDevice *device, const char *key,
				      dbus_bool_t added, dbus_bool_t removed)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

/*
    HAL_INFO(("Entering, udi=%s, key=%s, in_gdl=%s, removed=%s added=%s",
              hal_device_get_udi (device), key, 
              in_gdl ? "true" : "false",
              removed ? "true" : "false",
              added ? "true" : "false"));
*/

	if (atomic_count > 0) {
		PendingUpdate *pu;

		pu = g_new0 (PendingUpdate, 1);
		pu->udi = g_strdup (udi);
		pu->key = g_strdup (key);
		pu->removed = removed;
		pu->added = added;
		pu->next = pending_updates_head;

		pending_updates_head = pu;
		num_pending_updates++;
	} else {
		dbus_int32_t i;
		DBusMessageIter iter_struct;
		DBusMessageIter iter_array;

		if (dbus_connection == NULL || hald_is_initialising)
			goto out;
		
		message = dbus_message_new_signal (udi,
						  "org.freedesktop.Hal.Device",
						   "PropertyModified");

		dbus_message_iter_init_append (message, &iter);
		i = 1;
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &i);

		dbus_message_iter_open_container (&iter, 
						  DBUS_TYPE_ARRAY,
						  DBUS_STRUCT_BEGIN_CHAR_AS_STRING
						  DBUS_TYPE_STRING_AS_STRING
						  DBUS_TYPE_BOOLEAN_AS_STRING
						  DBUS_TYPE_BOOLEAN_AS_STRING
						  DBUS_STRUCT_END_CHAR_AS_STRING,
						  &iter_array);
		
		dbus_message_iter_open_container (&iter_array,
						  DBUS_TYPE_STRUCT,
						  NULL,
						  &iter_struct);
		
		dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &key);
		dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &removed);
		dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &added);

		dbus_message_iter_close_container (&iter_array, &iter_struct);
		dbus_message_iter_close_container (&iter, &iter_array);

		if (!dbus_connection_send (dbus_connection, message, NULL))
			DIE (("error broadcasting message"));

		dbus_message_unref (message);
	}
out:
	;
}

/**  
 * device_send_signal_condition:
 * @device:                  The device
 * @condition_name:          Name of condition
 * @condition_details:       Details of the condition as e.g. the button name
 *
 *  Emits a condition on a device; the device has to be in the GDL for
 *  this function to have effect.
 *
 *  Is intended for non-continuous events on the device like
 *  ProcesserOverheating, BlockDeviceGotDevice, e.g. conditions that
 *  are exceptional and may not be inferred by looking at properties
 *  (though some may).
 *
 *  This function accepts a number of parameters that are passed along
 *  in the D-BUS message. The recipient is supposed to extract the parameters
 *  himself, by looking at the HAL specification.
 */
void
device_send_signal_condition (HalDevice *device, const char *condition_name, const char *condition_details)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL || hald_is_initialising)
		goto out;

	message = dbus_message_new_signal (udi,
					   "org.freedesktop.Hal.Device",
					   "Condition");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter,
					DBUS_TYPE_STRING, 
					&condition_name);
	dbus_message_iter_append_basic (&iter,
					DBUS_TYPE_STRING, 
					&condition_details);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
out:
	return;
}



static gboolean
reinit_dbus (gpointer user_data)
{
	HAL_INFO (("entering!"));
	if (dbus_connection == NULL) 
		hald_dbus_init_preprobe ();
	if (hald_dbus_init ())
		return FALSE;
	else
		return TRUE;
}

static DBusHandlerResult
device_rescan (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *device;
	DBusMessage *reply;
	DBusMessageIter iter;
	gboolean res;	

	HAL_INFO (("entering, local_interface=%d", local_interface));

	udi = dbus_message_get_path (message);

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "Rescan: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s", udi));

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	res = osspec_device_rescan (device);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &res);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
device_reprobe (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *device;
	DBusMessageIter iter;
	DBusMessage *reply;
	gboolean res;
	
	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "Reprobe: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("device_reprobe() for udi=%s", udi));

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	res = osspec_device_reprobe (device);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &res);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}


static DBusHandlerResult
device_emit_condition (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *device;
	DBusMessageIter iter;
	DBusMessage *reply;
	DBusError error;
	const char *condition_name;
	const char *condition_details;
	dbus_bool_t res;
	
	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	if (!local_interface) {
		raise_permission_denied (connection, message, "EmitCondition: only allowed for helpers");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s", udi));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &condition_name,
				    DBUS_TYPE_STRING, &condition_details,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "EmitCondition");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}


	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	device_send_signal_condition (device, condition_name, condition_details);

	res = TRUE;

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &res);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct
{
	DBusConnection  *connection;
	char            *interface_name;
	char            *introspection_xml;
	char            *udi;
} HelperInterfaceHandler;

static GSList *helper_interface_handlers = NULL;

static DBusHandlerResult
device_claim_interface (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *device;
	DBusMessageIter iter;
	DBusMessage *reply;
	DBusError error;
	const char *interface_name;
	const char *introspection_xml;
	HelperInterfaceHandler *hih;
	dbus_bool_t res;
	
	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	if (!local_interface) {
		raise_permission_denied (connection, message, "ClaimInterface: only allowed for helpers");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s", udi));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &interface_name,
				    DBUS_TYPE_STRING, &introspection_xml,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "ClaimInterface");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	res = TRUE;

	HAL_INFO (("Local connection 0x%x to handle interface '%s' on udi '%s'",
		   connection,
		   interface_name, 
		   udi));

	hal_device_property_strlist_add (device, "info.interfaces", interface_name);

	hih = g_new0 (HelperInterfaceHandler, 1);
	hih->connection = connection;
	hih->interface_name = g_strdup (interface_name);
	hih->introspection_xml = g_strdup (introspection_xml);
	hih->udi = g_strdup (udi);
	helper_interface_handlers = g_slist_append (helper_interface_handlers, hih);
	

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &res);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}



static DBusHandlerResult
addon_is_ready (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	const char *udi;
	HalDevice *device;
	DBusMessageIter iter;
	DBusMessage *reply;
	DBusError error;
	dbus_bool_t res;
	
	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	if (!local_interface) {
		raise_permission_denied (connection, message, "AddonIsReady: only allowed for helpers");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s", udi));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "AddonIsReady");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL)
		device = hal_device_store_find (hald_get_tdl (), udi);

	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (hal_device_inc_num_ready_addons (device)) {
		if (hal_device_are_all_addons_ready (device)) {
			manager_send_signal_device_added (device);
		}
	}

	res = TRUE;

	HAL_INFO (("AddonIsReady on udi '%s'", udi));

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &res);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct _SingletonInfo SingletonInfo;
struct _SingletonInfo
{
	DBusConnection *connection;
	GList *devices;
};

static void
singleton_info_destroy (SingletonInfo *info)
{
	if (info->devices) {
		HAL_ERROR (("Singleton '%s' exited with devices still to handle"));
		/*should we signal the devices that the addon's gone?*/
		g_list_free (info->devices);
	}
	g_free(info);
}

static gboolean
singleton_remove_by_connection (char *udi,
			   SingletonInfo *info,
			   DBusConnection *connection)
{
	if (info->connection == connection)
		return TRUE;
	else
		return FALSE;
}

typedef struct _SingletonDeviceChangedInfo SingletonDeviceChangedInfo;
struct _SingletonDeviceChangedInfo
{
	gboolean added;
	HalDevice *device;
};

static SingletonDeviceChangedInfo* new_sdci (gboolean added, HalDevice *device)
{
	SingletonDeviceChangedInfo* sdci;
	sdci = g_new (SingletonDeviceChangedInfo, 1);
	sdci->added = added;
	sdci->device = device;
	g_object_add_weak_pointer (G_OBJECT (device), (gpointer) &(sdci->device));
	return sdci;
}

static
void del_sdci (SingletonDeviceChangedInfo *sdci)
{
	if (sdci->device)
		g_object_remove_weak_pointer (G_OBJECT (sdci->device),
					      (gpointer)&(sdci->device));
	g_free (sdci);
}

static void
reply_from_singleton_device_changed (DBusPendingCall *pending_call,
				     void            *user_data);

static void
singleton_signal_device_changed (DBusConnection *connection, HalDevice *device, gboolean added)
{
	DBusMessage *message;
	DBusMessageIter iter, iter_dict;
	DBusPendingCall *pending_call;
	const char *udi = hal_device_get_udi (device);

	message = dbus_message_new_method_call (NULL,
		"/org/freedesktop/Hal/SingletonAddon",
		"org.freedesktop.Hal.SingletonAddon",
		added ? "DeviceAdded" : "DeviceRemoved");

	if (message == NULL)
		DIE (("No memory"));
	dbus_message_iter_init_append (message, &iter);

	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING,
					&udi);

	dbus_message_iter_open_container (&iter,
					  DBUS_TYPE_ARRAY,
					  DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					  DBUS_TYPE_STRING_AS_STRING
					  DBUS_TYPE_VARIANT_AS_STRING
					  DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					  &iter_dict);

	hal_device_property_foreach (device,
				     foreach_property_append,
				     &iter_dict);

	dbus_message_iter_close_container (&iter, &iter_dict);

	HAL_DEBUG (("%s about to send message to connection %p", G_STRFUNC, connection));

	if (dbus_connection_send_with_reply (connection,
					     message,
					     &pending_call,
					     /*-1*/ 8000) && pending_call) {
		/*HAL_INFO (("connection=%x message=%x", connection, message));*/
		dbus_pending_call_set_notify (pending_call,
					      reply_from_singleton_device_changed,
					      (void *) new_sdci (added, device),
					      (DBusFreeFunction) del_sdci);
	}

	dbus_message_unref (message);
}

static void
reply_from_singleton_device_changed (DBusPendingCall *pending_call,
				     void            *user_data)
{
	DBusMessage *reply;
	DBusError error;
	SingletonDeviceChangedInfo *data = (SingletonDeviceChangedInfo*) user_data;

	reply = dbus_pending_call_steal_reply (pending_call);

	if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR) {
		dbus_set_error_from_message (&error, reply);

		HAL_ERROR (("Error calling %s for %s on singleton: %s: %s",
		      data->added ? "DeviceAdded" : "DeviceRemoved",
		      data->device ? hal_device_get_udi (data->device)
		                   : "a device that has been deleted",
		      error.name, error.message));

		dbus_error_free (&error);
		goto out;
	}

	/* if the device has been destroyed, data->device will be null */
	if (data->added && data->device) {
		HAL_DEBUG (("incrementing ready addons for device"));
		if (hal_device_inc_num_ready_addons (data->device)) {
			if (hal_device_are_all_addons_ready (data->device)) {
				manager_send_signal_device_added (data->device);
			}
		}
	}

out:
	dbus_message_unref (reply);
	dbus_pending_call_unref (pending_call);
}

/**
 * hald_singleton_device_added:
 * @command_line: command line identifying addon singleton
 * @device: device being added
 *
 * Signal to a singleton addon that a device has been added
 *
 * Return: TRUE if successful
 */
gboolean
hald_singleton_device_added (const char * command_line,
			     HalDevice *device)
{
	SingletonInfo *info;
	gchar *extra_env[2] = {NULL, NULL};

	if (!singletons)
		singletons = g_hash_table_new_full (
				g_str_hash, g_str_equal,
				g_free,
				(GDestroyNotify) singleton_info_destroy);
	info = g_hash_table_lookup (singletons, command_line);

	if (!info) {
		extra_env[0] = g_malloc (strlen (command_line) + 26);
		g_sprintf (extra_env[0], "SINGLETON_COMMAND_LINE=%s", command_line);
		if (hald_runner_start_singleton (command_line, extra_env, NULL, NULL, NULL)) {
			HAL_INFO (("Started singleton addon %s for udi %s",
				   command_line, hal_device_get_udi(device)));
		} else {
			HAL_ERROR (("Cannot start singleton addon %s for udi %s",
				    command_line, hal_device_get_udi(device)));
			return FALSE;
		}
		g_free (extra_env[0]);
		info = g_new0 (SingletonInfo, 1);
		g_hash_table_insert (singletons, g_strdup(command_line), info);
	}

	info->devices = g_list_prepend (info->devices, device);

	if (info->connection) {
		singleton_signal_device_changed (info->connection, device, TRUE);
	} else {
		HAL_DEBUG (("singleton not initialised yet"));
	}

        return TRUE;
}

/**
 * hald_singleton_device_removed:
 * @command_line: command line identifying addon singleton
 * @device: device that is being removed
 *
 * Signal to a singleton addon that a device has been removed
 *
 * Return: TRUE if successful
 */
gboolean
hald_singleton_device_removed (const char * command_line,
			       HalDevice *device)
{
	SingletonInfo *info;
	GList *lp;

	if (G_UNLIKELY (!singletons)) {
		HAL_ERROR (("Singleton table is not initialied"));
		return FALSE;
	}

	info = g_hash_table_lookup (singletons, command_line);

	if (G_UNLIKELY (!info)) {
		HAL_WARNING (("Attempting to remove a device from an unknown singleton %s", command_line));
		return FALSE;
	}

	lp = g_list_find (info->devices, device);
	if (G_UNLIKELY (!lp)) {
		HAL_WARNING (("Device was not in use by this singleton: %s", command_line));
		return FALSE;
	}

	info->devices = g_list_remove_link (info->devices, lp);
	g_list_free_1 (lp);

	if (G_LIKELY (info->connection)) {
		singleton_signal_device_changed (info->connection, device, FALSE);
	}

	return TRUE;
}

static DBusHandlerResult
singleton_addon_is_ready (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	HalDevice *device;
	SingletonInfo *info;
	DBusMessage *reply;
	DBusError error;
	const char *command_line;
	GList *lp;

	HAL_TRACE (("entering"));
	HAL_DEBUG (("singleton_addon_is_ready"));

	if (!local_interface) {
		raise_permission_denied (connection, message, "SingletonAddonIsReady: only allowed for helpers");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &command_line,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "SingletonAddonIsReady");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (G_UNLIKELY (!singletons)) {
		HAL_ERROR (("Got SingletonIsReady with no known singletons"));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	info = g_hash_table_lookup (singletons, command_line);

	if (G_UNLIKELY (!info)) {
		HAL_ERROR (("Got SingletonIsReady from unknown singleton"));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_INFO (("SingletonAddonIsReady recieved for '%s'", command_line));

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	info->connection = connection;

	HAL_DEBUG (("Signalling device added for queued devices"));
	for (lp = info->devices; lp; lp = lp->next) {
		device = lp->data;

		HAL_DEBUG (("device added for '%s'", hal_device_get_udi (device)));
		singleton_signal_device_changed (connection, device, TRUE);

	}


	return DBUS_HANDLER_RESULT_HANDLED;
}


/*
 * Create new device in tdl. Return temporary udi.
 */
DBusHandlerResult
manager_new_device (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	HalDevice *d;
	gchar *udi;
	int i;
	struct timeval tv;

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "NewDevice: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);
	d = hal_device_new ();

	gettimeofday(&tv, NULL);
	for (i = 0; i < 1000000 ; i++) {
		udi = g_strdup_printf ("/org/freedesktop/Hal/devices/tmp%05x", ((unsigned) tv.tv_usec & 0xfffff)) + i;
		if (!hal_device_store_find (hald_get_tdl (), udi)) break;
		g_free (udi);
		udi = NULL;
	}

	if (!udi) {
		raise_error (connection, message, "org.freedesktop.Hal.NoSpace", "NewDevice: no space for device");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	hal_device_set_udi (d, udi);
	hal_device_store_add (hald_get_tdl (), d);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);
	g_free (udi);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/*
 * Callout helper.
 */
static void 
manager_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Remove callouts completed udi=%s", hal_device_get_udi (d)));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}
}


/*
 * Remove device. Looks in gdl and tdl.
 */
DBusHandlerResult
manager_remove (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	char *udi;
	int in_tdl = 0;

	dbus_error_init (&error);

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "Remove: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Remove");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL) {
		hal_device_store_find (hald_get_tdl (), udi);
		in_tdl = 1;
	}
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* FIXME:
	 * run "info.callouts.remove" ?
	 * delete in gdl ?
	 * (auto) stop "info.addons" ?
	 */

	if (!in_tdl) {
		hal_util_callout_device_remove (d, manager_remove_done, NULL, NULL);
	}

	hal_device_store_remove (in_tdl ? hald_get_tdl () : hald_get_gdl (), d);
	g_object_unref (d);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


/*
 * Callout helper.
 */
static void
manager_commit_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));
}

/*
 * Preprobing helper.
 */
static void
manager_commit_preprobing_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	if (hal_device_property_get_bool (d, "info.ignore")) {
		/* Leave the device here with info.ignore==TRUE so we won't pick up children 
		 * Also remove category and all capabilities
		 */
		hal_device_property_remove (d, "info.category");
		hal_device_property_remove (d, "info.capabilities");
		hal_device_property_set_string (d, "info.udi", "/org/freedesktop/Hal/devices/ignored-device");
		hal_device_property_set_string (d, "info.product", "Ignored Device");

		HAL_INFO (("Preprobing merged info.ignore==TRUE"));

		return;
	}

	hal_util_callout_device_add (d, manager_commit_done, NULL, NULL);
	
	/* Merge properties from .fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);
}


/*
 * Move device from tdl to gdl. Runs helpers and callouts.
 */
DBusHandlerResult
manager_commit_to_gdl (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	char udi[256], *udi0, *tmp_udi;

	dbus_error_init (&error);

	if (!local_interface && !access_check_message_caller_is_root_or_hal (ci_tracker, message)) {
		raise_permission_denied (connection, message, "CommitToGdl: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &tmp_udi,
				    DBUS_TYPE_STRING, &udi0,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "CommitToGdl");
		dbus_error_free (&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);

	/* look it up in tdl */

	d = hal_device_store_find (hald_get_tdl (), tmp_udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, tmp_udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* sanity check & avoid races */
	hal_util_make_udi_unique (hald_get_gdl (), udi, sizeof udi, udi0);

	if (hal_device_store_find (hald_get_gdl (), udi)) {
		/* loose it */
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		raise_error (connection, message, "org.freedesktop.Hal.DeviceExists", "CommitToGdl: Device exists: %s", udi);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	/* set new udi */
	hal_device_property_remove (d, "info.udi");
	hal_device_set_udi (d, udi);

	/* FIXME:
	 * 'RequireEnable' property?
	 * fdi "preprobe"?
	 * run "info.callouts.preprobe"?
	 * remove "info.ignore" devices?
	 * fdi "information"?
	 * fdi "policy"?
	 * run "info.callouts.add"?
	 * tdl -> gdl?
	 * (auto) start "info.addons"?
	 */

	/* Process preprobe fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);
	hal_util_callout_device_preprobe (d, manager_commit_preprobing_done, NULL, NULL);

	/* move from tdl to gdl */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct {
	char *udi;
	char *execpath;
	char **extra_env;
	char *mstdin;
	char *member;
	char *interface;
	DBusMessage *message;
	DBusConnection *connection;
} MethodInvocation;

static void
hald_exec_method_cb (HalDevice *d, guint32 exit_type, 
		     gint return_code, gchar **error,
		     gpointer data1, gpointer data2);


static void
hald_exec_method_free_mi (MethodInvocation *mi)
{
	/* hald_runner_run_method() assumes ownership of mi->message.. so we don't free it here */
	g_free (mi->udi);
	g_free (mi->execpath);
	g_strfreev (mi->extra_env);
	g_free (mi->mstdin);
	g_free (mi->member);
	g_free (mi->interface);
	g_free (mi);
}

/* returns FALSE if we don't actually invoke anything */
static gboolean
hald_exec_method_do_invocation (MethodInvocation *mi)
{
	gboolean ret;
	HalDevice *d;

	ret = FALSE;

	d = hal_device_store_find (hald_get_gdl (), mi->udi);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), mi->udi);

	if (d != NULL) {
		/* no timeout */
		hald_runner_run_method(d, 
				       mi->execpath, 
				       mi->extra_env, 
				       mi->mstdin, 
				       TRUE,
				       0,
				       hald_exec_method_cb,
				       (gpointer) mi->message, 
				       (gpointer) mi->connection);

		ret = TRUE;
	} else {
		HAL_WARNING (("In-queue method call on non-existant device"));

		raise_no_such_device (mi->connection, mi->message, mi->udi);
	}

	return ret;
}


static GHashTable *udi_to_method_queue = NULL;


gboolean 
device_is_executing_method (HalDevice *d, const char *interface_name, const char *method_name)
{
	gpointer origkey;
	gboolean ret;
	GList *queue;

	ret = FALSE;

	if (udi_to_method_queue == NULL) {
		goto out;
	}

	if (g_hash_table_lookup_extended (udi_to_method_queue, hal_device_get_udi (d), &origkey, (gpointer) &queue)) {

		if (queue != NULL) {
			MethodInvocation *mi;
			mi = (MethodInvocation *) queue->data;
			if ((strcmp (mi->interface, interface_name) == 0) &&
			    (strcmp (mi->member, method_name) == 0)) {
				ret = TRUE;
			}
		}

		ret = TRUE;
	}
out:
	return ret;
}

static void 
hald_exec_method_process_queue (const char *udi);

static void
hald_exec_method_enqueue (MethodInvocation *mi)
{
	gpointer origkey;
	GList *queue;

	if (udi_to_method_queue == NULL) {
		udi_to_method_queue = g_hash_table_new_full (g_str_hash,
							     g_str_equal,
							     g_free,
							     NULL);
	}

	if (g_hash_table_lookup_extended (udi_to_method_queue, mi->udi, &origkey, (gpointer) &queue)) {
		HAL_INFO (("enqueue"));
		queue = g_list_append (queue, mi);
		g_hash_table_replace (udi_to_method_queue, g_strdup (mi->udi), queue);
	} else {
		HAL_INFO (("no need to enqueue"));
		queue = g_list_append (NULL, mi);
		g_hash_table_insert (udi_to_method_queue, g_strdup (mi->udi), queue);

		hald_exec_method_do_invocation (mi);
	}
}


static void 
hald_exec_method_process_queue (const char *udi)
{
	gpointer origkey;
	GList *queue;
	MethodInvocation *mi;

	if (g_hash_table_lookup_extended (udi_to_method_queue, udi, &origkey, (gpointer) &queue)) {

		/* clean the top of the list */
		if (queue != NULL) {
			mi = (MethodInvocation *) queue->data;
			queue = g_list_delete_link (queue, queue);
			if (queue == NULL) {
				g_hash_table_remove (udi_to_method_queue, udi);
				HAL_INFO (("No more methods in queue"));
			}

			/* if method was Volume.Unmount() then refresh mount state */
			if (strcmp (mi->interface, "org.freedesktop.Hal.Device.Volume") == 0 &&
			    strcmp (mi->member, "Unmount") == 0) {
				HalDevice *d;

				HAL_INFO (("Refreshing mount state for %s since Unmount() completed", mi->udi));

				d = hal_device_store_find (hald_get_gdl (), mi->udi);
				if (d == NULL) {
					d = hal_device_store_find (hald_get_tdl (), mi->udi);
				}

				if (d != NULL) {
					osspec_refresh_mount_state_for_block_device (d);
				} else {
					HAL_WARNING ((" Cannot find device object for %s", mi->udi));
				}
			}

			hald_exec_method_free_mi (mi);
		}

		/* process the rest of the list */
		if (queue != NULL) {
			HAL_INFO (("Execing next method in queue"));
			g_hash_table_replace (udi_to_method_queue, g_strdup (udi), queue);

			mi = (MethodInvocation *) queue->data;
			if (!hald_exec_method_do_invocation (mi)) {
				/* the device went away before we got to it... */
				hald_exec_method_process_queue (mi->udi);
			}
		}
	}
}

static void
hald_exec_method_cb (HalDevice *d, guint32 exit_type, 
		     gint return_code, gchar **error,
		     gpointer data1, gpointer data2)
{
	dbus_int32_t result;
	DBusMessage *reply = NULL;
	DBusMessage *message;
	DBusMessageIter iter;
	DBusConnection *conn;
	gchar *exp_name = NULL;
	gchar *exp_detail = NULL;

	hald_exec_method_process_queue (hal_device_get_udi (d));

	message = (DBusMessage *) data1;
	conn = (DBusConnection *) data2;
	
	if (exit_type == HALD_RUN_SUCCESS && error != NULL && 
	    error[0] != NULL && error[1] != NULL) {
		exp_name = error[0];
		if (error[0] != NULL) {
			exp_detail = g_strdup (error[1]);
		}
		HAL_INFO (("failed with '%s' '%s'", exp_name, exp_detail));
	}
	
	if (exit_type != HALD_RUN_SUCCESS) {
		reply = dbus_message_new_error (message, "org.freedesktop.Hal.Device.UnknownError", "An unknown error occured");
		if (conn != NULL) {
			if (!dbus_connection_send (conn, reply, NULL))
				DIE (("No memory"));
		}
		dbus_message_unref (reply);
	} else if (exp_name != NULL && exp_detail != NULL) {
		if (!is_valid_interface_name (exp_name)) {
			gchar *old_detail = exp_detail;

			exp_detail = g_strconcat (exp_name, " \n ", old_detail, NULL);
			exp_name = "org.freedesktop.Hal.Device.UnknownError";

			g_free (old_detail);
		}
		reply = dbus_message_new_error (message, exp_name, exp_detail);
                if (reply == NULL) {
                        DIE (("No memory"));
		}
		if (conn != NULL) {
			if (!dbus_connection_send (conn, reply, NULL))
				DIE (("No memory"));
		}
		dbus_message_unref (reply);

	} else {
		result = (dbus_int32_t) return_code;

		reply = dbus_message_new_method_return (message);
		if (reply == NULL)
			DIE (("No memory"));
		
		dbus_message_iter_init_append (reply, &iter);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &result);
		
		if (conn != NULL) {
			if (!dbus_connection_send (conn, reply, NULL))
				DIE (("No memory"));
		}

		dbus_message_unref (reply);
	}

	g_free(exp_detail);
	dbus_message_unref (message);
}

static DBusHandlerResult
hald_exec_method (HalDevice *d, CICallerInfo *ci, DBusConnection *connection, dbus_bool_t local_interface, 
		  DBusMessage *message, const char *execpath)
{
	int type;
	GString *stdin_str = NULL;
	DBusMessageIter iter;
	char *extra_env[6];
	char uid_export[256];
	char sender_export[256];
#ifdef HAVE_CONKIT
        char pid_export[256];
        char selinux_context_export[256];
        char ck_session_path_export[256];
#endif
	MethodInvocation *mi;

	/* add extra information about the caller... */
	extra_env[0] = NULL;
	extra_env[1] = NULL;
	extra_env[2] = NULL;
	extra_env[3] = NULL;
	extra_env[4] = NULL;
	extra_env[5] = NULL;
	if (local_interface) {
		extra_env[0] = "HAL_METHOD_INVOKED_BY_UID=0";
	} else {
                int n;
#ifdef HAVE_CONKIT
                const char *selinux_context;
                const char *ck_session_path;
#endif

                n = 0;
		sprintf (uid_export, "HAL_METHOD_INVOKED_BY_UID=%u", ci_tracker_caller_get_uid (ci));
		extra_env[n++] = uid_export;
		snprintf (sender_export, sizeof (sender_export), 
			  "HAL_METHOD_INVOKED_BY_SYSTEMBUS_CONNECTION_NAME=%s", 
                          dbus_message_get_sender (message));
		extra_env[n++] = sender_export;
#ifdef HAVE_CONKIT
		sprintf (pid_export, "HAL_METHOD_INVOKED_BY_PID=%u", ci_tracker_caller_get_pid (ci));
		extra_env[n++] = pid_export;
                selinux_context = ci_tracker_caller_get_selinux_context (ci);
                if (selinux_context != NULL) {
                        sprintf (selinux_context_export, "HAL_METHOD_INVOKED_BY_SELINUX_CONTEXT=%s", selinux_context);
                        extra_env[n++] = selinux_context_export;
                }

                /* caller may, or may not, be in a session.. if he is, export the session he belongs to... */
                ck_session_path = ci_tracker_caller_get_ck_session_path (ci);
                if (ck_session_path != NULL) {
			char * basename;

			basename = g_path_get_basename (ck_session_path); 
                        sprintf (ck_session_path_export, "HAL_METHOD_INVOKED_BY_SESSION=%s", basename);
                        extra_env[n++] = ck_session_path_export;
			g_free (basename);
                }
#endif /* HAVE_CONKIT */
	}

	/* prepare stdin with parameters */
	stdin_str = g_string_sized_new (256); /* default size for passing params; can grow */
	dbus_message_iter_init (message, &iter);
	while ((type = dbus_message_iter_get_arg_type (&iter)) != DBUS_TYPE_INVALID) {
		switch (type) {
		case DBUS_TYPE_BYTE:
		{
			unsigned char value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%u", value);
			break;
		}
		case DBUS_TYPE_INT16:
		{
			dbus_int16_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%d", value);
			break;
		}
		case DBUS_TYPE_UINT16:
		{
			dbus_uint16_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%u", value);
			break;
		}
		case DBUS_TYPE_INT32:
		{
			dbus_int32_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%d", value);
			break;
		}
		case DBUS_TYPE_UINT32:
		{
			dbus_uint32_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%u", value);
			break;
		}
		case DBUS_TYPE_INT64:
		{
			dbus_int64_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%lld", (long long int) value);
			break;
		}
		case DBUS_TYPE_UINT64:
		{
			dbus_uint64_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%llu", (long long unsigned int) value);
			break;
		}
		case DBUS_TYPE_DOUBLE:
		{
			double value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%g", value);
			break;
		}
		case DBUS_TYPE_BOOLEAN:
		{
			dbus_bool_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append (stdin_str, value ? "true" : "false");
			break;
		}
		case DBUS_TYPE_STRING:
		{
			char *value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append (stdin_str, value);
			break;
		}

		case DBUS_TYPE_ARRAY:
		{
			DBusMessageIter iter_strlist;
			if (dbus_message_iter_get_element_type (&iter) != DBUS_TYPE_STRING)
				goto error;

			dbus_message_iter_recurse (&iter, &iter_strlist);
			while (dbus_message_iter_get_arg_type (&iter_strlist) == DBUS_TYPE_STRING) {
				const char *value;
				dbus_message_iter_get_basic (&iter_strlist, &value);
				g_string_append (stdin_str, value);
				g_string_append (stdin_str, "\t");
				dbus_message_iter_next(&iter_strlist);
			}
			break;
		}

		default:
			goto error;
		}

		g_string_append_c (stdin_str, '\n');
		dbus_message_iter_next (&iter);
	}

	mi = g_new0 (MethodInvocation, 1);
	mi->udi = g_strdup (hal_device_get_udi (d));
	mi->execpath = g_strdup (execpath);
	mi->extra_env = g_strdupv (extra_env);
	mi->mstdin = g_strdup (stdin_str->str);
	mi->message = message;
	mi->connection = connection;
	mi->member = g_strdup (dbus_message_get_member (message));
	mi->interface = g_strdup (dbus_message_get_interface (message));
	hald_exec_method_enqueue (mi);

	dbus_message_ref (message);
	g_string_free (stdin_str, TRUE);
	return DBUS_HANDLER_RESULT_HANDLED;

error:
	g_string_free (stdin_str, TRUE);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static gboolean
foreach_device_get_xml_node (HalDeviceStore *store, HalDevice *device,
			     gpointer user_data)
{
	GString *xml = user_data;
	const char *udi, *name;

	udi = hal_device_get_udi (device);
	name = strrchr(udi, '/')+1;

	xml = g_string_append(xml, "  <node name=\"");
	xml = g_string_append(xml, name);
	xml = g_string_append(xml, "\"/>\n");

	return TRUE;
}

static DBusHandlerResult
do_introspect (DBusConnection  *connection, 
	       DBusMessage     *message,
	       dbus_bool_t      local_interface)
{
	const char *path;
	DBusMessage *reply;
	GString *xml;
	char *xml_string;

	HAL_TRACE (("entering do_introspect"));

	path = dbus_message_get_path (message);

	xml = g_string_new ("<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
			    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
			    "<node>\n"
			    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
			    "    <method name=\"Introspect\">\n"
			    "      <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"
			    "    </method>\n"
			    "  </interface>\n");

	if (strcmp (path, "/") == 0) {

		xml = g_string_append (xml,
				       "  <node name=\"org\"/>\n");

	} else if (strcmp (path, "/org") == 0) {

		xml = g_string_append (xml,
				       "  <node name=\"freedesktop\"/>\n");

	} else if (strcmp (path, "/org/freedesktop") == 0) {

		xml = g_string_append (xml,
				       "  <node name=\"Hal\"/>\n");

	} else if (strcmp (path, "/org/freedesktop/Hal") == 0) {

		xml = g_string_append (xml,
				       "  <node name=\"Manager\"/>\n"
				       "  <node name=\"devices\"/>\n");

	} else if (strcmp (path, "/org/freedesktop/Hal/devices") == 0) {

		hal_device_store_foreach (hald_get_gdl (),
					  foreach_device_get_xml_node,
					  xml);

	} else if (strcmp (path, "/org/freedesktop/Hal/Manager") == 0) {

		xml = g_string_append (xml,
				       "  <interface name=\"org.freedesktop.Hal.Manager\">\n"
				       "    <method name=\"GetAllDevices\">\n"
				       "      <arg name=\"devices\" direction=\"out\" type=\"as\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetAllDevicesWithProperties\">\n"
				       "      <arg name=\"devices_with_props\" direction=\"out\" type=\"a(sa{sv})\"/>\n"
				       "    </method>\n"
				       "    <method name=\"DeviceExists\">\n"
				       "      <arg name=\"does_it_exist\" direction=\"out\" type=\"b\"/>\n"
				       "      <arg name=\"udi\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"FindDeviceStringMatch\">\n"
				       "      <arg name=\"devices\" direction=\"out\" type=\"as\"/>\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"FindDeviceByCapability\">\n"
				       "      <arg name=\"devices\" direction=\"out\" type=\"as\"/>\n"
				       "      <arg name=\"capability\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"NewDevice\">\n"
				       "      <arg name=\"temporary_udi\" direction=\"out\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"Remove\">\n"
				       "      <arg name=\"udi\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"CommitToGdl\">\n"
				       "      <arg name=\"temporary_udi\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"global_udi\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"

				       "    <method name=\"AcquireGlobalInterfaceLock\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"exclusive\" direction=\"in\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"ReleaseGlobalInterfaceLock\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SingletonAddonIsReady\">\n"
				       "      <arg name=\"command_line\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <signal name=\"DeviceAdded\">\n"
				       "      <arg name=\"udi\" type=\"s\"/>\n"
				       "    </signal>\n"
				       "    <signal name=\"DeviceRemoved\">\n"
				       "      <arg name=\"udi\" type=\"s\"/>\n"
				       "    </signal>\n"
				       "    <signal name=\"NewCapability\">\n"
				       "      <arg name=\"udi\" type=\"s\"/>\n"
				       "      <arg name=\"cap_name\" type=\"s\"/>\n"
				       "    </signal>\n"
				       "    <signal name=\"GlobalInterfaceLockAcquired\">\n"
				       "      <arg name=\"interface_name\" type=\"s\"/>\n"
				       "      <arg name=\"lock_holder\" type=\"s\"/>\n"
				       "      <arg name=\"num_locks\" type=\"i\"/>\n"
				       "    </signal>\n"
				       "    <signal name=\"GlobalInterfaceLockReleased\">\n"
				       "      <arg name=\"interface_name\" type=\"s\"/>\n"
				       "      <arg name=\"lock_holder\" type=\"s\"/>\n"
				       "      <arg name=\"num_locks\" type=\"i\"/>\n"
				       "    </signal>\n"

				       "  </interface>\n");
	} else {
		HalDevice *d;
		HalDeviceStrListIter if_iter;

		d = hal_device_store_find (hald_get_gdl (), path);
		if (d == NULL)
			d = hal_device_store_find (hald_get_tdl (), path);
		
		if (d == NULL) {
			raise_no_such_device (connection, message, path);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		xml = g_string_append (xml,
				       "  <interface name=\"org.freedesktop.Hal.Device\">\n"
				       "    <method name=\"GetAllProperties\">\n"
				       "      <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetMultipleProperties\">\n"
				       "      <arg name=\"properties\" direction=\"in\" type=\"a{sv}\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetProperty\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"out\" type=\"v\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetPropertyString\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"out\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetPropertyStringList\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"out\" type=\"as\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetPropertyInteger\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"out\" type=\"i\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetPropertyBoolean\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetPropertyDouble\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"out\" type=\"d\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetProperty\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"v\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetPropertyString\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetPropertyStringList\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"as\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetPropertyInteger\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"i\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetPropertyBoolean\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"SetPropertyDouble\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"d\"/>\n"
				       "    </method>\n"

				       "    <method name=\"RemoveProperty\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"GetPropertyType\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"type\" direction=\"out\" type=\"i\"/>\n"
				       "    </method>\n"
				       "    <method name=\"PropertyExists\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"does_it_exist\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"AddCapability\">\n"
				       "      <arg name=\"capability\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"QueryCapability\">\n"
				       "      <arg name=\"capability\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"does_it_have_capability\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"Lock\">\n"
				       "      <arg name=\"reason\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"acquired_lock\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"Unlock\">\n"
				       "      <arg name=\"released_lock\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n");

		xml = g_string_append (xml,
				       "    <method name=\"AcquireInterfaceLock\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"exclusive\" direction=\"in\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"ReleaseInterfaceLock\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"IsCallerLockedOut\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"caller_sysbus_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"whether_caller_is_locked_out\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"IsCallerPrivileged\">\n"
				       "      <arg name=\"action\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"caller_sysbus_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"result\" direction=\"out\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"IsLockedByOthers\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"whether_it_is_locked_by_others\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"

				       "    <method name=\"StringListAppend\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"StringListPrepend\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"StringListRemove\">\n"
				       "      <arg name=\"key\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"value\" direction=\"in\" type=\"s\"/>\n"
				       "    </method>\n"
				       "    <method name=\"EmitCondition\">\n"
				       "      <arg name=\"condition_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"condition_details\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"rc\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"

				       "    <method name=\"Rescan\">\n"
				       "      <arg name=\"call_had_sideeffect\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <method name=\"Reprobe\">\n"
				       "      <arg name=\"call_had_sideeffect\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"

				       "    <method name=\"ClaimInterface\">\n"
				       "      <arg name=\"interface_name\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"introspection_xml\" direction=\"in\" type=\"s\"/>\n"
				       "      <arg name=\"rc\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"

				       "    <method name=\"AddonIsReady\">\n"
				       "      <arg name=\"rc\" direction=\"out\" type=\"b\"/>\n"
				       "    </method>\n"
				       "    <signal name=\"PropertyModified\">\n"
				       "      <arg name=\"num_updates\" type=\"i\"/>\n"
				       "      <arg name=\"updates\" type=\"a(sbb)\"/>\n"
				       "    </signal>\n"
				       "    <signal name=\"Condition\">\n"
				       "      <arg name=\"cond_name\" type=\"s\"/>\n"
				       "      <arg name=\"cond_details\" type=\"s\"/>\n"
				       "    </signal>\n"

				       "    <signal name=\"InterfaceLockAcquired\">\n"
				       "      <arg name=\"interface_name\" type=\"s\"/>\n"
				       "      <arg name=\"lock_holder\" type=\"s\"/>\n"
				       "      <arg name=\"num_locks\" type=\"i\"/>\n"
				       "    </signal>\n"
				       "    <signal name=\"InterfaceLockReleased\">\n"
				       "      <arg name=\"interface_name\" type=\"s\"/>\n"
				       "      <arg name=\"lock_holder\" type=\"s\"/>\n"
				       "      <arg name=\"num_locks\" type=\"i\"/>\n"
				       "    </signal>\n"

				       "  </interface>\n");

			for (hal_device_property_strlist_iter_init (d, "info.interfaces", &if_iter);
			     hal_device_property_strlist_iter_is_valid (&if_iter);
			     hal_device_property_strlist_iter_next (&if_iter)) {
				const char *ifname;
				char *method_name_prop;
				char *method_sign_prop;
				char *method_argn_prop;
				GSList *i;
				HalDeviceStrListIter name_iter;
				HalDeviceStrListIter sign_iter;
				HalDeviceStrListIter argn_iter;

				ifname = hal_device_property_strlist_iter_get_value (&if_iter);

				g_string_append_printf (xml, "  <interface name=\"%s\">\n", ifname);

				method_name_prop = g_strdup_printf ("%s.method_names", ifname);
				method_sign_prop = g_strdup_printf ("%s.method_signatures", ifname);
				method_argn_prop = g_strdup_printf ("%s.method_argnames", ifname);

				/* consult local list */
				for (i = helper_interface_handlers; i != NULL; i = g_slist_next (i)) {
					HelperInterfaceHandler *hih = i->data;
					if (strcmp (hih->udi, path) == 0 &&
                                            strcmp (hih->interface_name, ifname) == 0) {
						xml = g_string_append (xml, hih->introspection_xml);
					}
				}
				
				for (hal_device_property_strlist_iter_init (d, method_name_prop, &name_iter),
					     hal_device_property_strlist_iter_init (d, method_sign_prop, &sign_iter),
					     hal_device_property_strlist_iter_init (d, method_argn_prop, &argn_iter);
				     hal_device_property_strlist_iter_is_valid (&name_iter) &&
					     hal_device_property_strlist_iter_is_valid (&sign_iter) &&
					     hal_device_property_strlist_iter_is_valid (&argn_iter);
				     hal_device_property_strlist_iter_next (&name_iter),
					     hal_device_property_strlist_iter_next (&sign_iter),
					     hal_device_property_strlist_iter_next (&argn_iter)) {
					const char *name;
					const char *sig;
					const char *argnames;
					gchar **args;
					unsigned int n;
					unsigned int m;

					name     = hal_device_property_strlist_iter_get_value (&name_iter);
					sig      = hal_device_property_strlist_iter_get_value (&sign_iter);
					argnames = hal_device_property_strlist_iter_get_value (&argn_iter);

					args = g_strsplit (argnames, " ", 0);

					g_string_append_printf (xml, "    <method name=\"%s\">\n", name);

					for (n = 0, m = 0; n < strlen (sig) && args[m] != NULL; n++, m++) {
						switch (sig[n]) {
						case 'a':
							if (n == strlen (sig) - 1) {
								HAL_WARNING (("Broken signature for method %s "
									      "on interface %s for object %s", 
									      name, ifname, path));
								continue;
							}
							g_string_append_printf (
							  xml, 
							  "      <arg name=\"%s\" direction=\"in\" type=\"a%c\"/>\n", 
							  args[m], sig[n + 1]);
							n++;
							break;

						default:
							g_string_append_printf (
							  xml, 
							  "      <arg name=\"%s\" direction=\"in\" type=\"%c\"/>\n", 
							  args[m], sig[n]);
							break;
						}
					}
					xml = g_string_append (
						xml, 
						"      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n");
					xml = g_string_append  (
						xml, 
						"    </method>\n");

					g_strfreev(args);
				}
				

				xml = g_string_append (xml, "  </interface>\n");

				g_free (method_name_prop);
				g_free (method_sign_prop);
				g_free (method_argn_prop);
			}		

	}

	reply = dbus_message_new_method_return (message);

	xml = g_string_append (xml, "</node>\n");
	xml_string = g_string_free (xml, FALSE);

	dbus_message_append_args (reply, 
				  DBUS_TYPE_STRING, &xml_string,
				  DBUS_TYPE_INVALID);

	g_free (xml_string);

	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void
reply_from_fwd_message (DBusPendingCall *pending_call,
			void            *user_data)
{
	DBusMessage *reply_from_addon;
	DBusMessage *method_from_caller;
	DBusMessage *reply;

	/*HAL_INFO (("in reply_from_fwd_message : user_data = 0x%x", user_data));*/

	method_from_caller = (DBusMessage *) user_data;
	reply_from_addon = dbus_pending_call_steal_reply (pending_call);

	reply = dbus_message_copy (reply_from_addon);
	if (reply != NULL) {
		dbus_message_set_destination (reply, dbus_message_get_sender (method_from_caller));
		dbus_message_set_reply_serial (reply, dbus_message_get_serial (method_from_caller));

		if (dbus_connection != NULL)
			dbus_connection_send (dbus_connection, reply, NULL);

	}
	dbus_message_unref (reply_from_addon);
	dbus_message_unref (reply);
	dbus_message_unref (method_from_caller);
	dbus_pending_call_unref (pending_call);
}

static DBusHandlerResult
hald_dbus_filter_handle_methods (DBusConnection *connection, DBusMessage *message, 
				 void *user_data, dbus_bool_t local_interface)
{
	/*HAL_INFO (("connection=0x%x obj_path=%s interface=%s method=%s local_interface=%d", 
		   connection,
		   dbus_message_get_path (message), 
		   dbus_message_get_interface (message),
		   dbus_message_get_member (message),
		   local_interface));*/

	if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Manager",
					 "GetAllDevices") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_get_all_devices (connection, message);
	} else if (dbus_message_is_method_call (message,
                                                "org.freedesktop.Hal.Manager",
                                                "GetAllDevicesWithProperties") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_get_all_devices_with_properties (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"DeviceExists") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_device_exists (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"FindDeviceStringMatch") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_find_device_string_match (connection,
							 message);
	} else if (dbus_message_is_method_call
		   (message, "org.freedesktop.Hal.Manager",
		    "FindDeviceByCapability")
		   && strcmp (dbus_message_get_path (message),
			      "/org/freedesktop/Hal/Manager") == 0) {
		return manager_find_device_by_capability (connection,
							  message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"NewDevice") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return manager_new_device (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"Remove") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return manager_remove (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"CommitToGdl") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return manager_commit_to_gdl (connection, message, local_interface);

	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"AcquireGlobalInterfaceLock") &&
                   strcmp (dbus_message_get_path (message), "/org/freedesktop/Hal/Manager") == 0) {
		return device_acquire_global_interface_lock (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"ReleaseGlobalInterfaceLock") &&
                   strcmp (dbus_message_get_path (message), "/org/freedesktop/Hal/Manager") == 0) {
		return device_release_global_interface_lock (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"SingletonAddonIsReady") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return singleton_addon_is_ready (connection, message, local_interface);

	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"AcquireInterfaceLock")) {
		return device_acquire_interface_lock (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"ReleaseInterfaceLock")) {
		return device_release_interface_lock (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"IsCallerLockedOut")) {
		return device_is_caller_locked_out (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"IsCallerPrivileged")) {
		return device_is_caller_privileged (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"IsLockedByOthers")) {
		return device_is_locked_by_others (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
					      "org.freedesktop.Hal.Device",
					      "GetAllProperties")) {
		return device_get_all_properties (connection, message);
	} else if (dbus_message_is_method_call (message,
					      "org.freedesktop.Hal.Device",
					      "SetMultipleProperties")) {
		return device_set_multiple_properties (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetProperty")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyString")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyStringList")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyInteger")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyBoolean")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyDouble")) {
		return device_get_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetProperty")) {
		return device_set_property (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyString")) {
		return device_set_property (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyInteger")) {
		return device_set_property (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyBoolean")) {
		return device_set_property (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyDouble")) {
		return device_set_property (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"RemoveProperty")) {
		return device_remove_property (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"GetPropertyType")) {
		return device_get_property_type (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"PropertyExists")) {
		return device_property_exists (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"AddCapability")) {
		return device_add_capability (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"QueryCapability")) {
		return device_query_capability (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"Lock")) {
		return device_lock (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"Unlock")) {
		return device_unlock (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"StringListAppend")) {
		return device_string_list_append_prepend (connection, message, FALSE);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"StringListPrepend")) {
		return device_string_list_append_prepend (connection, message, TRUE);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"StringListRemove")) {
		return device_string_list_remove (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"Rescan")) {
		return device_rescan (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"Reprobe")) {
		return device_reprobe (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"EmitCondition")) {
		return device_emit_condition (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"ClaimInterface")) {
		return device_claim_interface (connection, message, local_interface);
#if 0
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"ReleaseInterface")) {
		return device_release_interface (connection, message, local_interface);
#endif
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"AddonIsReady")) {
		return addon_is_ready (connection, message, local_interface);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.DBus.Introspectable",
						"Introspect")) {
		return do_introspect (connection, message, local_interface);
	} else {
		const char *interface;
		const char *udi;
		const char *method;
		const char *signature;
		const char *caller;
		HalDevice *d;
                GSList *i;
                HalDeviceStrListIter if_iter;
                CICallerInfo *ci;

		/* check for device-specific interfaces that individual objects may support */

		udi = dbus_message_get_path (message);
		interface = dbus_message_get_interface (message);
		method = dbus_message_get_member (message);
		signature = dbus_message_get_signature (message);

                if (udi == NULL || method == NULL || signature == NULL || interface == NULL)
                        goto out;

		caller = dbus_message_get_sender (message);
		if (local_interface) {
			ci = NULL;
		} else {
			ci = ci_tracker_get_info (ci_tracker, caller);
			if (ci == NULL) {
				HAL_ERROR (("Cannot get caller info for %s", caller));
				goto out;
			}
		}

		d = NULL;
                d = hal_device_store_find (hald_get_gdl (), udi);
                if (d == NULL)
                        d = hal_device_store_find (hald_get_tdl (), udi);
                if (d == NULL)
                        goto out;

                /* no security checks; each method implementation is supposed to 
                 * check this (with e.g. PolicyKit) itself... 
                 */

                /* check if the interface on the device is locked by someone else 
                 *
                 * Note that method implementations can and will do their own
                 * checks too; e.g. operations on org.freedesktop.Hal.Device.Volume
                 * will check if the interface org.freedesktop.Hal.Device.Storage
                 * is locked on the enclosing drive.
                 */
                if (access_check_caller_locked_out (ci_tracker, d, caller, interface)) {
                        raise_interface_locked (connection, message, interface);
                        return DBUS_HANDLER_RESULT_HANDLED;
                }

                /* first see if an addon grabbed the interface */
                for (i = helper_interface_handlers; i != NULL; i = g_slist_next (i)) {
                        HelperInterfaceHandler *hih = i->data;
                        if (strcmp (hih->udi, udi) == 0 &&
                            strcmp (hih->interface_name, interface) == 0) {
                                DBusPendingCall *pending_call;
                                DBusMessage *copy;

                                /*HAL_INFO (("forwarding method to connection 0x%x", hih->connection));*/

                                dbus_message_ref (message);
                                
                                /* send a copy of the message */
                                copy = dbus_message_copy (message);
                                if (!dbus_connection_send_with_reply (hih->connection,
                                                                      copy,
                                                                      &pending_call,
                                                                      /*-1*/ 8000)) {
                                        /* TODO: handle error */
                                } else {
                                        /*HAL_INFO (("connection=%x message=%x", connection, message));*/
                                        dbus_pending_call_set_notify (pending_call,
                                                                      reply_from_fwd_message,
                                                                      (void *) message,
                                                                      NULL);
                                }
                                
                                dbus_message_unref (copy);
                                return DBUS_HANDLER_RESULT_HANDLED;
                        }
                }

                /* nope, see if we have a program to run.. */        
                for (hal_device_property_strlist_iter_init (d, "info.interfaces", &if_iter);
                     hal_device_property_strlist_iter_is_valid (&if_iter);
                     hal_device_property_strlist_iter_next (&if_iter)) {
                        const char *ifname = hal_device_property_strlist_iter_get_value (&if_iter);
                        
                        if (strcmp (ifname, interface) == 0) {
                                guint num;
                                HalDeviceStrListIter name_iter;
                                char *s;
                                
                                s = g_strdup_printf ("%s.method_names", interface);
                                hal_device_property_strlist_iter_init (d, s, &name_iter);
                                g_free (s);
                                for (num = 0; 
                                     hal_device_property_strlist_iter_is_valid (&name_iter);
                                     hal_device_property_strlist_iter_next (&name_iter), num++) {
                                        const char *methodname;
                                        methodname = hal_device_property_strlist_iter_get_value (&name_iter);
                                        if (strcmp (methodname, method) == 0) {
                                                const char *execpath;
                                                const char *sig;
                                                
                                                s = g_strdup_printf ("%s.method_execpaths", interface);
                                                execpath = hal_device_property_get_strlist_elem (d, s, num);
                                                g_free (s);
                                                s = g_strdup_printf ("%s.method_signatures", interface);
                                                sig = hal_device_property_get_strlist_elem (d, s, num);
                                                g_free (s);
                                                
                                                if (execpath != NULL && sig != NULL && 
                                                    strcmp (sig, signature) == 0) {
                                                        
                                                        HAL_INFO (("OK for method '%s' with signature '%s' on interface '%s' for UDI '%s' and execpath '%s'", method, signature, interface, udi, execpath));
                                                        
                                                        return hald_exec_method (d, ci, connection, 
                                                                                 local_interface,
                                                                                 message, execpath);
                                                }
                                                
                                        }
                                }
                        }
                }
	}
out:
	return osspec_filter_function (connection, message, user_data);
}

/**  
 *  hald_dbus_filter_function:
 *  @connection:          D-BUS connection
 *  @message:             Message
 *  @user_data:           User data
 *
 *  Returns:              What to do with the message
 *
 *  Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function. *ONLY* messages from the
 *  system bus goes through this method.
 */
DBusHandlerResult
hald_dbus_filter_function (DBusConnection * connection,
			   DBusMessage * message, void *user_data)
{
	if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected") &&
	    strcmp (dbus_message_get_path (message), DBUS_PATH_LOCAL) == 0) {

		/* this is a local message; e.g. from libdbus in this process */

		HAL_INFO (("Got disconnected from the system message bus; "
			   "retrying to reconnect every 3000 ms"));
		dbus_connection_unref (dbus_connection);
		dbus_connection = NULL;

#ifdef HAVE_GLIB_2_14
		g_timeout_add_seconds (3, reinit_dbus, NULL);
#else
		g_timeout_add (3000, reinit_dbus, NULL);
#endif
	} else if (dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameAcquired")){
		/* we don't need to do anything atm with this signal ... */
		return DBUS_HANDLER_RESULT_HANDLED; 
	} else if (dbus_message_is_signal (message,
					   DBUS_INTERFACE_DBUS,
					   "NameOwnerChanged")) {
		char *name;
		char *old_service_name;
		char *new_service_name;

		if (!dbus_message_get_args (message, NULL,
					    DBUS_TYPE_STRING, &name,
					    DBUS_TYPE_STRING, &old_service_name,
					    DBUS_TYPE_STRING, &new_service_name,
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid NameOwnerChanged signal from bus!"));
			goto out;
		}

		/*HAL_INFO (("NameOwnerChanged name=%s old=%s new=%s", name, old_service_name, new_service_name));*/

		ci_tracker_name_owner_changed (ci_tracker, name, old_service_name, new_service_name);

		if (services_with_locks != NULL)
			services_with_locks_remove_lockowner(old_service_name);

                if (strlen (old_service_name) > 0)
                        hal_device_client_disconnected (old_service_name);

#ifdef HAVE_CONKIT
	} else if (dbus_message_is_signal (message,
					   "org.freedesktop.ConsoleKit.Session",
					   "ActiveChanged")) {
		gboolean is_active;
		const char *session_objpath;

		if (!dbus_message_get_args (message, NULL,
					    DBUS_TYPE_BOOLEAN, &is_active,
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid ActiveChanged signal from ConsoleKit!"));
			goto out;
		}

		session_objpath = dbus_message_get_path (message);

		HAL_INFO (("active=%d for session %s", is_active, session_objpath));
		ci_tracker_active_changed (ci_tracker, session_objpath, is_active);
#endif /* HAVE_CONKIT */
	} else {

#ifdef HAVE_CONKIT
                /* TODO: only push the appropriate messages to the tracker; see ck-tracker.h */
                if (ck_tracker != NULL) {
                        ck_tracker_process_system_bus_message (ck_tracker, message);
                }
#endif
		return hald_dbus_filter_handle_methods (connection, message, user_data, FALSE);
        }

out:

#ifdef HAVE_CONKIT
	/* TODO: only push the appropriate messages to the tracker; see ck-tracker.h */
	if (ck_tracker != NULL) {
		ck_tracker_process_system_bus_message (ck_tracker, message);
	}
#endif

	return DBUS_HANDLER_RESULT_HANDLED;
}


static DBusHandlerResult 
local_server_message_handler (DBusConnection *connection, 
			      DBusMessage *message, 
			      void *user_data)
{
	/*HAL_INFO (("local_server_message_handler: destination=%s obj_path=%s interface=%s method=%s", 
		   dbus_message_get_destination (message) ? dbus_message_get_destination (message) : "",
		   dbus_message_get_path (message) ?  dbus_message_get_path (message) : "" , 
		   dbus_message_get_interface (message) ? dbus_message_get_interface (message) : "",
		   dbus_message_get_member (message) ? dbus_message_get_member (message) : ""));*/

	if (dbus_message_is_method_call (message, "org.freedesktop.DBus", "AddMatch")) {
		DBusMessage *reply;

		/* cheat, and handle AddMatch since libhal will try to invoke this method */
		reply = dbus_message_new_method_return (message);
		if (reply == NULL)
			DIE (("No memory"));
		if (!dbus_connection_send (connection, reply, NULL))
			DIE (("No memory"));
		dbus_message_unref (reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	} else if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected") &&
		   strcmp (dbus_message_get_path (message), DBUS_PATH_LOCAL) == 0) {
		GSList *i;
		GSList *j;
		
		//HAL_INFO (("************************"));
		//HAL_INFO (("Client to local_server was disconnected for %x", connection));
		//HAL_INFO (("************************"));

		for (i = helper_interface_handlers; i != NULL; i = j) {
			HelperInterfaceHandler *hih = i->data;

			j = g_slist_next (i);

			if (hih->connection == connection) {
				g_free (hih->interface_name);
				g_free (hih->introspection_xml);
				g_free (hih->udi);
				g_free (hih);
				helper_interface_handlers = g_slist_remove_link (helper_interface_handlers, i);
			}
		}

		if (singletons)
			g_hash_table_foreach_remove (singletons, (GHRFunc) singleton_remove_by_connection, connection);

		dbus_connection_unref (connection);
		return DBUS_HANDLER_RESULT_HANDLED;
	} else if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_SIGNAL) {
		DBusMessage *copy;

		/* it's a signal, just forward it onto the system message bus */
		copy = dbus_message_copy (message);
		if (dbus_connection != NULL) {
			dbus_connection_send (dbus_connection, copy, NULL);
		}
		dbus_message_unref (copy);
	} else {
		DBusHandlerResult ret;
		ret = hald_dbus_filter_handle_methods (connection, message, user_data, TRUE);
		return ret;
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void
local_server_unregister_handler (DBusConnection *connection, void *user_data)
{
	//HAL_INFO (("***************************"));
	//HAL_INFO (("********* unregistered %x", connection));
	//HAL_INFO (("***************************"));
}

static void
local_server_handle_connection (DBusServer *server,
			  DBusConnection *new_connection,
			  void *data)
{
	DBusObjectPathVTable vtable = { &local_server_unregister_handler, 
					&local_server_message_handler, 
					NULL, NULL, NULL, NULL};

	//HAL_INFO (("***************************"));
	//HAL_INFO (("********* got a connection %x", new_connection));
	//HAL_INFO (("***************************"));

	/*dbus_connection_add_filter (new_connection, server_filter_function, NULL, NULL);*/

	dbus_connection_register_fallback (new_connection, 
					   "/org/freedesktop",
					   &vtable,
					   NULL);
	dbus_connection_ref (new_connection);
	dbus_connection_setup_with_g_main (new_connection, NULL);
}


static DBusServer *local_server = NULL;
static char *local_server_address = NULL;

char *
hald_dbus_local_server_addr (void)
{
	return local_server_address;
}

gboolean
hald_dbus_local_server_init (void)
{
	gboolean ret;
	DBusError error;

	ret = FALSE;

	/* setup a server listening on a socket so we can do point to point
	 * connections for programs spawned by hald
	 */
	dbus_error_init (&error);
	if ((local_server = dbus_server_listen (HALD_DBUS_ADDRESS, &error)) == NULL) { 
		HAL_ERROR (("Cannot create D-BUS server:  %s: %s", error.name, error.message));
		dbus_error_free (&error);
		goto out;
	}
	local_server_address = dbus_server_get_address (local_server);
	HAL_INFO (("local server is listening at %s", local_server_address));
	dbus_server_setup_with_g_main (local_server, NULL);
	dbus_server_set_new_connection_function (local_server, local_server_handle_connection, NULL, NULL);	

	ret = TRUE;

out:
	return ret;
}

void
hald_dbus_local_server_shutdown (void)
{
	if (local_server != NULL) {
		HAL_INFO (("Shutting down local server"));
		dbus_server_disconnect (local_server);
		dbus_server_unref (local_server);
		local_server = NULL;
		dbus_free (local_server_address);
		local_server_address = NULL;
	}
}

#ifdef HAVE_CONKIT

static void 
hald_dbus_seat_added (CKTracker *tracker, CKSeat *seat, void *user_data)
{
	/* TODO: we could run callouts here... but they wouldn't do anything useful right now */
}

static void 
hald_dbus_seat_removed (CKTracker *tracker, CKSeat *seat, void *user_data)
{
	/* TODO: we could run callouts here... but they wouldn't do anything useful right now */
}

static gboolean
validate_lock_for_device (HalDeviceStore *store,
                          HalDevice      *device,
                          gpointer        user_data)
{
        int n, m;
        char **holders;
        char **locked_interfaces;

        locked_interfaces = hal_device_property_dup_strlist_as_strv (device, "info.named_locks");
        if (locked_interfaces == NULL)
                goto out;

        for (n = 0; locked_interfaces[n] != NULL; n++) {
                holders = hal_device_get_lock_holders (device, locked_interfaces[n]);
                if (holders == NULL)
                        continue;

                for (m = 0; holders[m] != NULL; m++) {

                        HAL_INFO (("Validating lock holder '%s' on interface '%s' on udi '%s'",
                                   holders[m], locked_interfaces[n], hal_device_get_udi (device)));

                        if (!access_check_caller_have_access_to_device (
                                    ci_tracker, device, "org.freedesktop.hal.lock", holders[m], NULL)) {
                                HAL_INFO (("Kicking out lock holder '%s' on interface '%s' on udi '%s' "
                                           "as he no longer has access to the device",
                                           holders[m], locked_interfaces[n], hal_device_get_udi (device)));
                                hal_device_release_lock (device, locked_interfaces[n], holders[m]);
                        }
                }
                g_strfreev (holders);
        }

        g_strfreev (locked_interfaces);
out:
        return TRUE;
}

static void
validate_locks (void)
{
        hal_device_store_foreach (hald_get_tdl (), validate_lock_for_device, NULL);
        hal_device_store_foreach (hald_get_gdl (), validate_lock_for_device, NULL);
}

static void
reconfigure_acl (void)
{
#ifdef HAVE_ACLMGMT
	HalDevice *d;
	char *extra_env[1] = {NULL};

	d = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	if (d == NULL) {
		d = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	}
	if (d == NULL) {
		HAL_ERROR (("No computer object?!?"));
		goto out;
	}

	hald_runner_run (d,
			 "hal-acl-tool --reconfigure", 
			 extra_env,
			 HAL_HELPER_TIMEOUT, 
			 NULL, /* run_terminated_cb */
			 NULL  /* userdata1 */, 
			 NULL  /* userdata2 */ );
out:
	;
#endif /* HAVE_ACLMGMT */
}

/* this function is normally called in response to when PolicyKit says the policy have changed */
void
reconfigure_all_policy (void)
{
        HAL_INFO (("Reconfiguring all policy"));

        /* first, validate all the locks */
        validate_locks ();

        /* second, reconfigure all ACL's */
        reconfigure_acl ();
}


typedef struct {
        HalDevice *d;
        char **programs;
        char **extra_env;
} SessionChangesEntry;

static GList *session_changes_queue = NULL;
gboolean session_changes_is_running = FALSE;

static void session_changes_process_queue (void);

static void
session_changes_done_cb (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
        SessionChangesEntry *entry = userdata1;

        HAL_INFO (("session_changes_done_cb"));

        g_assert (entry->d == d);

        /* hal_callout_device takes ownership of entry->programs so we don't free it here */
        g_object_unref (entry->d);
        g_strfreev (entry->extra_env);
        g_free (entry);

        session_changes_is_running = FALSE;

        session_changes_process_queue ();
}

static void
session_changes_process_queue (void)
{
        SessionChangesEntry *entry;

        HAL_INFO (("session_changes_process_queue"));

        /* nothing to run */
        if (session_changes_queue == NULL)
                return;

        /* something is already running; wait for it to terminate */
        if (session_changes_is_running)
                return;

        HAL_INFO (("  running element in queue"));

        /* pop the first element */
        entry = session_changes_queue->data;
        session_changes_queue = g_list_remove (session_changes_queue, entry);

	hal_callout_device (entry->d, 
			    session_changes_done_cb, /* callback */
			    entry,                          /* userdata1 */
			    NULL,                           /* userdata2 */
			    entry->programs, 
			    entry->extra_env);

        session_changes_is_running = TRUE;
}

static void
session_changes_push (HalDevice *d, char **programs, char **extra_env)
{
        SessionChangesEntry *entry;

        HAL_INFO (("session_changes_push"));

        entry = g_new0 (SessionChangesEntry, 1);
        entry->d = g_object_ref (d);
        entry->programs = g_strdupv (programs);
        entry->extra_env = g_strdupv (extra_env);

        /* push to end of queue */
        session_changes_queue = g_list_append (session_changes_queue, entry);

        /* only process the queue if there we are the only element */
        session_changes_process_queue ();
}

static void 
hald_dbus_session_active_changed (CKTracker *tracker, CKSession *session, void *user_data)
{
	HalDevice *d;
	char **programs;
	char *session_id;
	char *extra_env[5] = {"HALD_ACTION=session_active_changed", 
			      NULL /* "HALD_SESSION_ACTIVE_CHANGED_SESSION_ID=" */,
			      NULL /* "HALD_SESSION_ACTIVE_CHANGED_SESSION_UID=" */,
			      NULL /* "HALD_SESSION_ACTIVE_CHANGED_SESSION_IS_ACTIVE=" */,
			      NULL};

	session_id = ck_session_get_id (session);

	HAL_INFO (("In hald_dbus_session_active_changed for session '%s': %s", 
		   session_id, ck_session_is_active (session) ? "ACTIVE" : "INACTIVE"));

        /* revalidate all locks (to remove locks from callers in that session who no longer has access to devices */
        validate_locks ();

	d = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	if (d == NULL) {
		d = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	}
	if (d == NULL) {
		goto out;
	}

	programs = hal_device_property_dup_strlist_as_strv (d, 
							    ck_session_is_active (session) ?
							    "info.callouts.session_active" :
							    "info.callouts.session_inactive");
	if (programs == NULL) {
		goto out;
	}

	extra_env[1] = g_strdup_printf ("HALD_SESSION_ACTIVE_CHANGED_SESSION_ID=%s", session_id);
	extra_env[2] = g_strdup_printf ("HALD_SESSION_ACTIVE_CHANGED_SESSION_UID=%u", ck_session_get_user (session));
	extra_env[3] = g_strdup_printf ("HALD_SESSION_ACTIVE_CHANGED_SESSION_IS_ACTIVE=%s", 
					ck_session_is_active (session) ? "true" : "false");

        session_changes_push (d, programs, extra_env);

	g_free (extra_env[1]);
	g_free (extra_env[2]);
	g_free (extra_env[3]);
out:
	g_free (session_id);
}

static void 
hald_dbus_session_added (CKTracker *tracker, CKSession *session, void *user_data)
{
	HalDevice *d;
	char **programs;
	char *session_id;
	char *seat_id;
	char *extra_env[5] = {"HALD_ACTION=session_add", 
			      NULL /* "HALD_SESSION_ADD_SESSION_ID=" */,
			      NULL /* "HALD_SESSION_ADD_SESSION_UID=" */,
			      NULL /* "HALD_SESSION_ADD_SESSION_IS_ACTIVE=" */,
			      NULL};
	session_id = ck_session_get_id (session);
	seat_id = ck_seat_get_id (ck_session_get_seat (session));
	
	HAL_INFO (("In hald_dbus_session_added for session '%s' on seat '%s'", 
		   session_id, ck_session_get_seat (session) != NULL ? seat_id : "(NONE)"));

	d = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	if (d == NULL) {
		d = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	}
	if (d == NULL) {
		goto out;
	}

	programs = hal_device_property_dup_strlist_as_strv (d, "info.callouts.session_add");
	if (programs == NULL) {
		goto out;
	}

	extra_env[1] = g_strdup_printf ("HALD_SESSION_ADD_SESSION_ID=%s", session_id );
	extra_env[2] = g_strdup_printf ("HALD_SESSION_ADD_SESSION_UID=%u", ck_session_get_user (session));
	extra_env[3] = g_strdup_printf ("HALD_SESSION_ADD_SESSION_IS_ACTIVE=%s", 
					ck_session_is_active (session) ? "true" : "false");

        session_changes_push (d, programs, extra_env);

	g_free (extra_env[1]);
	g_free (extra_env[2]);
	g_free (extra_env[3]);
out:
	g_free (session_id);
	g_free (seat_id);
}

static void 
hald_dbus_session_removed (CKTracker *tracker, CKSession *session, void *user_data)
{
	HalDevice *d;
	char *session_id;
	char *seat_id;
	char **programs;
	char *extra_env[5] = {"HALD_ACTION=session_remove", 
			      NULL /* "HALD_SESSION_REMOVE_SESSION_ID=" */,
			      NULL /* "HALD_SESSION_REMOVE_SESSION_UID=" */,
			      NULL /* "HALD_SESSION_REMOVE_SESSION_IS_ACTIVE=" */,
			      NULL};

	session_id = ck_session_get_id (session);
	seat_id = ck_seat_get_id (ck_session_get_seat (session));

	HAL_INFO (("In hald_dbus_session_removed for session '%s' on seat '%s'", 
		   session_id, ck_session_get_seat (session) != NULL ? seat_id : "(NONE)"));

	d = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	if (d == NULL) {
		d = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	}
	if (d == NULL) {
		goto out;
	}

	programs = hal_device_property_dup_strlist_as_strv (d, "info.callouts.session_remove");
	if (programs == NULL) {
		goto out;
	}

	extra_env[1] = g_strdup_printf ("HALD_SESSION_REMOVE_SESSION_ID=%s", session_id );
	extra_env[2] = g_strdup_printf ("HALD_SESSION_REMOVE_SESSION_UID=%d", ck_session_get_user (session));
	extra_env[3] = g_strdup_printf ("HALD_SESSION_REMOVE_SESSION_IS_ACTIVE=%s", 
					ck_session_is_active (session) ? "true" : "false");

        session_changes_push (d, programs, extra_env);

	g_free (extra_env[1]);
	g_free (extra_env[2]);
	g_free (extra_env[3]);
out:
	g_free (session_id);
	g_free (seat_id);
}

static void
hald_dbus_ck_disappeared (CKTracker *tracker, void *user_data)
{
	HAL_INFO (("In hald_dbus_ck_disappeared"));
        reconfigure_acl ();
}

static void
hald_dbus_ck_appeared (CKTracker *tracker, void *user_data)
{
	HAL_INFO (("In hald_dbus_ck_appeared"));
        reconfigure_acl ();
}

#endif /* HAVE_CONKIT */

gboolean
hald_dbus_init_preprobe (void)
{
	gboolean ret;
	DBusError dbus_error;

	ret = FALSE;

	dbus_connection_set_change_sigpipe (TRUE);

	dbus_error_init (&dbus_error);
	dbus_connection = dbus_bus_get (DBUS_BUS_SYSTEM, &dbus_error);
	if (dbus_connection == NULL) {
		HAL_ERROR (("dbus_bus_get(): %s", dbus_error.message));
		dbus_error_free (&dbus_error);
		goto out;
	}

	dbus_connection_setup_with_g_main (dbus_connection, NULL);
	dbus_connection_set_exit_on_disconnect (dbus_connection, FALSE);

	dbus_connection_add_filter (dbus_connection, hald_dbus_filter_function, NULL, NULL);


	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='"DBUS_INTERFACE_DBUS"'"
			    ",sender='"DBUS_SERVICE_DBUS"'"
			    ",member='NameOwnerChanged'",
			    NULL);

#ifdef HAVE_CONKIT
	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Session'"
			    ",sender='org.freedesktop.ConsoleKit'"
			    ",member='ActiveChanged'",
			    NULL);
	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Seat'"
			    ",sender='org.freedesktop.ConsoleKit'"
			    ",member='SessionAdded'",
			    NULL);
	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Seat'"
			    ",sender='org.freedesktop.ConsoleKit'"
			    ",member='SessionRemoved'",
			    NULL);
	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Manager'"
			    ",sender='org.freedesktop.ConsoleKit'"
			    ",member='SeatAdded'",
			    NULL);
	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Manager'"
			    ",sender='org.freedesktop.ConsoleKit'"
			    ",member='SeatRemoved'",
			    NULL);

	ck_tracker = ck_tracker_new ();
	ck_tracker_set_system_bus_connection (ck_tracker, dbus_connection);
	ck_tracker_set_seat_added_cb (ck_tracker, hald_dbus_seat_added);
	ck_tracker_set_seat_removed_cb (ck_tracker, hald_dbus_seat_removed);
	ck_tracker_set_session_added_cb (ck_tracker, hald_dbus_session_added);
	ck_tracker_set_session_removed_cb (ck_tracker, hald_dbus_session_removed);
	ck_tracker_set_session_active_changed_cb (ck_tracker, hald_dbus_session_active_changed);
	ck_tracker_set_service_disappeared_cb (ck_tracker, hald_dbus_ck_disappeared);
	ck_tracker_set_service_appeared_cb (ck_tracker, hald_dbus_ck_appeared);
	if (!ck_tracker_init (ck_tracker)) {
		HAL_WARNING (("Could not initialize seats and sessions from ConsoleKit"));
	}

#endif /* HAVE_CONKIT */

	ci_tracker = ci_tracker_new ();
	ci_tracker_set_system_bus_connection (ci_tracker, dbus_connection);
	ci_tracker_init (ci_tracker);

	ret = TRUE;

out:
	return ret;
}

#ifdef HAVE_CONKIT
CKTracker *
hald_dbus_get_ck_tracker (void)
{
	return ck_tracker;
}
#endif

gboolean
hald_dbus_init (void)
{
	DBusError dbus_error;

	HAL_INFO (("entering"));

	if (dbus_connection == NULL) {
		HAL_ERROR(("not connected to system bus"));
		return FALSE;
	}

	dbus_error_init (&dbus_error);
	dbus_bus_request_name (dbus_connection, "org.freedesktop.Hal", 0, &dbus_error);
	if (dbus_error_is_set (&dbus_error)) {
		HAL_ERROR (("dbus_bus_request_name(): %s", dbus_error.message));
		dbus_error_free(&dbus_error);
		return FALSE;
	}

	return TRUE;
}

