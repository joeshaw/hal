/***************************************************************************
 * CVSID: $Id$
 *
 * dbus.c : D-BUS interface of HAL daemon
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

static DBusConnection *dbus_connection = NULL;

static void
raise_error (DBusConnection *connection,
	     DBusMessage *in_reply_to,
	     const char *error_name,
	     char *format, ...) __attribute__((format (printf, 4, 5)));

/**
 * @defgroup DaemonErrors Error conditions
 * @ingroup HalDaemon
 * @brief Various error messages the HAL daemon can raise
 * @{
 */

/** Raise HAL error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  error_name          D-Bus error name
 *  @param  format              printf-style format for error message
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

	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to, error_name, buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.NoSuchDevice error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  udi                 Unique device id given
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

/** Raise the org.freedesktop.Hal.NoSuchProperty error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property that didn't exist
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

/** Raise the org.freedesktop.Hal.TypeMismatch error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property
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

/** Raise the org.freedesktop.Hal.SyntaxError error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  method_name         Name of the method that was invoked with
 *                              the wrong signature
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

/** Raise the org.freedesktop.Hal.DeviceNotLocked error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device              device which isn't locked
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

/** Raise the org.freedesktop.Hal.DeviceAlreadyLocked error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device              device which isn't locked
 */
static void
raise_device_already_locked (DBusConnection *connection,
			     DBusMessage    *in_reply_to,
			     HalDevice      *device)
{
	DBusMessage *reply;
	const char *reason;

	reason = hal_device_property_get_string (device, "info.locked.reason");
	HAL_WARNING (("Device %s is already locked: %s",
		      hal_device_get_udi (device), reason));


	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.DeviceAlreadyLocked",
					reason);

	if (reply == NULL || !dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.PermissionDenied error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  message             what you're not allowed to do
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

/** @} */

/**
 * @defgroup ManagerInterface D-BUS interface org.freedesktop.Hal.Manager
 * @ingroup HalDaemon
 * @brief D-BUS interface for querying device objects
 *
 * @{
 */

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
	if (strncmp (device->udi, "/org/freedesktop/Hal/devices/temp",
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

/** Find devices in the GDL where a single string property matches a given
 *  value. Also returns devices in the TDL that has a non-tmp UDI.
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
		raise_syntax (connection, message,
			      "Manager.FindDeviceStringMatch");
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
		dbus_message_iter_append_basic (info->iter,
						DBUS_TYPE_STRING,
						&(device->udi));
	}

	return TRUE;
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
		raise_syntax (connection, message,
			      "Manager.FindDeviceByCapability");
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

/** Send signal DeviceAdded(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 *  @param  device              The HalDevice added
 */
void
manager_send_signal_device_added (HalDevice *device)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL)
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

/** Send signal DeviceRemoved(string udi) on the org.freedesktop.Hal.Manager
 *  interface on the object /org/freedesktop/Hal/Manager.
 *
 *  @param  device              The HalDevice removed
 */
void
manager_send_signal_device_removed (HalDevice *device)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL)
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

/** Send signal NewCapability(string udi, string capability) on the 
 *  org.freedesktop.Hal.Manager interface on the object 
 *  /org/freedesktop/Hal/Manager.
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability
 */
void
manager_send_signal_new_capability (HalDevice *device,
				    const char *capability)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL)
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

/** @} */

/**
 * @defgroup DeviceInterface D-BUS interface org.freedesktop.Hal.Device
 * @ingroup HalDaemon
 * @brief D-BUS interface for generic device operations
 * @{
 */

static gboolean
foreach_property_append (HalDevice *device, HalProperty *p,
			 gpointer user_data)
{
	DBusMessageIter *iter;
	DBusMessageIter iter_dict_entry;
	const char *key;
	int type;

	iter = (DBusMessageIter *)user_data;

	dbus_message_iter_open_container (iter,
					  DBUS_TYPE_DICT_ENTRY,
					  NULL,
					  &iter_dict_entry);

	key = hal_property_get_key (p);
	type = hal_property_get_type (p);

	dbus_message_iter_append_basic (&iter_dict_entry, DBUS_TYPE_STRING, &key);

	switch (type) {
	case HAL_PROPERTY_TYPE_STRING:
	{
		DBusMessageIter iter_var;
		const char *v;

		v = hal_property_get_string (p);

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

		v = hal_property_get_int (p);

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

		v = hal_property_get_uint64 (p);

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

		v = hal_property_get_double (p);

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

		v = hal_property_get_bool (p);

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
		GSList *iter;

		dbus_message_iter_open_container (&iter_dict_entry,
						  DBUS_TYPE_VARIANT,
						  DBUS_TYPE_ARRAY_AS_STRING
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_var);

		dbus_message_iter_open_container (&iter_var,
						  DBUS_TYPE_ARRAY,
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_array);

		for (iter = hal_property_get_strlist (p); iter != NULL; iter = iter->next) {
				     
			const char *v;
			v = (const char *) iter->data;

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


	return TRUE;
}
		
	
	
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
	HalProperty *p;

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
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	p = hal_device_property_find (d, key);
	if (p == NULL) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init_append (reply, &iter);

	type = hal_property_get_type (p);
	switch (type) {
	case HAL_PROPERTY_TYPE_STRING:
	{
		const char *s;
		s = hal_property_get_string (p);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &s);
		break;
	}
	case HAL_PROPERTY_TYPE_INT32:
	{
		dbus_int32_t i;
		i = hal_property_get_int (p);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &i);
		break;
	}
	case HAL_PROPERTY_TYPE_UINT64:
	{
		dbus_uint64_t ul;
		ul = hal_property_get_uint64 (p);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT64, &ul);
		break;
	}
	case HAL_PROPERTY_TYPE_DOUBLE:
	{
		double d;
		d = hal_property_get_double (p);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_DOUBLE, &d);
		break;
	}
	case HAL_PROPERTY_TYPE_BOOLEAN:
	{
		dbus_bool_t b;
		b = hal_property_get_bool (p);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &b);
		break;
	}
	case HAL_PROPERTY_TYPE_STRLIST:
	{
		GSList *l;
		DBusMessageIter iter_array;

		dbus_message_iter_open_container (&iter, 
						  DBUS_TYPE_ARRAY, 
						  DBUS_TYPE_STRING_AS_STRING,
						  &iter_array);

		for (l = hal_property_get_strlist (p); l != NULL; l = g_slist_next (l)) {
			dbus_message_iter_append_basic (&iter_array, DBUS_TYPE_STRING, &(l->data));
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
	HalProperty *p;
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
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	p = hal_device_property_find (d, key);
	if (p == NULL) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	i = hal_property_get_type (p);
	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &i);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}


static dbus_bool_t 
sender_has_privileges (DBusConnection *connection, DBusMessage *message)
{
	DBusError error;
	unsigned long user_uid;
	const char *user_base_svc;
	dbus_bool_t ret;

	ret = FALSE;

	user_base_svc = dbus_message_get_sender (message);
	if (user_base_svc == NULL) {
		HAL_WARNING (("Cannot determine base service of caller"));
		goto out;
	}

	HAL_DEBUG (("base_svc = %s", user_base_svc));

	dbus_error_init (&error);
	user_uid = dbus_bus_get_unix_user (connection, user_base_svc, &error);
	if (user_uid == (unsigned long) -1 || dbus_error_is_set (&error)) {
		HAL_WARNING (("Could not get uid for connection: %s %s", error.name, error.message));
		dbus_error_free (&error);
		goto out;
	}

	HAL_INFO (("uid for caller is %ld", user_uid));

	if (user_uid != 0 && user_uid != geteuid()) {
		HAL_WARNING (("uid %d is not privileged", user_uid));
		goto out;
	}

	ret = TRUE;

out:
	return ret;
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
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
 *    raises org.freedesktop.Hal.PermissionDenied, 
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
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
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (do_prepend)
		ret = hal_device_property_strlist_prepend (d, key, value);
	else
		ret = hal_device_property_strlist_append (d, key, value);
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



/** Remove a property on a device.
 *
 *  <pre>
 *  void Device.RemoveProperty(string key)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.NoSuchProperty
 *           org.freedesktop.Hal.PermissionDenied
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
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


/** Determine if a device has a capability
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
DBusHandlerResult
device_query_capability (DBusConnection * connection,
			 DBusMessage * message)
{
	dbus_bool_t rc;
	const char *udi;
	const char *caps;
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
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	rc = FALSE;
	caps = hal_device_property_get_string (d, "info.capabilities");
	if (caps != NULL) {
		char **capsv, **iter;

		capsv = g_strsplit (caps, " ", 0);
		for (iter = capsv; *iter != NULL; iter++) {
			if (strcmp (*iter, capability) == 0) {
				rc = TRUE;
				break;
			}
		}

		g_strfreev (capsv);
	}

	dbus_free (capability);

	dbus_message_iter_init_append (reply, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &rc);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static GHashTable *services_with_locks = NULL;

/** Grab an advisory lock on a device.
 *
 *  <pre>
 *  bool Device.Lock(string reason)
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.DeviceAlreadyLocked
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
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

	g_hash_table_insert (services_with_locks, g_strdup (sender),
			     g_object_ref (d));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** Release an advisory lock on a device.
 *
 *  <pre>
 *  bool Device.Unlock()
 *
 *    raises org.freedesktop.Hal.NoSuchDevice, 
 *           org.freedesktop.Hal.DeviceNotLocked,
 *           org.freedesktop.Hal.PermissionDenied
 *  </pre>
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
DBusHandlerResult
device_unlock (DBusConnection * connection,
	       DBusMessage * message)
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

	if (g_hash_table_lookup (services_with_locks, sender))
		g_hash_table_remove (services_with_locks, sender);
	else {
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

/** Begin an atomic update - this is useful for updating several properties
 *  in one go.
 *
 *  Note that an atomic update is recursive - use with caution!
 */
void
device_property_atomic_update_begin (void)
{
	atomic_count++;
}

/** End an atomic update.
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
              device->udi, key, 
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

		if (dbus_connection == NULL)
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

/** Emits a condition on a device; the device has to be in the GDL for
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
 *
 * @param  udi                  The UDI for this device
 * @param  condition_name       Name of condition
 * @param  first_arg_type       Type of the first argument
 * @param  ...                  value of first argument, list of additional
 *                              type-value pairs. Must be terminated with
 *                              DBUS_TYPE_INVALID
 */
void
device_send_signal_condition (HalDevice *device, const char *condition_name, const char *condition_details)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;

	if (dbus_connection == NULL)
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
	if (hald_dbus_init ())
		return FALSE;
	else
		return TRUE;
}

static void
service_deleted (DBusMessage *message)
{
	char *old_service_name;
	char *new_service_name;
	HalDevice *d;

	if (!dbus_message_get_args (message, NULL,
				    DBUS_TYPE_STRING, &old_service_name,
				    DBUS_TYPE_STRING, &new_service_name,
				    DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Invalid NameOwnerChanged signal from bus!"));
		return;
	}

	d = g_hash_table_lookup (services_with_locks, new_service_name);

	if (d != NULL) {
		hal_device_property_remove (d, "info.locked");
		hal_device_property_remove (d, "info.locked.reason");
		hal_device_property_remove (d, "info.locked.dbus_name");

		g_hash_table_remove (services_with_locks, new_service_name);
	}
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "Reprobe: not privileged");
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "EmitCondition: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	HAL_DEBUG (("udi=%s", udi));

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &condition_name,
				    DBUS_TYPE_STRING, &condition_details,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "EmitCondition");
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


/*
 * Create new device in tdl. Return temporary udi.
 */
DBusHandlerResult
manager_new_device (DBusConnection * connection, DBusMessage * message, dbus_bool_t local_interface)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	gchar *udi;
	int i;
	struct timeval tv;

	dbus_error_init (&error);

	if (!local_interface && !sender_has_privileges (connection, message)) {
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
	hal_device_property_set_string (d, "info.udi", udi);
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
	HAL_INFO (("Remove callouts completed udi=%s", d->udi));

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

	if (!local_interface && !sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "Remove: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "CommitToGdl");
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
	HAL_INFO (("Add callouts completed udi=%s", d->udi));
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

	/* Merge properties from .fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

	hal_util_callout_device_add (d, manager_commit_done, NULL, NULL);
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

	if (!local_interface && !sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "CommitToGdl: not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &tmp_udi,
				    DBUS_TYPE_STRING, &udi0,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "CommitToGdl");
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
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof udi, "%s", udi0);

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
	hal_device_property_set_string (d, "info.udi", udi);

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

static void
hald_exec_method_cb (HalDevice *d, gboolean timed_out, gint return_code, 
		     gpointer data1, gpointer data2, HalHelperData *helper_data)
{
	dbus_uint32_t result;
	DBusMessage *reply;
	DBusMessage *message;
	DBusMessageIter iter;
	int *stderr_fd;
	char buf[512]; /* TODO: don't hardcode error message size */
	char *exp_name;
	char *exp_detail;

	message = (DBusMessage *) data1;
	stderr_fd = (int *) data2;

	exp_name = NULL;
	exp_detail = NULL;

	/* read back possible error conditions from stderr */
	if (stderr_fd != NULL) {
		ssize_t num_read;

		num_read = read (*stderr_fd, buf, sizeof (buf) - 2);
		buf[sizeof (buf) - 2] = '\0';
		buf[sizeof (buf) - 1] = '\0';
		if (num_read > 0) {
			char *p;
			char *s;
			p = buf;
			for (s = p; *s != '\n' && *s != '\0'; s++)
				;
			if (*s != '\0') {
				exp_name = g_strndup (p, s - p);
				p = s + 1;
				for (s = p; *s != '\n' && *s != '\0'; s++)
					;
				if (*s != '\0') {
					exp_detail = g_strndup (p, s - p);
				}
			}
		}
	}

	if (exp_name != NULL && exp_detail != NULL) {
		HAL_INFO (("failed with '%s' '%s'", exp_name, exp_detail));

		/* throw exception */

		reply = dbus_message_new_error (message, exp_name, exp_detail);
		if (reply == NULL) {
			/* error name may be invalid - assume caller fucked up and use a generic HAL error name */
			reply = dbus_message_new_error (message, "org.freedesktop.Hal.Device.UnknownError", "An unknown error occured");
			if (reply == NULL) {
				DIE (("No memory"));
			}
		}
		if (dbus_connection != NULL) {
			if (!dbus_connection_send (dbus_connection, reply, NULL))
				DIE (("No memory"));
		}
		dbus_message_unref (reply);

	} else {
		result = (dbus_uint32_t) return_code;

		reply = dbus_message_new_method_return (message);
		if (reply == NULL)
			DIE (("No memory"));
		
		dbus_message_iter_init_append (reply, &iter);
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &result);
		
		if (dbus_connection != NULL) {
			if (!dbus_connection_send (dbus_connection, reply, NULL))
				DIE (("No memory"));
		}

		dbus_message_unref (reply);
	}

	dbus_message_unref (message);
	g_free (stderr_fd);
	g_free (exp_name);
	g_free (exp_detail);
}

static DBusHandlerResult
hald_exec_method (HalDevice *d, DBusConnection *connection, DBusMessage *message, const char *execpath)
{
	int type;
	char *stdin;
	GString *stdin_str;
	DBusMessageIter iter;
	int stdin_fd;
	int *stderr_fd;

	/* TODO: check that sender is e.g. at console */
/*
	if (!sender_has_privileges (connection, message)) {
		raise_permission_denied (connection, message, "not privileged");
		return DBUS_HANDLER_RESULT_HANDLED;
	}
*/

	stdin_str = g_string_sized_new (256); /* reasonable default size for passing params; can grow */

	/* prepare stdin with parameters */
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
			g_string_append_printf (stdin_str, "%lld", value);
			break;
		}
		case DBUS_TYPE_UINT64:
		{
			dbus_uint64_t value;
			dbus_message_iter_get_basic (&iter, &value);
			g_string_append_printf (stdin_str, "%llu", value);
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
				return FALSE;

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

	stdin = g_string_free (stdin_str, FALSE);

	stderr_fd = (int *) g_new0 (int, 1);

	/* no timeout */
	if (hal_util_helper_invoke_with_pipes (execpath, NULL, d, 
					       (gpointer) message, (gpointer) stderr_fd, 
					       hald_exec_method_cb, 0, &stdin_fd, NULL, stderr_fd) != NULL) {
		write (stdin_fd, stdin, strlen (stdin));
		close (stdin_fd);
	}

	dbus_message_ref (message);

	g_free (stdin);

	return DBUS_HANDLER_RESULT_HANDLED;

error:
	g_string_free (stdin_str, TRUE);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
hald_dbus_filter_handle_methods (DBusConnection *connection, DBusMessage *message, 
				 void *user_data, dbus_bool_t local_interface)
{
	if (dbus_message_is_method_call (message,
					 "org.freedesktop.Hal.Manager",
					 "GetAllDevices") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_get_all_devices (connection, message);
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
	}
	else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"NewDevice") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return manager_new_device (connection, message, local_interface);
	}
	else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"Remove") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return manager_remove (connection, message, local_interface);
	}
	else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Manager",
						"CommitToGdl") &&
		   strcmp (dbus_message_get_path (message),
			    "/org/freedesktop/Hal/Manager") == 0) {
		return manager_commit_to_gdl (connection, message, local_interface);
	}
	else if (dbus_message_is_method_call (message,
					      "org.freedesktop.Hal.Device",
					      "GetAllProperties")) {
		return device_get_all_properties (connection, message);
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
	} else {
		const char *interface;
		const char *udi;
		const char *method;
		const char *signature;
		HalDevice *d;

		/* check for device-specific interfaces that individual objects may support */

		udi = dbus_message_get_path (message);
		interface = dbus_message_get_interface (message);
		method = dbus_message_get_member (message);
		signature = dbus_message_get_signature (message);

		d = NULL;

		if (method != NULL) {
			d = hal_device_store_find (hald_get_gdl (), udi);
			if (d == NULL)
				d = hal_device_store_find (hald_get_tdl (), udi);
		}

		if (d != NULL && interface != NULL && method != NULL && signature != NULL) {
			GSList *interfaces;
			GSList *i;

			interfaces = hal_device_property_get_strlist (d, "info.interfaces");
			for (i = interfaces; i != NULL; i = g_slist_next (i)) {
				const char *ifname = (const char *) i->data;

				if (strcmp (ifname, interface) == 0) {
					guint num;
					GSList *method_names;
					char *s;

					s = g_strdup_printf ("%s.method_names", interface);
					method_names = hal_device_property_get_strlist (d, s);
					g_free (s);
					for (i = method_names, num = 0; i != NULL; i = g_slist_next (i), num++) {
						const char *methodname = (const char *) i->data;
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

								return hald_exec_method (d, connection, message, execpath);
							}
							
						}
					}
				}
			}
			
		}
	}
		
	return osspec_filter_function (connection, message, user_data);
}
       

/** Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @param  user_data           User data
 *  @return                     What to do with the message
 */
DBusHandlerResult
hald_dbus_filter_function (DBusConnection * connection,
			   DBusMessage * message, void *user_data)
{
	/*HAL_INFO (("obj_path=%s interface=%s method=%s", 
		   dbus_message_get_path(message), 
		   dbus_message_get_interface(message),
		   dbus_message_get_member(message)));*/

	if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected") &&
	    strcmp (dbus_message_get_path (message), DBUS_PATH_LOCAL) == 0) {

		/* this is a local message; e.g. from libdbus in this process */

		HAL_INFO (("Got disconnected from the system message bus; "
			   "retrying to reconnect every 3000 ms"));
		dbus_connection_unref (dbus_connection);
		dbus_connection = NULL;

		g_timeout_add (3000, reinit_dbus, NULL);

	} else if (dbus_message_is_signal (message,
					   DBUS_INTERFACE_DBUS,
					   "NameOwnerChanged")) {

		if (services_with_locks != NULL)
			service_deleted (message);
	} else 
		return hald_dbus_filter_handle_methods (connection, message, user_data, FALSE);

	return DBUS_HANDLER_RESULT_HANDLED;
}



static DBusHandlerResult 
local_server_message_handler (DBusConnection *connection, 
			      DBusMessage *message, 
			      void *user_data)
{
	HAL_INFO (("local_server_message_handler: destination=%s obj_path=%s interface=%s method=%s", 
		   dbus_message_get_destination (message), 
		   dbus_message_get_path (message), 
		   dbus_message_get_interface (message),
		   dbus_message_get_member (message)));

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
		
		HAL_INFO (("Client to local_server was disconnected"));
		dbus_connection_unref (connection);
		return DBUS_HANDLER_RESULT_HANDLED;
	} 
	else return hald_dbus_filter_handle_methods (connection, message, user_data, TRUE);
}

static void
local_server_unregister_handler (DBusConnection *connection, void *user_data)
{
	HAL_INFO (("unregistered"));
}

static void
local_server_handle_connection (DBusServer *server,
			  DBusConnection *new_connection,
			  void *data)
{
	DBusObjectPathVTable vtable = { &local_server_unregister_handler, 
					&local_server_message_handler, 
					NULL, NULL, NULL, NULL};

	HAL_INFO (("%d: Got a connection", getpid ()));
	HAL_INFO (("dbus_connection_get_is_connected = %d", dbus_connection_get_is_connected (new_connection)));

	/*dbus_connection_add_filter (new_connection, server_filter_function, NULL, NULL);*/

	dbus_connection_register_fallback (new_connection, 
					   "/org/freedesktop",
					   &vtable,
					   NULL);
	dbus_connection_ref (new_connection);
	dbus_connection_setup_with_g_main (new_connection, NULL);
}


static DBusServer *local_server = NULL;

char *
hald_dbus_local_server_addr (void)
{
	if (local_server == NULL)
		return NULL;

	return dbus_server_get_address (local_server);
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
	if ((local_server = dbus_server_listen ("unix:tmpdir=/tmp/hald-local", &error)) == NULL) { 
		HAL_ERROR (("Cannot create D-BUS server"));
		goto out;
	}
	HAL_INFO (("local server is listening at %s", dbus_server_get_address (local_server)));
	dbus_server_setup_with_g_main (local_server, NULL);
	dbus_server_set_new_connection_function (local_server, local_server_handle_connection, NULL, NULL);	

	ret = TRUE;

out:
	return ret;
}

gboolean
hald_dbus_init (void)
{
	DBusError dbus_error;

	HAL_INFO (("entering"));

	dbus_connection_set_change_sigpipe (TRUE);

	dbus_error_init (&dbus_error);
	dbus_connection = dbus_bus_get (DBUS_BUS_SYSTEM, &dbus_error);
	if (dbus_connection == NULL) {
		HAL_ERROR (("dbus_bus_get(): %s", dbus_error.message));
		return FALSE;
	}

	dbus_connection_setup_with_g_main (dbus_connection, NULL);
	dbus_connection_set_exit_on_disconnect (dbus_connection, FALSE);

	dbus_bus_request_name (dbus_connection, "org.freedesktop.Hal",
				  0, &dbus_error);
	if (dbus_error_is_set (&dbus_error)) {
		HAL_ERROR (("dbus_bus_request_name(): %s",
			    dbus_error.message));
		return FALSE;
	}

	dbus_connection_add_filter (dbus_connection, hald_dbus_filter_function, NULL, NULL);

	dbus_bus_add_match (dbus_connection,
			    "type='signal'"
			    ",interface='"DBUS_INTERFACE_DBUS"'"
			    ",sender='"DBUS_SERVICE_DBUS"'"
			    ",member='NameOwnerChanged'",
			    NULL);

	return TRUE;
}

/** @} */
