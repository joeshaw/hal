/***************************************************************************
 * CVSID: $Id$
 *
 * libhal.c : HAL daemon C convenience library
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307	 USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include "libhal.h"

/**
 * @defgroup LibHal HAL convenience library
 * @brief A convenience library used to communicate with the HAL daemon
 *	  using D-BUS
 *
 *  @{
 */

/** Frees a NULL-terminated array of strings. If passed NULL, does nothing.
 *
 *  @param  str_array		The array to be freed
 */
void
hal_free_string_array (char **str_array)
{
	if (str_array != NULL) {
		int i;

		for (i = 0; str_array[i] != NULL; i++) {
			free (str_array[i]);
		}
		free (str_array);
	}
}

/** Frees a nul-terminated string
 *
 *  @param  str			The nul-terminated sting to free
 */
void
hal_free_string (char *str)
{
	/** @todo implement for UTF8 */
	free (str);
}


/** Represents a set of properties */
struct LibHalPropertySet_s {
	unsigned int num_properties; /**< Number of properties in set */
	LibHalProperty *properties_head;
				     /**< Pointer to first property or NULL
				      *	  if there are no properties */
};

/** Device property class */
struct LibHalProperty_s {
	int type;		     /**< Type of property */
	char *key;		     /**< ASCII string */

	/** Possible values of the property */
	union {
		char *str_value;     /**< UTF-8 zero-terminated string */
		dbus_int32_t int_value;
				     /**< 32-bit signed integer */
		double double_value; /**< IEEE754 double precision float */
		dbus_bool_t bool_value;
				     /**< Truth value */
	};

	LibHalProperty *next;	     /**< Next property or NULL if this is 
				      *	  the last */
};

/** Context for connection to the HAL daemon */
struct LibHalContext_s {
	DBusConnection *connection;           /**< D-BUS connection */
	dbus_bool_t is_initialized;           /**< Are we initialised */
	dbus_bool_t cache_enabled;            /**< Is the cache enabled */
	const LibHalFunctions *functions;     /**< Callback functions */
	void *user_data;                      /**< User data */
};

/** Set user data for the context
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  user_data           Opaque pointer
 */
void 
hal_ctx_set_user_data(LibHalContext *ctx, void *user_data)
{
	ctx->user_data = user_data;
}

/** Get user data for the context
 *
 *  @param  ctx                 The context for the connection to hald
 *  @return                     Opaque pointer stored through 
 *                              hal_ctx_set_user_data or NULL if not set
 */
void*
hal_ctx_get_user_data(LibHalContext *ctx)
{
	return ctx->user_data;
}


/** Retrieve all the properties on a device. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique id of device
 *  @return			An object represent all properties. Must be
 *				freed with hal_free_property_set
 */
LibHalPropertySet *
hal_device_get_all_properties (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict_iter;
	LibHalPropertySet *result;
	LibHalProperty **pn;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetAllProperties");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d: %s raised\n\"%s\"\n\n", __FILE__,
			 __LINE__, error.name, error.message);
		dbus_message_unref (message);
		return NULL;
	}

	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	dbus_message_iter_init (reply, &iter);

	result = malloc (sizeof (LibHalPropertySet));
	if (result == NULL) {
		fprintf (stderr, "%s %d : error allocating memory\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	}

/*
    result->properties = malloc(sizeof(LibHalProperty)*result->num_properties);
    if( result->properties==NULL )
    {
    /// @todo  cleanup
	return NULL;
    }
*/

	pn = &result->properties_head;
	result->num_properties = 0;

	dbus_message_iter_init_dict_iterator (&iter, &dict_iter);

	do {
		char *dbus_str;
		LibHalProperty *p;

		p = malloc (sizeof (LibHalProperty));
		if (p == NULL) {
			fprintf (stderr,
				 "%s %d : error allocating memory\n",
				 __FILE__, __LINE__);
			/** @todo FIXME cleanup */
			return NULL;
		}

		*pn = p;
		pn = &p->next;
		p->next = NULL;
		result->num_properties++;

		dbus_str = dbus_message_iter_get_dict_key (&dict_iter);
		p->key =
		    (char *) ((dbus_str != NULL) ? strdup (dbus_str) :
			      NULL);
		if (p->key == NULL) {
			fprintf (stderr,
				 "%s %d : error allocating memory\n",
				 __FILE__, __LINE__);
			/** @todo FIXME cleanup */
			return NULL;
		}
		dbus_free (dbus_str);

		p->type = dbus_message_iter_get_arg_type (&dict_iter);

		switch (p->type) {
		case DBUS_TYPE_STRING:
			dbus_str =
			    dbus_message_iter_get_string (&dict_iter);
			p->str_value =
			    (char *) ((dbus_str != NULL) ?
				      strdup (dbus_str) : NULL);
			if (p->str_value == NULL) {
				fprintf (stderr,
					 "%s %d : error allocating memory\n",
					 __FILE__, __LINE__);
				/** @todo FIXME cleanup */
				return NULL;
			}
			dbus_free (dbus_str);
			break;
		case DBUS_TYPE_INT32:
			p->int_value =
			    dbus_message_iter_get_int32 (&dict_iter);
			break;
		case DBUS_TYPE_DOUBLE:
			p->double_value =
			    dbus_message_iter_get_double (&dict_iter);
			break;
		case DBUS_TYPE_BOOLEAN:
			p->bool_value =
			    dbus_message_iter_get_boolean (&dict_iter);
			break;

		default:
			/** @todo  report error */
			break;
		}

	}
	while (dbus_message_iter_has_next (&dict_iter) &&
	       dbus_message_iter_next (&dict_iter));

	dbus_message_unref (message);
	dbus_message_unref (reply);

	return result;
}

/** Free a property set earlier obtained with hal_device_get_all_properties().
 *
 *  @param  set			Property-set to free
 */
void
hal_free_property_set (LibHalPropertySet * set)
{
	LibHalProperty *p;
	LibHalProperty *q;

	for (p = set->properties_head; p != NULL; p = q) {
		free (p->key);
		if (p->type == DBUS_TYPE_STRING)
			free (p->str_value);
		q = p->next;
		free (p);
	}
	free (set);
}

/** Initialize a property set iterator.
 *
 *  @param  iter		Iterator object
 *  @param  set			Property set
 */
void
hal_psi_init (LibHalPropertySetIterator * iter, LibHalPropertySet * set)
{
	iter->set = set;
	iter->index = 0;
	iter->cur_prop = set->properties_head;
}

/** Determine whether there are more properties to iterate over
 *
 *  @param  iter		Iterator object
 *  @return			TRUE if there are more properties
 */
dbus_bool_t
hal_psi_has_more (LibHalPropertySetIterator * iter)
{
	return iter->index < iter->set->num_properties;
}

/** Advance iterator to next property.
 *
 *  @param  iter		Iterator object
 */
void
hal_psi_next (LibHalPropertySetIterator * iter)
{
	iter->index++;
	iter->cur_prop = iter->cur_prop->next;
}

/** Get type of property.
 *
 *  @param  iter		Iterator object
 *  @return			One of DBUS_TYPE_STRING, DBUS_TYPE_INT32,
 *				DBUS_TYPE_BOOL, DBUS_TYPE_DOUBLE
 */
int
hal_psi_get_type (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->type;
}

/** Get the key of a property. 
 *
 *  @param  iter		Iterator object
 *  @return			ASCII nul-terminated string. This pointer is
 *				only valid until hal_free_property_set() is
 *				invoked on the property set this property 
 *				belongs to
 */
char *
hal_psi_get_key (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->key;
}

/** Get the value of a property of type string. 
 *
 *  @param  iter		Iterator object
 *  @return			UTF8 nul-terminated string. This pointer is
 *				only valid until hal_free_property_set() is
 *				invoked on the property set this property 
 *				belongs to
 */
char *
hal_psi_get_string (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->str_value;
}

/** Get the value of a property of type integer. 
 *
 *  @param  iter		Iterator object
 *  @return			32-bit signed integer
 */
dbus_int32_t
hal_psi_get_int (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->int_value;
}

/** Get the value of a property of type double. 
 *
 *  @param  iter		Iterator object
 *  @return			IEEE754 double precision float
 */
double
hal_psi_get_double (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->double_value;
}

/** Get the value of a property of type bool. 
 *
 *  @param  iter		Iterator object
 *  @return			Truth value
 */
dbus_bool_t
hal_psi_get_bool (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->bool_value;
}


#ifndef DOXYGEN_SHOULD_SKIP_THIS
static DBusHandlerResult
filter_func (DBusConnection * connection,
	     DBusMessage * message, void *user_data)
{
	const char *object_path;
	DBusError error;
	LibHalContext *ctx = (LibHalContext *) user_data;

	dbus_error_init (&error);

	object_path = dbus_message_get_path (message);

	/*printf("*** in filter_func, object_path=%s\n", object_path); */

	if (dbus_message_is_signal (message, "org.freedesktop.Hal.Manager",
				    "DeviceAdded")) {
		char *udi;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &udi,
					   DBUS_TYPE_INVALID)) {
			if (ctx->functions->device_added != NULL) {
				ctx->functions->device_added (ctx, udi);
				dbus_free (udi);
			}
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	} else
	    if (dbus_message_is_signal
		(message, "org.freedesktop.Hal.Manager",
		 "DeviceRemoved")) {
		char *udi;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &udi,
					   DBUS_TYPE_INVALID)) {
			if (ctx->functions->device_removed != NULL) {
				ctx->functions->device_removed (ctx, udi);
				dbus_free (udi);
			}
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	} else
	    if (dbus_message_is_signal
		(message, "org.freedesktop.Hal.Manager",
		 "NewCapability")) {
		char *udi;
		char *capability;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &udi,
					   DBUS_TYPE_STRING, &capability,
					   DBUS_TYPE_INVALID)) {
			if (ctx->functions->device_new_capability != NULL) {
				ctx->functions->device_new_capability (ctx, 
								       udi,
								  capability);
				dbus_free (udi);
				dbus_free (capability);
			}
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	} else
	    if (dbus_message_is_signal
		(message, "org.freedesktop.Hal.Device", "Condition")) {
		if (ctx->functions->device_condition != NULL) {
			DBusMessageIter iter;
			char *condition_name;

			dbus_message_iter_init (message, &iter);
			condition_name =
			    dbus_message_iter_get_string (&iter);

			ctx->functions->device_condition (ctx, 
							  object_path,
							  condition_name,
							  message);

			dbus_free (condition_name);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	} else
	    if (dbus_message_is_signal
		(message, "org.freedesktop.Hal.Device",
		 "PropertyModified")) {
		if (ctx->functions->device_property_modified != NULL) {
			int i;
			char *key;
			dbus_bool_t removed, added;
			int num_modifications;
			DBusMessageIter iter;

			dbus_message_iter_init (message, &iter);
			num_modifications =
			    dbus_message_iter_get_int32 (&iter);
			dbus_message_iter_next (&iter);


			for (i = 0; i < num_modifications; i++) {

				key = dbus_message_iter_get_string (&iter);
				dbus_message_iter_next (&iter);
				removed =
				    dbus_message_iter_get_boolean (&iter);
				dbus_message_iter_next (&iter);
				added =
				    dbus_message_iter_get_boolean (&iter);
				dbus_message_iter_next (&iter);

				ctx->functions->
				    device_property_modified (ctx, 
							      object_path,
							      key, removed,
							      added);

				dbus_free (key);
			}

		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static LibHalFunctions hal_null_functions = {
	NULL /*mainloop_integration */ ,
	NULL /*device_added */ ,
	NULL /*device_removed */ ,
	NULL /*device_new_capability */ ,
	NULL /*device_lost_capability */ ,
	NULL /*property_modified */ ,
	NULL /*device_condition */
};
#endif /* DOXYGEN_SHOULD_SKIP_THIS */

/** Initialize the HAL library. 
 *
 *  @param  cb_functions	  Callback functions. If this is set top NULL
 *				  then the library will not listen for
 *				  notifications. 
 *  @param  use_cache		  Retrieve all device information and cache it.
 *				  This is expensive both in terms of memory
 *				  (there may be 50 device objects with 20
 *				  properties each) and in terms of processing
 *				  power (your process will be woken up every
 *				  time a property is changed). 
 *				  Use with caution.
 *				  NOTE NOTE NOTE: Caching isn't actually
 *				  implemented yet, this is just a placeholder
 *				  to preserve API compatibility.
 *  @return			  A LibHalContext object if succesful, and
 *				  NULL if an error occured
 */
LibHalContext*
hal_initialize (const LibHalFunctions * cb_functions,
		dbus_bool_t use_cache)
{
	DBusError error;
	LibHalContext *ctx;
	
	ctx = malloc (sizeof (LibHalContext));
	if (ctx == NULL) {
		fprintf (stderr, "%s %d : Cannot allocated %d bytes!\n",
			 __FILE__, __LINE__, sizeof (LibHalContext));
		return NULL;
	}

	ctx->cache_enabled = use_cache;

	ctx->functions = cb_functions;
	/* allow caller to pass NULL */
	if (ctx->functions == NULL)
		ctx->functions = &hal_null_functions;

	/* connect to hald service on the system bus */
	dbus_error_init (&error);
	ctx->connection = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (ctx->connection == NULL) {
		fprintf (stderr,
			 "%s %d : Error connecting to system bus: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_error_free (&error);
		return NULL;
	}

	if (ctx->functions->main_loop_integration != NULL) {

		ctx->functions->main_loop_integration (ctx, ctx->connection);
	}

	if (!dbus_connection_add_filter
	    (ctx->connection, filter_func, ctx, NULL)) {
		fprintf (stderr,
			 "%s %d : Error creating connection handler\r\n",
			 __FILE__, __LINE__);
		/** @todo  clean up */
		return NULL;
	}

	dbus_bus_add_match (ctx->connection,
			    "type='signal',"
			    "interface='org.freedesktop.Hal.Manager',"
			    "sender='org.freedesktop.Hal',"
			    "path='/org/freedesktop/Hal/Manager'", &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error subscribing to signals, "
			 "error=%s\r\n",
			 __FILE__, __LINE__, error.message);
		/** @todo  clean up */
		return NULL;
	}

	ctx->is_initialized = TRUE;

	return ctx;
}

/** Shutdown the HAL library. All resources allocated are freed. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @return			Zero if the shutdown went well, otherwise
 *				non-zero if an error occured
 */
int
hal_shutdown (LibHalContext *ctx)
{
	if (!ctx->is_initialized)
		return 1;

	/** @todo cleanup */
	free (ctx);
	return 0;
}

/** Get all devices in the Global Device List (GDL).
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  num_devices		The number of devices will be stored here
 *  @return			An array of device identifiers terminated
 *				with NULL. It is the responsibility of the
 *				caller to free with hal_free_string_array().
 *				If an error occurs NULL is returned.
 */
char **
hal_get_all_devices (LibHalContext *ctx, int *num_devices)
{
	int i;
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char **device_names;
	char **hal_device_names;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"GetAllDevices");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : %s raised\n\"%s\"\n\n", __FILE__,
			 __LINE__, error.name, error.message);
		dbus_message_unref (message);
		return NULL;
	}

	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	/* now analyze reply */
	dbus_message_iter_init (reply, &iter);
	if (!dbus_message_iter_get_string_array (&iter,
						 &device_names,
						 num_devices)) {
		fprintf (stderr, "%s %d : wrong reply from hald\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_unref (reply);
	dbus_message_unref (message);

	/* Have to convert from dbus string array to hal string array 
	 * since we can't poke at the dbus string array for the reason
	 * that d-bus use their own memory allocation scheme
	 */
	hal_device_names = malloc (sizeof (char *) * ((*num_devices) + 1));
	if (hal_device_names == NULL)
		return NULL;
	/** @todo Handle OOM better */

	for (i = 0; i < (*num_devices); i++) {
		hal_device_names[i] = strdup (device_names[i]);
		if (hal_device_names[i] == NULL) {
			fprintf (stderr,
				 "%s %d : error allocating memory\n",
				 __FILE__, __LINE__);
			/** @todo FIXME cleanup */
			return NULL;
		}
	}
	hal_device_names[i] = NULL;

	dbus_free_string_array (device_names);

	return hal_device_names;
}

/** Query a property type of a device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @return			One of DBUS_TYPE_STRING, DBUS_TYPE_INT32,
 *				DBUS_TYPE_BOOL, DBUS_TYPE_DOUBLE or 
 *				DBUS_TYPE_NIL if the property didn't exist.
 */
int
hal_device_get_property_type (LibHalContext *ctx, 
			      const char *udi, const char *key)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	int type;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyType");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return DBUS_TYPE_NIL;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : %s raised\n\"%s\"\n\n", __FILE__,
			 __LINE__, error.name, error.message);
		dbus_message_unref (message);
		return DBUS_TYPE_NIL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return DBUS_TYPE_NIL;
	}

	dbus_message_iter_init (reply, &iter);
	type = dbus_message_iter_get_int32 (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);

	return type;
}

/** Get the value of a property of type string. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @return			UTF8 nul-terminated string. The caller is
 *				responsible for freeing this string with the
 *				function hal_free_string(). 
 *				Returns NULL if the property didn't exist
 *				or we are OOM
 */
char *
hal_device_get_property_string (LibHalContext *ctx,
				const char *udi, const char *key)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char *value;
	char *dbus_str;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyString");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_NIL) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' does not "
			 "exist\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	} else if (dbus_message_iter_get_arg_type (&iter) !=
		   DBUS_TYPE_STRING) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type string\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	}

	dbus_str = dbus_message_iter_get_string (&iter);
	value = (char *) ((dbus_str != NULL) ? strdup (dbus_str) : NULL);
	if (value == NULL) {
		fprintf (stderr, "%s %d : error allocating memory\n",
			 __FILE__, __LINE__);
		/** @todo FIXME cleanup */
		return NULL;
	}
	dbus_free (dbus_str);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Get the value of a property of type integer. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @return			32-bit signed integer
 */
dbus_int32_t
hal_device_get_property_int (LibHalContext *ctx, 
			     const char *udi, const char *key)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	dbus_int32_t value;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyInteger");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return -1;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return -1;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return -1;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_NIL) {
		/* property didn't exist */
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' does not "
			 "exist\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1;
	} else if (dbus_message_iter_get_arg_type (&iter) !=
		   DBUS_TYPE_INT32) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type integer\n", __FILE__, __LINE__, key,
			 udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1;
	}
	value = dbus_message_iter_get_int32 (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Get the value of a property of type double. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @return			IEEE754 double precision float
 */
double
hal_device_get_property_double (LibHalContext *ctx, 
				const char *udi, const char *key)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	double value;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyDouble");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return -1.0f;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return -1.0f;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return -1.0f;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_NIL) {
		/* property didn't exist */
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' does not "
			 "exist\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1.0f;
	} else if (dbus_message_iter_get_arg_type (&iter) !=
		   DBUS_TYPE_DOUBLE) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type double\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1.0f;
	}
	value = dbus_message_iter_get_double (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return (double) value;
}

/** Get the value of a property of type bool. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @return			Truth value
 */
dbus_bool_t
hal_device_get_property_bool (LibHalContext *ctx, 
			      const char *udi, const char *key)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	double value;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyBoolean");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_NIL) {
		/* property didn't exist */
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' does not "
			 "exist\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	} else if (dbus_message_iter_get_arg_type (&iter) !=
		   DBUS_TYPE_BOOLEAN) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type bool\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}
	value = dbus_message_iter_get_boolean (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}


/* generic helper */
static int
hal_device_set_property_helper (LibHalContext *ctx, 
				const char *udi,
				const char *key,
				int type,
				const char *str_value,
				dbus_int32_t int_value,
				double double_value,
				dbus_bool_t bool_value)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char *method_name = NULL;

	/** @todo  sanity check incoming params */

	switch (type) {
	case DBUS_TYPE_NIL:
		method_name = "RemoveProperty";
		break;
	case DBUS_TYPE_STRING:
		method_name = "SetPropertyString";
		break;
	case DBUS_TYPE_INT32:
		method_name = "SetPropertyInteger";
		break;
	case DBUS_TYPE_DOUBLE:
		method_name = "SetPropertyDouble";
		break;
	case DBUS_TYPE_BOOLEAN:
		method_name = "SetPropertyBoolean";
		break;

	default:
		/* cannot happen; is not callable from outside this file */
		break;
	}

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						method_name);
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	switch (type) {
	case DBUS_TYPE_NIL:
		dbus_message_iter_append_nil (&iter);
		break;
	case DBUS_TYPE_STRING:
		dbus_message_iter_append_string (&iter, str_value);
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_append_int32 (&iter, int_value);
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_append_double (&iter, double_value);
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_append_boolean (&iter, bool_value);
		break;
	}

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d: %s raised\n\"%s\"\n\n", __FILE__,
			 __LINE__, error.name, error.message);
		dbus_message_unref (message);
		return FALSE;
	}

	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	return TRUE;
}

/** Set a property of type string.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @param  value		Value of the property; a UTF8 string
 *  @return			TRUE if the property was set, FALSE if
 *				the device didn't exist or the property
 *				had a different type.
 */
dbus_bool_t
hal_device_set_property_string (LibHalContext *ctx, 
				const char *udi,
				const char *key, const char *value)
{
	return hal_device_set_property_helper (ctx, udi, key,
					       DBUS_TYPE_STRING,
					       value, 0, 0.0f, FALSE);
}

/** Set a property of type integer.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @param  value		Value of the property
 *  @return			TRUE if the property was set, FALSE if
 *				the device didn't exist or the property
 *				had a different type.
 */
dbus_bool_t
hal_device_set_property_int (LibHalContext *ctx, const char *udi,
			     const char *key, dbus_int32_t value)
{
	return hal_device_set_property_helper (ctx, udi, key,
					       DBUS_TYPE_INT32,
					       NULL, value, 0.0f, FALSE);
}

/** Set a property of type double.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @param  value		Value of the property
 *  @return			TRUE if the property was set, FALSE if
 *				the device didn't exist or the property
 *				had a different type.
 */
dbus_bool_t
hal_device_set_property_double (LibHalContext *ctx, const char *udi,
				const char *key, double value)
{
	return hal_device_set_property_helper (ctx, udi, key,
					       DBUS_TYPE_DOUBLE,
					       NULL, 0, value, FALSE);
}

/** Set a property of type bool.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @param  value		Value of the property
 *  @return			TRUE if the property was set, FALSE if
 *				the device didn't exist or the property
 *				had a different type.
 */
dbus_bool_t
hal_device_set_property_bool (LibHalContext *ctx, const char *udi,
			      const char *key, dbus_bool_t value)
{
	return hal_device_set_property_helper (ctx, udi, key,
					       DBUS_TYPE_BOOLEAN,
					       NULL, 0, 0.0f, value);
}


/** Remove a property.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  key			Name of the property
 *  @return			TRUE if the property was set, FALSE if
 *				the device didn't exist
 */
dbus_bool_t
hal_device_remove_property (LibHalContext *ctx, 
			    const char *udi, const char *key)
{
	return hal_device_set_property_helper (ctx, udi, key, DBUS_TYPE_NIL,	
					       /* DBUS_TYPE_NIL means remove */
					       NULL, 0, 0.0f, FALSE);
}


/** Create a new device object which will be hidden from applications
 *  until the CommitToGdl(), ie. hal_agent_commit_to_gdl(), method is called.
 *
 *  Note that the program invoking this method needs to run with super user
 *  privileges.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @return			Tempoary device unique id or NULL if there
 *				was a problem. This string must be freed
 *				by the caller.
 */
char *
hal_agent_new_device (LibHalContext *ctx)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char *value;
	char *dbus_str;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.AgentManager",
						"NewDevice");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRING) {
		fprintf (stderr,
			 "%s %d : expected a string in reply to NewDevice\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	}

	dbus_str = dbus_message_iter_get_string (&iter);
	value = (char *) ((dbus_str != NULL) ? strdup (dbus_str) : NULL);
	if (value == NULL) {
		fprintf (stderr, "%s %d : error allocating memory\n",
			 __FILE__, __LINE__);
		/** @todo FIXME cleanup */
		return NULL;
	}
	dbus_free (dbus_str);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}


/** When a hidden device have been built using the NewDevice method, ie.
 *  hal_agent_new_device(), and the org.freedesktop.Hal.Device interface
 *  this function will commit it to the global device list. 
 *
 *  This means that the device object will be visible to applications and
 *  the HAL daemon will possibly attempt to boot the device (depending on
 *  the property RequireEnable).
 *
 *  Note that the program invoking this method needs to run with super user
 *  privileges.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  temp_udi		The tempoary unique device id as returned by
 *				hal_agent_new_device()
 *  @param  udi			The new unique device id.
 *  @return			FALSE if the given unique device id is already
 *				in use.
 */
dbus_bool_t
hal_agent_commit_to_gdl (LibHalContext *ctx, 
			 const char *temp_udi, const char *udi)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.AgentManager",
						"CommitToGdl");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, temp_udi);
	dbus_message_iter_append_string (&iter, udi);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		if (dbus_error_has_name
		    (&error, "org.freedesktop.Hal.UdiInUse"))
			return FALSE;
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return TRUE;
}

/** This method can be invoked when a device is removed. The HAL daemon
 *  will shut down the device. Note that the device may still be in the device
 *  list if the Persistent property is set to true. 
 *
 *  Note that the program invoking this method needs to run with super user
 *  privileges.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			The new unique device id.
 *  @return			TRUE if the device was removed
 */
dbus_bool_t
hal_agent_remove_device (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.AgentManager",
						"Remove");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return TRUE;
}

/** Determine if a device exists.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique device id.
 *  @return			TRUE if the device exists
 */
dbus_bool_t
hal_device_exists (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	dbus_bool_t value;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"DeviceExists");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_BOOLEAN) {
		fprintf (stderr,
			 "%s %d : expected a bool in reply to DeviceExists\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}

	value = dbus_message_iter_get_boolean (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Determine if a property on a device exists.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique device id.
 *  @param  key			Name of the property
 *  @return			TRUE if the device exists
 */
dbus_bool_t
hal_device_property_exists (LibHalContext *ctx, 
			    const char *udi, const char *key)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	dbus_bool_t value;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"PropertyExists");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_iter_init (reply, &iter);

	/* now analyse reply */
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_BOOLEAN) {
		fprintf (stderr, "%s %d : expected a bool in reply to "
			 "PropertyExists\n", __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}

	value = dbus_message_iter_get_boolean (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Merge properties from one device to another.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  target_udi		Unique device id of target device to merge to
 *  @param  source_udi		Unique device id of device to merge from
 *  @return			TRUE if the properties was merged
 */
dbus_bool_t
hal_agent_merge_properties (LibHalContext *ctx, 
			    const char *target_udi, const char *source_udi)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.AgentManager",
						"MergeProperties");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, target_udi);
	dbus_message_iter_append_string (&iter, source_udi);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		if (dbus_error_has_name
		    (&error, "org.freedesktop.Hal.NoSuchDevice"))
			return FALSE;
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return TRUE;
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
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi1		Unique Device Id for device 1
 *  @param  udi2		Unique Device Id for device 2
 *  @param  property_namespace	Namespace for set of devices, e.g. "usb"
 *  @return			TRUE if all properties starting
 *				with the given namespace parameter
 *				from one device is in the other and 
 *				have the same value.
 */
dbus_bool_t
hal_agent_device_matches (LibHalContext *ctx, 
			  const char *udi1, const char *udi2,
			  const char *property_namespace)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	dbus_bool_t value;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.AgentManager",
						"DeviceMatches");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, udi1);
	dbus_message_iter_append_string (&iter, udi2);
	dbus_message_iter_append_string (&iter, property_namespace);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &iter);
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_BOOLEAN) {
		fprintf (stderr,
			 "%s %d : expected a bool in reply to DeviceMatches\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}

	value = dbus_message_iter_get_boolean (&iter);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Print a device to stdout; useful for debugging.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 */
void
hal_device_print (LibHalContext *ctx, const char *udi)
{
	int type;
	char *key;
	LibHalPropertySet *pset;
	LibHalPropertySetIterator i;

	printf ("device_id = %s\n", udi);

	pset = hal_device_get_all_properties (ctx, udi);

	for (hal_psi_init (&i, pset); hal_psi_has_more (&i);
	     hal_psi_next (&i)) {
		type = hal_psi_get_type (&i);
		key = hal_psi_get_key (&i);
		switch (type) {
		case DBUS_TYPE_STRING:
			printf ("    %s = %s (string)\n", key,
				hal_psi_get_string (&i));
			break;
		case DBUS_TYPE_INT32:
			printf ("    %s = %d = 0x%x (int)\n", key,
				hal_psi_get_int (&i),
				hal_psi_get_int (&i));
			break;
		case DBUS_TYPE_BOOLEAN:
			printf ("    %s = %s (bool)\n", key,
				(hal_psi_get_bool (&i) ? "true" :
				 "false"));
			break;
		case DBUS_TYPE_DOUBLE:
			printf ("    %s = %g (double)\n", key,
				hal_psi_get_double (&i));
			break;
		default:
			printf ("    *** unknown type for key %s\n", key);
			break;
		}
	}
	hal_free_property_set (pset);
}

/** Find a device in the GDL where a single string property matches a
 *  given value.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  key			Name of the property
 *  @param  value		Value to match
 *  @param  num_devices		Pointer to store number of devices
 *  @return			UDI of devices; free with 
 *				hal_free_string_array()
 */
char **
hal_manager_find_device_string_match (LibHalContext *ctx, 
				      const char *key,
				      const char *value, int *num_devices)
{
	int i;
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char **device_names;
	char **hal_device_names;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"FindDeviceStringMatch");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, key);
	dbus_message_iter_append_string (&iter, value);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &iter);
	if (!dbus_message_iter_get_string_array (&iter,
						 &device_names,
						 num_devices)) {
		fprintf (stderr, "%s %d : wrong reply from hald\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);

	/* Have to convert from dbus string array to hal string array 
	 * since we can't poke at the dbus string array for the reason
	 * that d-bus use their own memory allocation scheme
	 */
	hal_device_names = malloc (sizeof (char *) * ((*num_devices) + 1));
	if (hal_device_names == NULL)
		return NULL;
		     /** @todo Handle OOM better */

	for (i = 0; i < (*num_devices); i++) {
		hal_device_names[i] = strdup (device_names[i]);
		if (hal_device_names[i] == NULL)
			return NULL;
			 /** @todo Handle OOM better */
	}
	hal_device_names[i] = NULL;

	dbus_free_string_array (device_names);

	return hal_device_names;
}


/** Assign a capability to a device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  capability		Capability name
 *  @return			TRUE if the capability was added, FALSE if
 *				the device didn't exist
 */
dbus_bool_t
hal_device_add_capability (LibHalContext *ctx, 
			   const char *udi, const char *capability)
{
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"AddCapability");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, capability);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d: %s raised\n\"%s\"\n\n", __FILE__,
			 __LINE__, error.name, error.message);
		dbus_message_unref (message);
		return FALSE;
	}

	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (reply);
	dbus_message_unref (message);
	return TRUE;
}

/** Check if a device got a capability. The result is undefined if the
 *  device doesn't exist.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @param  capability		Capability name
 *  @return			TRUE if the device got the capability, 
 *				otherwise FALSE
 */
dbus_bool_t
hal_device_query_capability (LibHalContext *ctx, 
			     const char *udi, const char *capability)
{
	char *caps;

	caps = hal_device_get_property_string (ctx, udi, "info.capabilities");

	if (caps != NULL)
		if (strstr (caps, capability) != NULL)
			return TRUE;

	return FALSE;
}

/** Find devices with a given capability. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  capability		Capability name
 *  @param  num_devices		Pointer to store number of devices
 *  @return			UDI of devices; free with 
 *				hal_free_string_array()
 */
char **
hal_find_device_by_capability (LibHalContext *ctx, 
			       const char *capability, int *num_devices)
{
	int i;
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char **device_names;
	char **hal_device_names;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"FindDeviceByCapability");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_iter_init (message, &iter);
	dbus_message_iter_append_string (&iter, capability);

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error sending msg: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &iter);
	if (!dbus_message_iter_get_string_array (&iter,
						 &device_names,
						 num_devices)) {
		fprintf (stderr, "%s %d : wrong reply from hald\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);

	/* Have to convert from dbus string array to hal string array 
	 * since we can't poke at the dbus string array for the reason
	 * that d-bus use their own memory allocation scheme
	 */
	hal_device_names = malloc (sizeof (char *) * ((*num_devices) + 1));
	if (hal_device_names == NULL)
		return NULL;
	/** @todo Handle OOM better */

	for (i = 0; i < (*num_devices); i++) {
		hal_device_names[i] = strdup (device_names[i]);
		if (hal_device_names[i] == NULL)
			return NULL;
		/** @todo Handle OOM better */
	}
	hal_device_names[i] = NULL;

	dbus_free_string_array (device_names);

	return hal_device_names;
}

/** Watch all devices, ie. the device_property_changed callback is
 *  invoked when the properties on any device changes.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @return			Zero if the operation succeeded, otherwise
 *				non-zero
 */
int
hal_device_property_watch_all (LibHalContext *ctx)
{
	DBusError error;

	dbus_error_init (&error);

	dbus_bus_add_match (ctx->connection,
			    "type='signal',"
			    "interface='org.freedesktop.Hal.Device',"
			    "sender='org.freedesktop.Hal'", &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error subscribing to signals, "
			 "error=%s\r\n",
			 __FILE__, __LINE__, error.message);
		return 1;
	}
	return 0;
}


/** Add a watch on a device, so the device_property_changed callback is
 *  invoked when the properties on the given device changes.
 *
 *  The application itself is responsible for deleting the watch, using
 *  hal_device_remove_property_watch, if the device is removed.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @return			Zero if the operation succeeded, otherwise
 *				non-zero
 */
int
hal_device_add_property_watch (LibHalContext *ctx, const char *udi)
{
	char buf[512];
	DBusError error;

	dbus_error_init (&error);

	snprintf (buf, 512,
		  "type='signal',"
		  "interface='org.freedesktop.Hal.Device',"
		  "sender='org.freedesktop.Hal'," "path=%s", udi);

	dbus_bus_add_match (ctx->connection, buf, &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error subscribing to signals, "
			 "error=%s\r\n",
			 __FILE__, __LINE__, error.message);
		return 1;
	}
	return 0;
}


/** Remove a watch on a device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi			Unique Device Id
 *  @return			Zero if the operation succeeded, otherwise
 *				non-zero
 */
int
hal_device_remove_property_watch (LibHalContext *ctx, const char *udi)
{
	char buf[512];
	DBusError error;

	dbus_error_init (&error);

	snprintf (buf, 512,
		  "type='signal',"
		  "interface='org.freedesktop.Hal.Device',"
		  "sender='org.freedesktop.Hal'," "path=%s", udi);

	dbus_bus_remove_match (ctx->connection, buf, &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error unsubscribing to signals, "
			 "error=%s\r\n",
			 __FILE__, __LINE__, error.message);
		return 1;
	}
	return 0;
}


/** @} */
