/***************************************************************************
 * CVSID: $Id$
 *
 * dbus.c : D-BUS interface of HAL daemon
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "hald.h"
#include "hald_dbus.h"
#include "device.h"
#include "device_store.h"
#include "logger.h"
#include "osspec.h"

static DBusConnection *dbus_connection;

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
static void
raise_no_such_device (DBusConnection * connection,
		      DBusMessage * in_reply_to, const char *udi)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "No device with id %s", udi);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.NoSuchDevice",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.UdiInUse error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  udi                 Unique device id that is already in use
 */
static void
raise_udi_in_use (DBusConnection * connection,
		  DBusMessage * in_reply_to, const char *udi)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "Unique device id '%s' is already in use",
		  udi);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.UdiInUse",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.NoSuchProperty error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property that didn't exist
 */
static void
raise_no_such_property (DBusConnection * connection,
			DBusMessage * in_reply_to,
			const char *device_id, const char *key)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511, "No property %s on device with id %s", key,
		  device_id);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.NoSuchProperty",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.TypeMismatch error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  device_id           Id of the device
 *  @param  key                 Key of the property
 */
static void
raise_property_type_error (DBusConnection * connection,
			   DBusMessage * in_reply_to,
			   const char *device_id, const char *key)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511,
		  "Type mismatch setting property %s on device with id %s",
		  key, device_id);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.TypeMismatch",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
}

/** Raise the org.freedesktop.Hal.SyntaxError error
 *
 *  @param  connection          D-Bus connection
 *  @param  in_reply_to         message to report error on
 *  @param  method_name         Name of the method that was invoked with
 *                              the wrong signature
 */
static void
raise_syntax (DBusConnection * connection,
	      DBusMessage * in_reply_to, const char *method_name)
{
	char buf[512];
	DBusMessage *reply;

	snprintf (buf, 511,
		  "There is a syntax error in the invocation of "
		  "the method %s", method_name);
	HAL_WARNING ((buf));
	reply = dbus_message_new_error (in_reply_to,
					"org.freedesktop.Hal.SyntaxError",
					buf);
	if (reply == NULL)
		DIE (("No memory"));
	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));
	dbus_message_unref (reply);
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

	dbus_message_iter_append_string (iter, hal_device_get_udi (device));

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

	HAL_TRACE (("entering"));

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_array (&iter, &iter_array,
					DBUS_TYPE_STRING);

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_get_udi,
				  &iter_array);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
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
		dbus_message_iter_append_string (info->iter,
						 hal_device_get_udi (device));
	}

	return TRUE;
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
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_array (&iter, &iter_array,
					DBUS_TYPE_STRING);

	info.key = key;
	info.iter = &iter_array;

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_match_get_udi,
				  &info);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

typedef struct {
	const char *capability;
	DBusMessageIter *iter;
} DeviceCapabilityInfo;

static gboolean
foreach_device_by_capability (HalDeviceStore *store, HalDevice *device,
			      gpointer user_data)
{
	DeviceCapabilityInfo *info = user_data;
	const char *caps;

	caps = hal_device_property_get_string (device, "info.capabilities");

	if (strstr (caps, info->capability) != NULL) {
		dbus_message_iter_append_string (info->iter,
						 hal_device_get_udi (device));
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
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_array (&iter, &iter_array,
					DBUS_TYPE_STRING);

	info.capability = capability;
	info.iter = &iter_array;

	hal_device_store_foreach (hald_get_gdl (),
				  foreach_device_by_capability,
				  &info);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

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
DBusHandlerResult
manager_device_exists (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusError error;
	HalDevice *d;
	const char *udi;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Manager.DeviceExists");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);

	reply = dbus_message_new_method_return (message);
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter, d != NULL);

	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
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

	HAL_TRACE (("entering, udi=%s", udi));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "DeviceAdded");

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
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

	HAL_TRACE (("entering, udi=%s", udi));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "DeviceRemoved");

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
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

	HAL_TRACE (("entering, udi=%s, cap=%s", udi, capability));

	message = dbus_message_new_signal ("/org/freedesktop/Hal/Manager",
					   "org.freedesktop.Hal.Manager",
					   "NewCapability");

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);
	dbus_message_iter_append_string (&iter, capability);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
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
	DBusMessageIter *iter = user_data;
	const char *key;
	int type;

	key = hal_property_get_key (p);
	type = hal_property_get_type (p);

	dbus_message_iter_append_dict_key (iter, key);

	switch (type) {
	case DBUS_TYPE_STRING:
		dbus_message_iter_append_string (iter,
						 hal_property_get_string (p));
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_append_int32 (iter,
						hal_property_get_int (p));
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_append_double (iter,
						 hal_property_get_double (p));
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_append_boolean (iter,
						  hal_property_get_bool (p));
		break;
		
	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

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
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_dict (&iter, &iter_dict);

	hal_device_property_foreach (d,
				     foreach_property_append,
				     &iter_dict);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

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
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "GetProperty");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	p = hal_device_property_find (d, key);
	if (p == NULL) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);

	type = hal_property_get_type (p);
	switch (type) {
	case DBUS_TYPE_STRING:
		dbus_message_iter_append_string (&iter,
						 hal_property_get_string (p));
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_append_int32 (&iter,
						hal_property_get_int (p));
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_append_double (&iter,
						 hal_property_get_double (p));
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_append_boolean (&iter,
						  hal_property_get_bool (p));
		break;

	default:
		HAL_WARNING (("Unknown property type %d", type));
		break;
	}

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

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

	udi = dbus_message_get_path (message);

	HAL_TRACE (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "GetPropertyType");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	p = hal_device_property_find (d, key);
	if (p == NULL) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_int32 (&iter,
					hal_property_get_type (p));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);

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
DBusHandlerResult
device_set_property (DBusConnection * connection, DBusMessage * message)
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
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	key = dbus_message_iter_get_string (&iter);

	HAL_DEBUG (("udi=%s, key=%s", udi, key));

	device = hal_device_store_find (hald_get_gdl (), udi);
	if (device == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	dbus_message_iter_next (&iter);

	/** @todo check permissions of the sender vs property to be modified */

	type = dbus_message_iter_get_arg_type (&iter);
	rc = FALSE;
	switch (type) {
	case DBUS_TYPE_STRING:
		rc = hal_device_property_set_string (device, key,
					     dbus_message_iter_get_string
					     (&iter));
		break;
	case DBUS_TYPE_INT32:
		rc = hal_device_property_set_int (device, key,
					  dbus_message_iter_get_int32
					  (&iter));
		break;
	case DBUS_TYPE_DOUBLE:
		rc = hal_device_property_set_double (device, key,
					     dbus_message_iter_get_double
					     (&iter));
		break;
	case DBUS_TYPE_BOOLEAN:
		rc = hal_device_property_set_bool (device, key,
					   dbus_message_iter_get_boolean
					   (&iter));
		break;

	default:
		HAL_WARNING (("Unsupported property type %d", type));
		break;
	}

	if (!rc) {
		raise_property_type_error (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
DBusHandlerResult
device_add_capability (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	const char *capability;
	const char *caps;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	char buf[MAX_CAP_SIZE];

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "AddCapability");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}


	caps = hal_device_property_get_string (d, "info.capabilities");
	if (caps == NULL) {
		hal_device_property_set_string (d, "info.capabilities",
					capability);
	} else {
		if (strstr (caps, capability) == NULL) {
			snprintf (buf, MAX_CAP_SIZE, "%s %s", caps,
				  capability);
			hal_device_property_set_string (d, "info.capabilities",
						buf);
		}
	}

	manager_send_signal_new_capability (d, capability);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
DBusHandlerResult
device_remove_property (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	char *key;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "RemoveProperty");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (!hal_device_property_remove (d, key)) {
		raise_no_such_property (connection, message, udi, key);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}


	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
DBusHandlerResult
device_property_exists (DBusConnection * connection, DBusMessage * message)
{
	const char *udi;
	char *key;
	HalDevice *d;
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter iter;

	HAL_TRACE (("entering"));

	udi = dbus_message_get_path (message);

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "RemoveProperty");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter,
					  hal_device_has_property (d, key));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &capability,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "QueryCapability");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	rc = FALSE;
	caps = hal_device_property_get_string (d, "info.capabilities");
	if (caps != NULL) {
		if (strstr (caps, capability) != NULL)
			rc = TRUE;
	}

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter, rc);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
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
		HAL_WARNING (("*** atomic_count = %d < 0 !!",
			      atomic_count));
		atomic_count = 0;
	}

	if (atomic_count == 0 && num_pending_updates > 0) {
		DBusMessage *message;
		DBusMessageIter iter;

		for (pu_iter = pending_updates_head;
		     pu_iter != NULL; pu_iter = pu_iter_next) {
			int num_updates_this;

			pu_iter_next = pu_iter->next;

			if (pu_iter->udi == NULL)
				goto have_processed;

			/* count number of updates for this device */
			num_updates_this = 0;
			for (pu_iter2 = pu_iter;
			     pu_iter2 != NULL; pu_iter2 = pu_iter2->next) {
				if (strcmp (pu_iter2->udi, pu_iter->udi) == 0)
					num_updates_this++;
			}

			/* prepare message */
			message = dbus_message_new_signal (
				pu_iter->udi,
				"org.freedesktop.Hal.Device",
				"PropertyModified");
			dbus_message_iter_init (message, &iter);
			dbus_message_iter_append_int32 (&iter,
							num_updates_this);
			for (pu_iter2 = pu_iter; pu_iter2 != NULL;
			     pu_iter2 = pu_iter2->next) {
				if (strcmp (pu_iter2->udi, pu_iter->udi) == 0) {
					dbus_message_iter_append_string
					    (&iter, pu_iter2->key);
					dbus_message_iter_append_boolean
					    (&iter, pu_iter2->removed);
					dbus_message_iter_append_boolean
					    (&iter, pu_iter2->added);

					/* signal this is already processed */
					if (pu_iter2 != pu_iter) {
						g_free (pu_iter2->udi);
						pu_iter2->udi = NULL;
					}
				}
			}


			if (!dbus_connection_send
			    (dbus_connection, message, NULL))
				DIE (("error broadcasting message"));

			dbus_message_unref (message);

		      have_processed:
			g_free (pu_iter->key);
			g_free (pu_iter);
		}		/* for all updates */

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
		message = dbus_message_new_signal (udi,
						  "org.freedesktop.Hal.Device",
						   "PropertyModified");

		dbus_message_iter_init (message, &iter);
		dbus_message_iter_append_int32 (&iter, 1);
		dbus_message_iter_append_string (&iter, key);
		dbus_message_iter_append_boolean (&iter, removed);
		dbus_message_iter_append_boolean (&iter, added);

		if (!dbus_connection_send (dbus_connection, message, NULL))
			DIE (("error broadcasting message"));

		dbus_message_unref (message);
	}
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
device_send_signal_condition (HalDevice *device, const char *condition_name,
			      int first_arg_type, ...)
{
	const char *udi = hal_device_get_udi (device);
	DBusMessage *message;
	DBusMessageIter iter;
	va_list var_args;

	message = dbus_message_new_signal (udi,
					   "org.freedesktop.Hal.Device",
					   "Condition");
	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, condition_name);

	va_start (var_args, first_arg_type);
	dbus_message_append_args_valist (message, first_arg_type,
					 var_args);
	va_end (var_args);

	if (!dbus_connection_send (dbus_connection, message, NULL))
		DIE (("error broadcasting message"));

	dbus_message_unref (message);
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
DBusHandlerResult
agent_manager_new_device (DBusConnection * connection,
			  DBusMessage * message)
{
	HalDevice *d;
	DBusMessage *reply;
	DBusMessageIter iter;
	const char *udi;

	HAL_TRACE (("entering"));

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	d = hal_device_new ();
	udi = hal_device_get_udi (d);

	HAL_TRACE (("udi=%s", udi));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_string (&iter, udi);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

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
DBusHandlerResult
agent_manager_commit_to_gdl (DBusConnection * connection,
			     DBusMessage * message)
{
	DBusMessage *reply;
	DBusError error;
	HalDevice *d;
	const char *old_udi;
	const char *new_udi;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &old_udi,
				    DBUS_TYPE_STRING, &new_udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "CommitToGdl");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	HAL_TRACE (("entering, old_udi=%s, new_udi=%s", old_udi, new_udi));

	d = hal_device_store_find (hald_get_gdl (), old_udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, old_udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (hal_device_store_find (hald_get_gdl (), new_udi) != NULL) {
		raise_udi_in_use (connection, message, new_udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* add to the GDL */
	hal_device_store_add (hald_get_gdl (), d);

	/* Ok, send out a signal on the Manager interface that we added
	 * this device to the gdl */
	/*manager_send_signal_device_added(new_udi); */

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
DBusHandlerResult
agent_manager_remove (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusError error;
	HalDevice *d;
	const char *udi;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "Remove");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	HAL_INFO (("entering, udi=%s", udi));

	d = hal_device_store_find (hald_get_gdl (), udi);
	if (d == NULL) {
		raise_no_such_device (connection, message, udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	g_object_unref (d);

	/* Ok, send out a signal on the Manager interface that we removed
	 * this device from the gdl */
	/*
	   manager_send_signal_device_removed(udi);
	 */

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
DBusHandlerResult
agent_merge_properties (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusError error;
	HalDevice *target_d;
	HalDevice *source_d;
	const char *target_udi;
	const char *source_udi;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &target_udi,
				    DBUS_TYPE_STRING, &source_udi,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "MergeProperties");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	HAL_TRACE (("entering, target_udi=%s, source_udi=%s",
		    target_udi, source_udi));

	target_d = hal_device_store_find (hald_get_gdl (), target_udi);
	if (target_d == NULL) {
		raise_no_such_device (connection, message, target_udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	source_d = hal_device_store_find (hald_get_gdl (), source_udi);
	if (source_d == NULL) {
		raise_no_such_device (connection, message, source_udi);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	hal_device_merge (target_d, source_d);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
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
DBusHandlerResult
agent_device_matches (DBusConnection * connection, DBusMessage * message)
{
	DBusMessage *reply;
	DBusError error;
	HalDevice *d1;
	HalDevice *d2;
	const char *udi1;
	const char *udi2;
	const char *namespace;
	dbus_bool_t rc;
	DBusMessageIter iter;

	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error,
				    DBUS_TYPE_STRING, &udi1,
				    DBUS_TYPE_STRING, &udi2,
				    DBUS_TYPE_STRING, &namespace,
				    DBUS_TYPE_INVALID)) {
		raise_syntax (connection, message, "DeviceMatches");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	HAL_TRACE (("entering, udi1=%s, udi2=%s, namespace=%s",
		    udi1, udi2, namespace));

	d1 = hal_device_store_find (hald_get_gdl (), udi1);
	if (d1 == NULL) {
		raise_no_such_device (connection, message, udi1);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	d2 = hal_device_store_find (hald_get_gdl (), udi2);
	if (d2 == NULL) {
		raise_no_such_device (connection, message, udi2);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	rc = hal_device_matches (d1, d2, namespace);

	reply = dbus_message_new_method_return (message);
	if (reply == NULL)
		DIE (("No memory"));

	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_append_boolean (&iter, rc);

	if (!dbus_connection_send (connection, reply, NULL))
		DIE (("No memory"));

	dbus_message_unref (reply);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @param  user_data           User data
 *  @return                     What to do with the message
 */
static DBusHandlerResult
filter_function (DBusConnection * connection,
		 DBusMessage * message, void *user_data)
{
/*
    HAL_INFO (("obj_path=%s interface=%s method=%s", 
    dbus_message_get_path(message), 
    dbus_message_get_interface(message),
    dbus_message_get_member(message)));
*/

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
	} else
	    if (dbus_message_is_method_call
		(message, "org.freedesktop.Hal.Manager",
		 "FindDeviceByCapability")
		&& strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return manager_find_device_by_capability (connection,
							  message);
	}

	else if (dbus_message_is_method_call (message,
					      "org.freedesktop.Hal.AgentManager",
					      "NewDevice") &&
		 strcmp (dbus_message_get_path (message),
			 "/org/freedesktop/Hal/Manager") == 0) {
		return agent_manager_new_device (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.AgentManager",
						"CommitToGdl") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return agent_manager_commit_to_gdl (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.AgentManager",
						"Remove") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return agent_manager_remove (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.AgentManager",
						"MergeProperties") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return agent_merge_properties (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.AgentManager",
						"DeviceMatches") &&
		   strcmp (dbus_message_get_path (message),
			   "/org/freedesktop/Hal/Manager") == 0) {
		return agent_device_matches (connection, message);
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
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyString")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyInteger")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyBoolean")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"SetPropertyDouble")) {
		return device_set_property (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"RemoveProperty")) {
		return device_remove_property (connection, message);
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
		return device_add_capability (connection, message);
	} else if (dbus_message_is_method_call (message,
						"org.freedesktop.Hal.Device",
						"QueryCapability")) {
		return device_query_capability (connection, message);
	} else
		osspec_filter_function (connection, message, user_data);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusConnection *
hald_dbus_init (void)
{
	DBusError dbus_error;

	dbus_connection_set_change_sigpipe (TRUE);

	dbus_error_init (&dbus_error);
	dbus_connection = dbus_bus_get (DBUS_BUS_SYSTEM, &dbus_error);
	if (dbus_connection == NULL) {
		HAL_ERROR (("dbus_bus_get(): %s", dbus_error.message));
		exit (1);
	}

	dbus_connection_setup_with_g_main (dbus_connection, NULL);

	dbus_bus_acquire_service (dbus_connection, "org.freedesktop.Hal",
				  0, &dbus_error);
	if (dbus_error_is_set (&dbus_error)) {
		HAL_ERROR (("dbus_bus_acquire_service(): %s",
			    dbus_error.message));
		exit (1);
	}

	dbus_connection_add_filter (dbus_connection, filter_function, NULL,
				    NULL);

	return dbus_connection;
}

/** @} */
