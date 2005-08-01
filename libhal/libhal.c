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

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(String) dgettext (GETTEXT_PACKAGE, String)
# ifdef gettext_noop
#   define N_(String) gettext_noop (String)
# else
#   define N_(String) (String)
# endif
#else
/* Stubs that do something close enough.  */
# define textdomain(String) (String)
# define gettext(String) (String)
# define dgettext(Domain,Message) (Message)
# define dcgettext(Domain,Message,Type) (Message)
# define bindtextdomain(Domain,Directory) (Domain)
# define _(String)
# define N_(String) (String)
#endif

static char **libhal_get_string_array_from_iter (DBusMessageIter *iter, int *num_elements);

static dbus_bool_t libhal_property_fill_value_from_variant (LibHalProperty *p, DBusMessageIter *var_iter);



/**
 * @defgroup LibHal HAL convenience library
 * @brief A convenience library used to communicate with the HAL daemon
 *	  using D-BUS
 *
 *  @{
 */

/** Frees a NULL-terminated array of strings. If passed NULL, does nothing.
 *
 *  @param  str_array           The array to be freed
 */
void
libhal_free_string_array (char **str_array)
{
	if (str_array != NULL) {
		int i;

		for (i = 0; str_array[i] != NULL; i++) {
			free (str_array[i]);
		}
		free (str_array);
	}
}


/** Creates a NULL terminated array of strings from a dbus message iterator.
 *
 *  @param  iter		The message iterator to extract the strings from
 *  @param  num_elements        Pointer to an integer where to store number of elements (can be NULL)
 *  @return			Pointer to the string array
 */
static char **
libhal_get_string_array_from_iter (DBusMessageIter *iter, int *num_elements)
{
	int count;
	char **buffer;

	count = 0;
	buffer = (char **)malloc (sizeof (char *) * 8);

	if (buffer == NULL)
		goto oom;

	buffer[0] = NULL;
	while (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_STRING) {
		const char *value;
		char *str;
		
		if ((count % 8) == 0 && count != 0) {
			buffer = realloc (buffer, sizeof (char *) * (count + 8));
			if (buffer == NULL)
				goto oom;
		}
		
		dbus_message_iter_get_basic (iter, &value);
		str = strdup (value);
		if (str == NULL)
			goto oom;

		buffer[count] = str;

		dbus_message_iter_next(iter);
		count++;
	}

	if ((count % 8) == 0) {
		buffer = realloc (buffer, sizeof (char *) * (count + 1));
		if (buffer == NULL)
			goto oom;
	}

	buffer[count] = NULL;
	if (num_elements != NULL)
		*num_elements = count;
	return buffer;

oom:
	fprintf (stderr, "%s %d : error allocating memory\n", __FILE__, __LINE__);
	return NULL;

}

/** Frees a nul-terminated string
 *
 *  @param  str                 The nul-terminated sting to free
 */
void
libhal_free_string (char *str)
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
		dbus_uint64_t uint64_value;
				     /**< 64-bit unsigned integer */
		double double_value; /**< IEEE754 double precision float */
		dbus_bool_t bool_value;
				     /**< Truth value */

		char **strlist_value; /**< List of UTF-8 zero-terminated strings */
	};

	LibHalProperty *next;	     /**< Next property or NULL if this is 
				      *	  the last */
};

/** Context for connection to the HAL daemon */
struct LibHalContext_s {
	DBusConnection *connection;           /**< D-BUS connection */
	dbus_bool_t is_initialized;           /**< Are we initialised */
	dbus_bool_t is_shutdown;              /**< Have we been shutdown */
	dbus_bool_t cache_enabled;            /**< Is the cache enabled */
	dbus_bool_t is_direct;                /**< Whether the connection to hald is direct */

	/** Device added */
	LibHalDeviceAdded device_added;

	/** Device removed */
	LibHalDeviceRemoved device_removed;

	/** Device got a new capability */
	LibHalDeviceNewCapability device_new_capability;

	/** Device got a new capability */
	LibHalDeviceLostCapability device_lost_capability;

	/** A property of a device changed  */
	LibHalDevicePropertyModified device_property_modified;

	/** A non-continous event on the device occured  */
	LibHalDeviceCondition device_condition;

	void *user_data;                      /**< User data */
};

/** Set user data for the context
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  user_data           Opaque pointer
 *  @return                     TRUE if user data was successfully set,
 *                              FALSE if otherwise
 */
dbus_bool_t
libhal_ctx_set_user_data(LibHalContext *ctx, void *user_data)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	ctx->user_data = user_data;
	return TRUE;
}

/** Get user data for the context
 *
 *  @param  ctx                 The context for the connection to hald
 *  @return                     Opaque pointer stored through 
 *                              libhal_ctx_set_user_data or NULL if not set
 */
void*
libhal_ctx_get_user_data(LibHalContext *ctx)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);
	return ctx->user_data;
}


/** Fills in the value for the LibHalProperty given a variant iterator. 
 *
 *  @param  p                 The property to fill in
 *  @param  var_iter	      Varient iterator to extract the value from
 */
static dbus_bool_t
libhal_property_fill_value_from_variant (LibHalProperty *p, DBusMessageIter *var_iter)
{
	DBusMessageIter iter_array;
	switch (p->type) {
	case DBUS_TYPE_ARRAY:
		if (dbus_message_iter_get_element_type (var_iter) != DBUS_TYPE_STRING)
			return FALSE;

		dbus_message_iter_recurse (var_iter, &iter_array);
		p->strlist_value = libhal_get_string_array_from_iter (&iter_array, NULL);

		p->type = LIBHAL_PROPERTY_TYPE_STRLIST; 

		break;
	case DBUS_TYPE_STRING:
	{
		const char *v;

		dbus_message_iter_get_basic (var_iter, &v);

		p->str_value = strdup (v);
		if (p->str_value == NULL) 
			return FALSE;
		p->type = LIBHAL_PROPERTY_TYPE_STRING; 

		break;
	}
	case DBUS_TYPE_INT32:
	{
		dbus_int32_t v;
		
		dbus_message_iter_get_basic (var_iter, &v);
		
		p->int_value = v;
		p->type = LIBHAL_PROPERTY_TYPE_INT32; 

		break;
	}
	case DBUS_TYPE_UINT64:
	{
		dbus_uint64_t v;
		
		dbus_message_iter_get_basic (var_iter, &v);

		p->uint64_value = v;
		p->type = LIBHAL_PROPERTY_TYPE_UINT64; 
		
		break;
	}
	case DBUS_TYPE_DOUBLE:
	{
		double v;

		dbus_message_iter_get_basic (var_iter, &v);

		p->double_value = v;
		p->type = LIBHAL_PROPERTY_TYPE_DOUBLE; 

		break;
	}
	case DBUS_TYPE_BOOLEAN:
	{
		double v;

		dbus_message_iter_get_basic (var_iter, &v);

		p->double_value = v;
		p->type = LIBHAL_PROPERTY_TYPE_BOOLEAN; 

		break;
	}
	default:
		/** @todo  report error */
		break;
	}

	return TRUE;
}

/** Retrieve all the properties on a device. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique id of device
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     An object represent all properties. Must be
 *                              freed with libhal_free_property_set
 */
LibHalPropertySet *
libhal_device_get_all_properties (LibHalContext *ctx, const char *udi, DBusError *error)
{	
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter reply_iter;
	DBusMessageIter dict_iter;
	LibHalPropertySet *result;
	LibHalProperty *p_last;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);
	
	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetAllProperties");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		fprintf (stderr,
			 "%s %d : %s\n",
			 __FILE__, __LINE__, error->message);

		dbus_message_unref (message);
		return NULL;
	}

	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	dbus_message_iter_init (reply, &reply_iter);

	result = malloc (sizeof (LibHalPropertySet));
	if (result == NULL) 
		goto oom;
/*
    result->properties = malloc(sizeof(LibHalProperty)*result->num_properties);
    if( result->properties==NULL )
    {
    /// @todo  cleanup
	return NULL;
    }
*/

	result->properties_head = NULL;
	result->num_properties = 0;

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_ARRAY  &&
	    dbus_message_iter_get_element_type (&reply_iter) != DBUS_TYPE_DICT_ENTRY) {
		fprintf (stderr, "%s %d : error, expecting an array of dict entries\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	}

	dbus_message_iter_recurse (&reply_iter, &dict_iter);

	p_last = NULL;

	while (dbus_message_iter_get_arg_type (&dict_iter) == DBUS_TYPE_DICT_ENTRY)
	{
		DBusMessageIter dict_entry_iter, var_iter;
		const char *key;
		LibHalProperty *p;

		dbus_message_iter_recurse (&dict_iter, &dict_entry_iter);

		dbus_message_iter_get_basic (&dict_entry_iter, &key);

		p = malloc (sizeof (LibHalProperty));
		if (p == NULL)
			goto oom;

		p->next = NULL;

		if (result->num_properties == 0)
			result->properties_head = p;

		if (p_last != NULL)
			p_last->next = p;

		p_last = p;

		p->key = strdup (key);
		if (p->key == NULL)
			goto oom;

		dbus_message_iter_next (&dict_entry_iter);

		dbus_message_iter_recurse (&dict_entry_iter, &var_iter);


		p->type = dbus_message_iter_get_arg_type (&var_iter);
	
		result->num_properties++;

		if(!libhal_property_fill_value_from_variant (p, &var_iter))
			goto oom;

		dbus_message_iter_next (&dict_iter);
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);

	return result;

oom:
	fprintf (stderr,
		"%s %d : error allocating memory\n",
		 __FILE__, __LINE__);
		/** @todo FIXME cleanup */
	return NULL;
}

/** Free a property set earlier obtained with libhal_device_get_all_properties().
 *
 *  @param  set                 Property-set to free
 */
void
libhal_free_property_set (LibHalPropertySet * set)
{
	LibHalProperty *p;
	LibHalProperty *q;

	if (set == NULL)
		return;

	for (p = set->properties_head; p != NULL; p = q) {
		free (p->key);
		if (p->type == DBUS_TYPE_STRING)
			free (p->str_value);
		if (p->type == LIBHAL_PROPERTY_TYPE_STRLIST)
			libhal_free_string_array (p->strlist_value);
		q = p->next;
		free (p);
	}
	free (set);
}

/** Get the number of properties in a property set.
 *
 *  @param set                  Property set to consider
 *  @return                     Number of properties in given property set
 */
unsigned int 
libhal_property_set_get_num_elems (LibHalPropertySet *set)
{
	unsigned int num_elems;
	LibHalProperty *p;

	if (set == NULL)
		return 0;
	
	num_elems = 0;
	for (p = set->properties_head; p != NULL; p = p->next)
		num_elems++;

	return num_elems;
}


/** Initialize a property set iterator.
 *
 *  @param  iter                Iterator object
 *  @param  set                 Property set to iterate over
 */
void
libhal_psi_init (LibHalPropertySetIterator * iter, LibHalPropertySet * set)
{
	if (set == NULL)
		return;

	iter->set = set;
	iter->index = 0;
	iter->cur_prop = set->properties_head;
}


/** Determine whether there are more properties to iterate over
 *
 *  @param  iter                Iterator object
 *  @return                     TRUE if there are more properties,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_psi_has_more (LibHalPropertySetIterator * iter)
{
	return iter->index < iter->set->num_properties;
}

/** Advance iterator to next property.
 *
 *  @param  iter                Iterator object
 */
void
libhal_psi_next (LibHalPropertySetIterator * iter)
{
	iter->index++;
	iter->cur_prop = iter->cur_prop->next;
}

/** Get type of property.
 *
 *  @param  iter                Iterator object
 *  @return                     The property type at the iterator's position
 */
LibHalPropertyType
libhal_psi_get_type (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->type;
}

/** Get the key of a property. 
 *
 *  @param  iter                Iterator object
 *  @return                     ASCII nul-terminated string. This pointer is
 *                              only valid until libhal_free_property_set() is
 *                              invoked on the property set this property
 *                              belongs to
 */
char *
libhal_psi_get_key (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->key;
}

/** Get the value of a property of type string. 
 *
 *  @param  iter                Iterator object
 *  @return                     UTF8 nul-terminated string. This pointer is
 *                              only valid until libhal_free_property_set() is
 *                              invoked on the property set this property
 *                              belongs to
 */
char *
libhal_psi_get_string (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->str_value;
}

/** Get the value of a property of type signed integer. 
 *
 *  @param  iter                Iterator object
 *  @return                     Property value (32-bit signed integer)
 */
dbus_int32_t
libhal_psi_get_int (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->int_value;
}

/** Get the value of a property of type unsigned integer. 
 *
 *  @param  iter                Iterator object
 *  @return                     Property value (64-bit unsigned integer)
 */
dbus_uint64_t
libhal_psi_get_uint64 (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->uint64_value;
}

/** Get the value of a property of type double.
 *
 *  @param  iter                Iterator object
 *  @return                     Property value (IEEE754 double precision float)
 */
double
libhal_psi_get_double (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->double_value;
}

/** Get the value of a property of type bool. 
 *
 *  @param  iter                Iterator object
 *  @return                     Property value (bool)
 */
dbus_bool_t
libhal_psi_get_bool (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->bool_value;
}

/** Get the value of a property of type string list. 
 *
 *  @param  iter                Iterator object
 *  @return                     Pointer to array of strings
 */
char **
libhal_psi_get_strlist (LibHalPropertySetIterator * iter)
{
	return iter->cur_prop->strlist_value;
}


#ifndef DOXYGEN_SHOULD_SKIP_THIS
static DBusHandlerResult
filter_func (DBusConnection * connection,
	     DBusMessage * message, void *user_data)
{
	const char *object_path;
	DBusError error;
	LibHalContext *ctx = (LibHalContext *) user_data;

	if (ctx->is_shutdown)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	dbus_error_init (&error);

	object_path = dbus_message_get_path (message);

	/*printf("*** in filter_func, object_path=%s\n", object_path);*/

	if (dbus_message_is_signal (message, "org.freedesktop.Hal.Manager",
				    "DeviceAdded")) {
		char *udi;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &udi,
					   DBUS_TYPE_INVALID)) {
			if (ctx->device_added != NULL) {
				ctx->device_added (ctx, udi);
			}
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	} else if (dbus_message_is_signal (message, "org.freedesktop.Hal.Manager", "DeviceRemoved")) {
		char *udi;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &udi,
					   DBUS_TYPE_INVALID)) {
			if (ctx->device_removed != NULL) {
				ctx->device_removed (ctx, udi);
			}
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	} else if (dbus_message_is_signal (message, "org.freedesktop.Hal.Manager","NewCapability")) {
		char *udi;
		char *capability;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &udi,
					   DBUS_TYPE_STRING, &capability,
					   DBUS_TYPE_INVALID)) {
			if (ctx->device_new_capability != NULL) {
				ctx->device_new_capability (ctx, udi, capability);
			}
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	} else if (dbus_message_is_signal (message, "org.freedesktop.Hal.Device", "Condition")) {
		char *condition_name;
		char *condition_detail;
		if (dbus_message_get_args (message, &error,
					   DBUS_TYPE_STRING, &condition_name,
					   DBUS_TYPE_STRING, &condition_detail,
					   DBUS_TYPE_INVALID)) {
			if (ctx->device_condition != NULL) {
				ctx->device_condition (ctx, object_path, condition_name, condition_detail);
			}
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	} else if (dbus_message_is_signal (message, "org.freedesktop.Hal.Device", "PropertyModified")) {
		if (ctx->device_property_modified != NULL) {
			int i;
			char *key;
			dbus_bool_t removed;
			dbus_bool_t added;
			int num_modifications;
			DBusMessageIter iter;
			DBusMessageIter iter_array;
	
			dbus_message_iter_init (message, &iter);
			dbus_message_iter_get_basic (&iter, &num_modifications);
			dbus_message_iter_next (&iter);

			dbus_message_iter_recurse (&iter, &iter_array);

			for (i = 0; i < num_modifications; i++) {
				DBusMessageIter iter_struct;

				dbus_message_iter_recurse (&iter_array, &iter_struct);

				dbus_message_iter_get_basic (&iter_struct, &key);
				dbus_message_iter_next (&iter_struct);
				dbus_message_iter_get_basic (&iter_struct, &removed);
				dbus_message_iter_next (&iter_struct);
				dbus_message_iter_get_basic (&iter_struct, &added);
				
				ctx->device_property_modified (ctx, 
							       object_path,
							       key, removed,
							       added);
				
				dbus_message_iter_next (&iter_array);
			}
			
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

#endif /* DOXYGEN_SHOULD_SKIP_THIS */

/* for i18n purposes */
static dbus_bool_t libhal_already_initialized_once = FALSE;

#if 0
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
libhal_initialize (const LibHalFunctions * cb_functions,
		dbus_bool_t use_cache)
{
	DBusError error;
	LibHalContext *ctx;

	if (!libhal_already_initialized_once) {
		bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
		bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
		
		libhal_already_initialized_once = TRUE;
	}
	
	ctx = malloc (sizeof (LibHalContext));
	if (ctx == NULL) {
		fprintf (stderr, "%s %d : Cannot allocated %d bytes!\n",
			 __FILE__, __LINE__, sizeof (LibHalContext));
		return NULL;
	}

	ctx->is_initialized = FALSE;
	ctx->is_shutdown = FALSE;

	ctx->cache_enabled = use_cache;

	ctx->functions = cb_functions;
	/* allow caller to pass NULL */
	if (ctx->functions == NULL)
		ctx->functions = &libhal_null_functions;

	/* connect to hald service on the system bus */
	
	ctx->connection = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (ctx->connection == NULL) {
		fprintf (stderr,
			 "%s %d : Error connecting to system bus: %s\n",
			 __FILE__, __LINE__, error.message);
		dbus_error_free (&error);
		return NULL;
	}

	if (ctx->main_loop_integration != NULL) {

		ctx->main_loop_integration (ctx, ctx->connection);
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
libhal_shutdown (LibHalContext *ctx)
{
	DBusError error;

	if (!ctx->is_initialized)
		return 1;

	/* unsubscribe the match rule we added in initialize; this is safe even with multiple
	 * instances of libhal running - see the dbus docs */
	
	dbus_bus_remove_match (ctx->connection,
			       "type='signal',"
			       "interface='org.freedesktop.Hal.Manager',"
			       "sender='org.freedesktop.Hal',"
			       "path='/org/freedesktop/Hal/Manager'", &error);
	if (dbus_error_is_set (&error)) {
		fprintf (stderr, "%s %d : Error removing match rule, error=%s\r\n",
			 __FILE__, __LINE__, error.message);
	}

	/* TODO: remove all other match rules */

	/* set a flag so we don't propagte callbacks from this context anymore */
	ctx->is_shutdown = TRUE;

	/* yikes, it's dangerous to unref the connection since it will terminate the process
	 * because this connection may be shared so we cannot set the exit_on_disconnect flag
	 *
	 * so we don't do that right now 
	 *
	 */
	/*dbus_connection_unref (ctx->connection);*/

	/* we also refuse to free the resources as filter_function may reference these 
	 * 
	 * should free async when our connection goes away.
	 */
	/* free (ctx); */
	return 0;
}
#endif

/** Get all devices in the Global Device List (GDL).
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  num_devices         The number of devices will be stored here
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     An array of device identifiers terminated
 *                              with NULL. It is the responsibility of the
 *                              caller to free with libhal_free_string_array().
 *                              If an error occurs NULL is returned.
 */
char **
libhal_get_all_devices (LibHalContext *ctx, int *num_devices, DBusError *error)
{	
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter_array, reply_iter;
	char **hal_device_names;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);

	*num_devices = 0;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"GetAllDevices");
	if (message == NULL) {
		fprintf (stderr, "%s %d : Could not allocate D-BUS message\n", __FILE__, __LINE__);
		return NULL;
	}

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection, message, -1, &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	/* now analyze reply */
	dbus_message_iter_init (reply, &reply_iter);

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_ARRAY) {
		fprintf (stderr, "%s %d : wrong reply from hald.  Expecting an array.\n", __FILE__, __LINE__);
		return NULL;
	}
	
	dbus_message_iter_recurse (&reply_iter, &iter_array);

	hal_device_names = libhal_get_string_array_from_iter (&iter_array, num_devices);
		      
	dbus_message_unref (reply);
	dbus_message_unref (message);

	return hal_device_names;
}

/** Query a property type of a device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     One of LIBHAL_PROPERTY_TYPE_INT32,
 *                              LIBHAL_PROPERTY_TYPE_UINT64, LIBHAL_PROPERTY_TYPE_DOUBLE,
 *                              LIBHAL_PROPERTY_TYPE_BOOLEAN, LIBHAL_PROPERTY_TYPE_STRING,
 *                              LIBHAL_PROPERTY_TYPE_STRLIST or
 *                              LIBHAL_PROPERTY_TYPE_INVALID if property doesn't exist.
 */
LibHalPropertyType
libhal_device_get_property_type (LibHalContext *ctx, const char *udi, const char *key, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	int type;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, LIBHAL_PROPERTY_TYPE_INVALID); // or return NULL?
	
	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyType");
	if (message == NULL) {
		fprintf (stderr, "%s %d : Couldn't allocate D-BUS message\n", __FILE__, __LINE__);
		return LIBHAL_PROPERTY_TYPE_INVALID;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return LIBHAL_PROPERTY_TYPE_INVALID;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return LIBHAL_PROPERTY_TYPE_INVALID;
	}

	dbus_message_iter_init (reply, &reply_iter);
	dbus_message_iter_get_basic (&reply_iter, &type);

	dbus_message_unref (message);
	dbus_message_unref (reply);

	return type;
}

/** Get the value of a property of type string list. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     Array of pointers to UTF8 nul-terminated
 *                              strings terminated by NULL. The caller is
 *                              responsible for freeing this string
 *                              array with the function
 *                              libhal_free_string_array().
 *                              Returns NULL if the property didn't exist
 *                              or we are OOM
 */
char **
libhal_device_get_property_strlist (LibHalContext *ctx, const char *udi, const char *key, DBusError *error)
{	
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, iter_array, reply_iter;
	char **our_strings;
	DBusError _error;
	
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyStringList");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &reply_iter);

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_ARRAY) {
		fprintf (stderr, "%s %d : wrong reply from hald.  Expecting an array.\n", __FILE__, __LINE__);
		return NULL;
	}
	
	dbus_message_iter_recurse (&reply_iter, &iter_array);

	our_strings = libhal_get_string_array_from_iter (&iter_array, NULL);
		      
	dbus_message_unref (reply);
	dbus_message_unref (message);

	return our_strings;
}

/** Get the value of a property of type string. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     UTF8 nul-terminated string. The caller is
 *                              responsible for freeing this string with the
 *                              function libhal_free_string(). 
 *                              Returns NULL if the property didn't exist
 *                              or we are OOM
 */
char *
libhal_device_get_property_string (LibHalContext *ctx,
				   const char *udi, const char *key, DBusError *error)
{	
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	char *value;
	char *dbus_str;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyString");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_STRING) {
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	}

	dbus_message_iter_get_basic (&reply_iter, &dbus_str);
	value = (char *) ((dbus_str != NULL) ? strdup (dbus_str) : NULL);
	if (value == NULL) {
		fprintf (stderr, "%s %d : error allocating memory\n",
			 __FILE__, __LINE__);
		/** @todo FIXME cleanup */
		return NULL;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Get the value of a property of type integer. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     Property value (32-bit signed integer)
 */
dbus_int32_t
libhal_device_get_property_int (LibHalContext *ctx, 
				const char *udi, const char *key, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	dbus_int32_t value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, -1);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyInteger");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return -1;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return -1;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return -1;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_INT32) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type integer\n", __FILE__, __LINE__, key,
			 udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1;
	}
	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Get the value of a property of type signed integer.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     Property value (64-bit unsigned integer)
 */
dbus_uint64_t
libhal_device_get_property_uint64 (LibHalContext *ctx, 
				   const char *udi, const char *key, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	dbus_uint64_t value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, -1);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyInteger");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return -1;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);
	
	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return -1;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return -1;
	}

	dbus_message_iter_init (reply, &reply_iter);
	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_UINT64) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type integer\n", __FILE__, __LINE__, key,
			 udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1;
	}
	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Get the value of a property of type double.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     Property value (IEEE754 double precision float)
 */
double
libhal_device_get_property_double (LibHalContext *ctx, 
				   const char *udi, const char *key, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	double value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, -1.0);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyDouble");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return -1.0f;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return -1.0f;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return -1.0f;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_DOUBLE) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type double\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return -1.0f;
	}
	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return (double) value;
}

/** Get the value of a property of type bool. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     Property value (boolean)
 */
dbus_bool_t
libhal_device_get_property_bool (LibHalContext *ctx, 
				 const char *udi, const char *key, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	dbus_bool_t value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"GetPropertyBoolean");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	
	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_BOOLEAN) {
		fprintf (stderr,
			 "%s %d : property '%s' for device '%s' is not "
			 "of type bool\n", __FILE__, __LINE__, key, udi);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}
	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}


/* generic helper */
static dbus_bool_t
libhal_device_set_property_helper (LibHalContext *ctx, 
				   const char *udi,
				   const char *key,
				   int type,
				   const char *str_value,
				   dbus_int32_t int_value,
				   dbus_uint64_t uint64_value,
				   double double_value,
				   dbus_bool_t bool_value,
				   DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	char *method_name = NULL;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	/** @todo  sanity check incoming params */
	switch (type) {
	case DBUS_TYPE_INVALID:
		method_name = "RemoveProperty";
		break;
	case DBUS_TYPE_STRING:
		method_name = "SetPropertyString";
		break;
	case DBUS_TYPE_INT32:
	case DBUS_TYPE_UINT64:
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

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	switch (type) {
	case DBUS_TYPE_STRING:
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &str_value);
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &int_value);
		break;
	case DBUS_TYPE_UINT64:
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT64, &uint64_value);
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_DOUBLE, &double_value);
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &bool_value);
		break;
	}

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
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

/** Set a property of type string.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value of the property; a UTF8 string
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the property was set, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_set_property_string (LibHalContext *ctx, 
				   const char *udi,
				   const char *key, 
				   const char *value,
				   DBusError *error)
{
	return libhal_device_set_property_helper (ctx, udi, key,
						  DBUS_TYPE_STRING,
						  value, 0, 0, 0.0f, FALSE, error);
}

/** Set a property of type signed integer.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value of the property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the property was set, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_set_property_int (LibHalContext *ctx, const char *udi,
				const char *key, dbus_int32_t value, DBusError *error)
{
	return libhal_device_set_property_helper (ctx, udi, key,
						  DBUS_TYPE_INT32,
						  NULL, value, 0, 0.0f, FALSE, error);
}

/** Set a property of type unsigned integer.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value of the property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the property was set, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_set_property_uint64 (LibHalContext *ctx, const char *udi,
				   const char *key, dbus_uint64_t value, DBusError *error)
{
	return libhal_device_set_property_helper (ctx, udi, key,
						  DBUS_TYPE_UINT64,
						  NULL, 0, value, 0.0f, FALSE, error);
}

/** Set a property of type double.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value of the property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the property was set, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_set_property_double (LibHalContext *ctx, const char *udi,
				   const char *key, double value, DBusError *error)
{
	return libhal_device_set_property_helper (ctx, udi, key,
						  DBUS_TYPE_DOUBLE,
						  NULL, 0, 0, value, FALSE, error);
}

/** Set a property of type bool.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value of the property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the property was set, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_set_property_bool (LibHalContext *ctx, const char *udi,
				 const char *key, dbus_bool_t value, DBusError *error)
{
	return libhal_device_set_property_helper (ctx, udi, key,
						  DBUS_TYPE_BOOLEAN,
						  NULL, 0, 0, 0.0f, value, error);
}


/** Remove a property.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the property was set, FALSE if
 *                              the device didn't exist
 */
dbus_bool_t
libhal_device_remove_property (LibHalContext *ctx, 
			       const char *udi, const char *key, DBusError *error)
{
	return libhal_device_set_property_helper (ctx, udi, key, DBUS_TYPE_INVALID,	
						  /* DBUS_TYPE_INVALID means remove */
						  NULL, 0, 0, 0.0f, FALSE, error);
}

/** Append to a property of type strlist.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value to append to property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the value was appended, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_property_strlist_append (LibHalContext *ctx, 
				       const char *udi,
				       const char *key,
				       const char *value,
				       DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"StringListAppend");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &value);
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}
	return TRUE;
}

/** Prepend to a property of type strlist.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               Value to prepend to property
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the value was prepended, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_property_strlist_prepend (LibHalContext *ctx, 
					const char *udi,
					const char *key,
					const char *value, 
					DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"StringListPrepend");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &value);
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}
	return TRUE;
}

/** Remove a specified string from a property of type strlist.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  index               Index of string to remove in the strlist
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the string was removed, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_property_strlist_remove_index (LibHalContext *ctx, 
					     const char *udi,
					     const char *key,
					     unsigned int index,
					     DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"StringListRemoveIndex");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &index);
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}
	return TRUE;
}

/** Remove a specified string from a property of type strlist.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property
 *  @param  value               The string to search for and remove
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the string was removed, FALSE if
 *                              the device didn't exist or the property
 *                              had a different type.
 */
dbus_bool_t
libhal_device_property_strlist_remove (LibHalContext *ctx, 
				       const char *udi,
				       const char *key,
				       const char *value, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"StringListRemove");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &value);
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}
	return TRUE;
}


/** Take an advisory lock on the device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  reason_to_lock      A user-presentable reason why the device
 *                              is locked.
 *  @param  reason_why_locked   A pointer to store the reason why the
 *                              device cannot be locked on failure, or
 *                              NULL
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the lock was obtained, FALSE
 *                              otherwise
 */
dbus_bool_t
libhal_device_lock (LibHalContext *ctx,
		    const char *udi,
		    const char *reason_to_lock,
		    char **reason_why_locked, DBusError *error)
{
	DBusMessage *message;
	DBusMessageIter iter;
	DBusMessage *reply;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	if (reason_why_locked != NULL)
		*reason_why_locked = NULL;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						udi,
						"org.freedesktop.Hal.Device",
						"Lock");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &reason_to_lock);

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);

	if (dbus_error_is_set (error)) {
		if (strcmp (error->name,
			    "org.freedesktop.Hal.DeviceAlreadyLocked") == 0) {
			if (reason_why_locked != NULL) {
				*reason_why_locked =
					dbus_malloc0 (strlen (error->message) + 1);
				strcpy (*reason_why_locked, error->message);
			}
		}

		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);

	if (reply == NULL)
		return FALSE;

	dbus_message_unref (reply);

	return TRUE;
}

/** Release an advisory lock on the device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the device was successfully unlocked,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_device_unlock (LibHalContext *ctx,
		      const char *udi, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						udi,
						"org.freedesktop.Hal.Device",
						"Unlock");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);

	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);

	if (reply == NULL)
		return FALSE;

	dbus_message_unref (reply);

	return TRUE;
}


/** Create a new device object which will be hidden from applications
 *  until the CommitToGdl(), ie. libhal_device_commit_to_gdl(), method is called.
 *
 *  Note that the program invoking this method needs to run with super user
 *  privileges.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     Temporary device unique id or NULL if there
 *                              was a problem. This string must be freed
 *                              by the caller.
 */
char *
libhal_new_device (LibHalContext *ctx, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter reply_iter;
	char *value;
	char *dbus_str;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"NewDevice");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return NULL;
	}

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_STRING) {
		fprintf (stderr,
			 "%s %d : expected a string in reply to NewDevice\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return NULL;
	}

	dbus_message_iter_get_basic (&reply_iter, &dbus_str);
	value = (char *) ((dbus_str != NULL) ? strdup (dbus_str) : NULL);
	if (value == NULL) {
		fprintf (stderr, "%s %d : error allocating memory\n",
			 __FILE__, __LINE__);
		/** @todo FIXME cleanup */
		return NULL;
	}

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}


/** When a hidden device has been built using the NewDevice method, ie.
 *  libhal_new_device(), and the org.freedesktop.Hal.Device interface
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
 *  @param  temp_udi            The temporary unique device id as returned by
 *                              libhal_new_device()
 *  @param  udi                 The new unique device id.
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     FALSE if the given unique device id is already
 *                              in use.
 */
dbus_bool_t
libhal_device_commit_to_gdl (LibHalContext *ctx, 
			     const char *temp_udi, const char *udi, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"CommitToGdl");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &temp_udi);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
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
 *  @param  udi                 The new unique device id.
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if the device was removed, FALSE otherwise
 */
dbus_bool_t
libhal_remove_device (LibHalContext *ctx, const char *udi, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"Remove");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
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
 *  @param  udi                 Unique device id.
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if the device exists
 */
dbus_bool_t
libhal_device_exists (LibHalContext *ctx, const char *udi, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	dbus_bool_t value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

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

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &udi);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyze reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_BOOLEAN) {
		fprintf (stderr,
			 "%s %d : expected a bool in reply to DeviceExists\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}

	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Determine if a property on a device exists.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique device id.
 *  @param  key                 Name of the property
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if the device exists, FALSE otherwise
 */
dbus_bool_t
libhal_device_property_exists (LibHalContext *ctx, 
			       const char *udi, const char *key, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	dbus_bool_t value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"PropertyExists");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_iter_init (reply, &reply_iter);

	/* now analyse reply */
	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_BOOLEAN) {
		fprintf (stderr, "%s %d : expected a bool in reply to "
			 "PropertyExists\n", __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}

	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Merge properties from one device to another.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  target_udi          Unique device id of target device to merge to
 *  @param  source_udi          Unique device id of device to merge from
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if the properties were merged, FALSE otherwise
 */
dbus_bool_t
libhal_merge_properties (LibHalContext *ctx, 
			 const char *target_udi, const char *source_udi, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"MergeProperties");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &target_udi);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &source_udi);

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
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
 *  @param  udi1                Unique Device Id for device 1
 *  @param  udi2                Unique Device Id for device 2
 *  @param  property_namespace  Namespace for set of devices, e.g. "usb"
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if all properties starting
 *                              with the given namespace parameter
 *                              from one device is in the other and 
 *                              have the same value.
 */
dbus_bool_t
libhal_device_matches (LibHalContext *ctx, 
			     const char *udi1, const char *udi2,
			     const char *property_namespace, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, reply_iter;
	dbus_bool_t value;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						"/org/freedesktop/Hal/Manager",
						"org.freedesktop.Hal.Manager",
						"DeviceMatches");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, udi1);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, udi2);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, property_namespace);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return FALSE;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &reply_iter);

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_BOOLEAN) {
		fprintf (stderr,
			 "%s %d : expected a bool in reply to DeviceMatches\n",
			 __FILE__, __LINE__);
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}

	dbus_message_iter_get_basic (&reply_iter, &value);

	dbus_message_unref (message);
	dbus_message_unref (reply);
	return value;
}

/** Print a device to stdout; useful for debugging.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  error               Pointer to an initialized dbus error object for
 *                              returning errors or NULL
 *  @return                     TRUE if device's information could be obtained,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_device_print (LibHalContext *ctx, const char *udi, DBusError *error)
{
	int type;
	char *key;
	LibHalPropertySet *pset;
	LibHalPropertySetIterator i;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	printf ("device_id = %s\n", udi);

	if ((pset = libhal_device_get_all_properties (ctx, udi, error)) == NULL)
		return FALSE;

	for (libhal_psi_init (&i, pset); libhal_psi_has_more (&i);
	     libhal_psi_next (&i)) {
		type = libhal_psi_get_type (&i);
		key = libhal_psi_get_key (&i);

		switch (type) {
		case LIBHAL_PROPERTY_TYPE_STRING:
			printf ("    %s = '%s' (string)\n", key,
				libhal_psi_get_string (&i));
			break;
		case LIBHAL_PROPERTY_TYPE_INT32:
			printf ("    %s = %d = 0x%x (int)\n", key,
				libhal_psi_get_int (&i),
				libhal_psi_get_int (&i));
			break;
		case LIBHAL_PROPERTY_TYPE_UINT64:
			printf ("    %s = %lld = 0x%llx (uint64)\n", key,
				libhal_psi_get_uint64 (&i),
				libhal_psi_get_uint64 (&i));
			break;
		case LIBHAL_PROPERTY_TYPE_BOOLEAN:
			printf ("    %s = %s (bool)\n", key,
				(libhal_psi_get_bool (&i) ? "true" :
				 "false"));
			break;
		case LIBHAL_PROPERTY_TYPE_DOUBLE:
			printf ("    %s = %g (double)\n", key,
				libhal_psi_get_double (&i));
			break;
		case LIBHAL_PROPERTY_TYPE_STRLIST:
		{
			unsigned int j;
			char **str_list;

			str_list = libhal_psi_get_strlist (&i);
			printf ("    %s = [", key);
			for (j = 0; str_list[j] != NULL; j++) {
				printf ("'%s'", str_list[j]);
				if (str_list[j+1] != NULL)
					printf (", ");
			}
			printf ("] (string list)\n");

			break;
		}
		default:
			printf ("    *** unknown type for key %s\n", key);
			break;
		}
	}

	libhal_free_property_set (pset);

	return TRUE;
}

/** Find a device in the GDL where a single string property matches a
 *  given value.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  key                 Name of the property
 *  @param  value               Value to match
 *  @param  num_devices         Pointer to store number of devices
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     UDI of devices; free with 
 *                              libhal_free_string_array()
 */
char **
libhal_manager_find_device_string_match (LibHalContext *ctx, 
					 const char *key,
					 const char *value, int *num_devices, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, iter_array, reply_iter;
	char **hal_device_names;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);

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

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &value);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);

	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &reply_iter);

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_ARRAY) {
		fprintf (stderr, "%s %d : wrong reply from hald.  Expecting an array.\n", __FILE__, __LINE__);
		return NULL;
	}
	
	dbus_message_iter_recurse (&reply_iter, &iter_array);

	hal_device_names = libhal_get_string_array_from_iter (&iter_array, num_devices);
		      
	dbus_message_unref (reply);
	dbus_message_unref (message);

	return hal_device_names;
}


/** Assign a capability to a device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability name
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if the capability was added, FALSE if
 *                              the device didn't exist
 */
dbus_bool_t
libhal_device_add_capability (LibHalContext *ctx, 
			      const char *udi, const char *capability, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"AddCapability");
	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &capability);

	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);
	if (dbus_error_is_set (error)) {
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

/** Check if a device has a capability. The result is undefined if the
 *  device doesn't exist.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability name
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if the device has the capability,
 *                              otherwise FALSE
 */
dbus_bool_t
libhal_device_query_capability (LibHalContext *ctx, const char *udi, const char *capability, DBusError *error)
{
	char **caps;
	unsigned int i;
	dbus_bool_t ret;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	ret = FALSE;

	caps = libhal_device_get_property_strlist (ctx, udi, "info.capabilities", error);
	if (caps != NULL) {
		for (i = 0; caps[i] != NULL; i++) {
			if (strcmp (caps[i], capability) == 0) {
				ret = TRUE;
				break;
			}
		}
		libhal_free_string_array (caps);
	}

	return ret;
}

/** Find devices with a given capability. 
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  capability          Capability name
 *  @param  num_devices         Pointer to store number of devices
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     UDI of devices; free with 
 *                              libhal_free_string_array()
 */
char **
libhal_find_device_by_capability (LibHalContext *ctx, 
				  const char *capability, int *num_devices, DBusError *error)
{
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter, iter_array, reply_iter;
	char **hal_device_names;
	DBusError _error;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, NULL);

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

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &capability);

	dbus_error_init (&_error);
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   &_error);
	
	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return NULL;
	}
	if (reply == NULL) {
		dbus_message_unref (message);
		return NULL;
	}
	/* now analyse reply */
	dbus_message_iter_init (reply, &reply_iter);

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_ARRAY) {
		fprintf (stderr, "%s %d : wrong reply from hald.  Expecting an array.\n", __FILE__, __LINE__);
		return NULL;
	}
	
	dbus_message_iter_recurse (&reply_iter, &iter_array);

	hal_device_names = libhal_get_string_array_from_iter (&iter_array, num_devices);
		      
	dbus_message_unref (reply);
	dbus_message_unref (message);

	return hal_device_names;
}

/** Watch all devices, ie. the device_property_changed callback is
 *  invoked when the properties on any device changes.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE only if the operation succeeded
 */
dbus_bool_t
libhal_device_property_watch_all (LibHalContext *ctx, DBusError *error)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	dbus_bus_add_match (ctx->connection,
			    "type='signal',"
			    "interface='org.freedesktop.Hal.Device',"
			    "sender='org.freedesktop.Hal'", error);
	if (dbus_error_is_set (error)) {
		return FALSE;
	}
	return TRUE;
}


/** Add a watch on a device, so the device_property_changed callback is
 *  invoked when the properties on the given device changes.
 *
 *  The application itself is responsible for deleting the watch, using
 *  libhal_device_remove_property_watch, if the device is removed.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE only if the operation succeeded
 */
dbus_bool_t
libhal_device_add_property_watch (LibHalContext *ctx, const char *udi, DBusError *error)
{	
	char buf[512];
	
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	snprintf (buf, 512,
		  "type='signal',"
		  "interface='org.freedesktop.Hal.Device',"
		  "sender='org.freedesktop.Hal'," "path=%s", udi);

	dbus_bus_add_match (ctx->connection, buf, error);
	if (dbus_error_is_set (error)) {
		return FALSE;
	}
	return TRUE;
}


/** Remove a watch on a device.
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE only if the operation succeeded
 */
dbus_bool_t
libhal_device_remove_property_watch (LibHalContext *ctx, const char *udi, DBusError *error)
{	
	char buf[512];
	
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	snprintf (buf, 512,
		  "type='signal',"
		  "interface='org.freedesktop.Hal.Device',"
		  "sender='org.freedesktop.Hal'," "path=%s", udi);

	dbus_bus_remove_match (ctx->connection, buf, error);
	if (dbus_error_is_set (error)) {
		return FALSE;
	}
	return TRUE;
}


/** Create a new LibHalContext
 *
 *  @return a new uninitialized LibHalContext
 */
LibHalContext *
libhal_ctx_new (void)
{
	LibHalContext *ctx;

	if (!libhal_already_initialized_once) {
		bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
		bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
		
		libhal_already_initialized_once = TRUE;
	}

	ctx = calloc (1, sizeof (LibHalContext));
	if (ctx == NULL) {
		fprintf (stderr, 
			 "%s %d : Failed to allocate %d bytes\n",
			 __FILE__, __LINE__, sizeof (LibHalContext));
		return NULL;
	}

	ctx->is_initialized = FALSE;
	ctx->is_shutdown = FALSE;
	ctx->connection = NULL;
	ctx->is_direct = FALSE;

	return ctx;
}

/** Enable or disable caching
 *
 *  @note                       Cache is not actually implemented yet.
 *
 *  @param  ctx                 Context to enable/disable cache for
 *  @param  use_cache           Whether or not to use cache
 *  @return                     TRUE if cache was successfully enabled/disabled,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_cache (LibHalContext *ctx, dbus_bool_t use_cache)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	ctx->cache_enabled = use_cache;
	return TRUE;
}

/** Set DBus connection to use to talk to hald.
 *
 *  @param  ctx                 Context to set connection for
 *  @param  conn                DBus connection to use
 *  @return                     TRUE if connection was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_dbus_connection (LibHalContext *ctx, DBusConnection *conn)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	if (conn == NULL)
		return FALSE;

	ctx->connection = conn;
	return TRUE;
}

/** Initialize the connection to hald
 *
 *  @param  ctx                 Context for connection to hald (connection
 *                              should be set with libhal_ctx_set_dbus_connection)
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if initialization succeeds, FALSE otherwise
 */
dbus_bool_t 
libhal_ctx_init (LibHalContext *ctx, DBusError *error)
{
	DBusError _error;
	dbus_bool_t hald_exists;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	if (ctx->connection == NULL)
		return FALSE;

	dbus_error_init (&_error);
	hald_exists = dbus_bus_name_has_owner (ctx->connection, "org.freedesktop.Hal", &_error);
	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		return FALSE;
	}

	if (!hald_exists) {
		return FALSE;
	}

	
	if (!dbus_connection_add_filter (ctx->connection, filter_func, ctx, NULL)) {
		return FALSE;
	}

	dbus_bus_add_match (ctx->connection, 
			    "type='signal',"
			    "interface='org.freedesktop.Hal.Manager',"
			    "sender='org.freedesktop.Hal',"
			    "path='/org/freedesktop/Hal/Manager'", &_error);
	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		return FALSE;
	}
	ctx->is_initialized = TRUE;
	ctx->is_direct = FALSE;

	return TRUE;
}

/** Create an already initialized connection to hald
 *
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     A pointer to an already initialized LibHalContext
 */
LibHalContext *
libhal_ctx_init_direct (DBusError *error)
{
	char *hald_addr;
	LibHalContext *ctx;
	DBusError _error;

	ctx = libhal_ctx_new ();
	if (ctx == NULL)
		goto out;

	if (((hald_addr = getenv ("HALD_DIRECT_ADDR"))) == NULL) {
		libhal_ctx_free (ctx);
		ctx = NULL;
		goto out;
	}

	dbus_error_init (&_error);
	ctx->connection = dbus_connection_open (hald_addr, &_error);
	dbus_move_error (&_error, error);
	if (error != NULL && dbus_error_is_set (error)) {
		libhal_ctx_free (ctx);
		ctx = NULL;
		goto out;
	}

	ctx->is_initialized = TRUE;
	ctx->is_direct = TRUE;

out:
	return ctx;
}

/** Shut down a connection to hald
 *
 *  @param  ctx                 Context for connection to hald
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if connection successfully shut down,
 *                              FALSE otherwise
 */
dbus_bool_t    
libhal_ctx_shutdown (LibHalContext *ctx, DBusError *error)
{	
	DBusError myerror;

	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);

	if (ctx->is_direct) {
		/* for some reason dbus_connection_set_exit_on_disconnect doesn't work yet so don't unref */
		/*dbus_connection_unref (ctx->connection);*/
	} else {
		dbus_error_init (&myerror);
		dbus_bus_remove_match (ctx->connection, 
				       "type='signal',"
				       "interface='org.freedesktop.Hal.Manager',"
				       "sender='org.freedesktop.Hal',"
				       "path='/org/freedesktop/Hal/Manager'", &myerror);
		if (dbus_error_is_set (&myerror)) {
			fprintf (stderr, "%s %d : Error unsubscribing to signals, error=%s\n", 
				 __FILE__, __LINE__, error->message);
			/** @todo  clean up */
		}

		/* TODO: remove other matches */

		dbus_connection_remove_filter (ctx->connection, filter_func, ctx);
	}

	ctx->is_initialized = FALSE;

	return TRUE;
}

/** Free a LibHalContext resource
 *
 *  @param  ctx                 Pointer to a LibHalContext
 *  @return                     TRUE
 */
dbus_bool_t    
libhal_ctx_free (LibHalContext *ctx)
{
	free (ctx);
	return TRUE;
}

/** Set the callback for when a device is added
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  callback            The function to call when a device is added
 *  @return                     TRUE if callback was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_device_added (LibHalContext *ctx, LibHalDeviceAdded callback)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	ctx->device_added = callback;
	return TRUE;
}

/** Set the callback for when a device is removed
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  callback            The function to call when a device is removed
 *  @return                     TRUE if callback was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_device_removed (LibHalContext *ctx, LibHalDeviceRemoved callback)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	ctx->device_removed = callback;
	return TRUE;
}

/** Set the callback for when a device gains a new capability
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  callback            The function to call when a device gains
 *                              a new capability
 *  @return                     TRUE if callback was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_device_new_capability (LibHalContext *ctx, LibHalDeviceNewCapability callback)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	ctx->device_new_capability = callback;
	return TRUE;
}

/** Set the callback for when a device loses a capability
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  callback            The function to call when a device loses
 *                              a capability
 *  @return                     TRUE if callback was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_device_lost_capability (LibHalContext *ctx, LibHalDeviceLostCapability callback)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	ctx->device_lost_capability = callback;
	return TRUE;
}

/** Set the callback for when a property is modified on a device
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  callback            The function to call when a property is
 *                              modified on a device
 *  @return                     TRUE if callback was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_device_property_modified (LibHalContext *ctx, LibHalDevicePropertyModified callback)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	ctx->device_property_modified = callback;
	return TRUE;
}

/** Set the callback for when a device emits a condition
 *
 *  @param  ctx                 The context for the connection to hald
 *  @param  callback            The function to call when a device emits a condition
 *  @return                     TRUE if callback was successfully set,
 *                              FALSE otherwise
 */
dbus_bool_t
libhal_ctx_set_device_condition (LibHalContext *ctx, LibHalDeviceCondition callback)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	ctx->device_condition = callback;
	return TRUE;
}

/** Get the length of an array of strings
 *
 *  @param  str_array           Array of strings to consider
 *  @return                     Amount of strings in array
 */
unsigned int libhal_string_array_length (char **str_array)
{
	unsigned int i;

	if (str_array == NULL)
		return 0;

	for (i = 0; str_array[i] != NULL; i++)
		;

	return i;
}


dbus_bool_t 
libhal_device_rescan (LibHalContext *ctx, const char *udi, DBusError *error)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	DBusMessage *message;
	DBusMessageIter reply_iter;
	DBusMessage *reply;
	dbus_bool_t result;

	message = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
						"org.freedesktop.Hal.Device",
						"Rescan");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);

	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);

	if (reply == NULL)
		return FALSE;

	dbus_message_iter_init (reply, &reply_iter);
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_BOOLEAN) {
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}
	dbus_message_iter_get_basic (&reply_iter, &result);

	dbus_message_unref (reply);

	return result;
}

dbus_bool_t
libhal_device_reprobe (LibHalContext *ctx, const char *udi, DBusError *error)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	DBusMessage *message;
	DBusMessageIter reply_iter;
	DBusMessage *reply;
	dbus_bool_t result;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						udi,
						"org.freedesktop.Hal.Device",
						"Reprobe");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);

	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);

	if (reply == NULL)
		return FALSE;

	dbus_message_iter_init (reply, &reply_iter);
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_BOOLEAN) {
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}
	dbus_message_iter_get_basic (&reply_iter, &result);

	dbus_message_unref (reply);

	return result;
}

/** Emit a condition from a device
 *
 *  @param  ctx                 Context for connection to hald
 *  @param  udi                 Unique Device Id
 *  @param  condition_name      User-readable name of condition
 *  @param  condition_details   User-readable details of condition
 *  @param  error               Pointer to an initialized dbus error object for 
 *                              returning errors or NULL
 *  @return                     TRUE if condition successfully emitted,
 *                              FALSE otherwise
 */
dbus_bool_t libhal_device_emit_condition (LibHalContext *ctx,
					  const char *udi,
					  const char *condition_name,
					  const char *condition_details,
					  DBusError *error)
{
	LIBHAL_CHECK_LIBHALCONTEXT(ctx, FALSE);
	
	DBusMessage *message;
	DBusMessageIter iter;
	DBusMessageIter reply_iter;
	DBusMessage *reply;
	dbus_bool_t result;

	message = dbus_message_new_method_call ("org.freedesktop.Hal",
						udi,
						"org.freedesktop.Hal.Device",
						"EmitCondition");

	if (message == NULL) {
		fprintf (stderr,
			 "%s %d : Couldn't allocate D-BUS message\n",
			 __FILE__, __LINE__);
		return FALSE;
	}

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &condition_name);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &condition_details);
	
	reply = dbus_connection_send_with_reply_and_block (ctx->connection,
							   message, -1,
							   error);

	if (dbus_error_is_set (error)) {
		dbus_message_unref (message);
		return FALSE;
	}

	dbus_message_unref (message);

	if (reply == NULL)
		return FALSE;

	dbus_message_iter_init (reply, &reply_iter);
	if (dbus_message_iter_get_arg_type (&reply_iter) !=
		   DBUS_TYPE_BOOLEAN) {
		dbus_message_unref (message);
		dbus_message_unref (reply);
		return FALSE;
	}
	dbus_message_iter_get_basic (&reply_iter, &result);

	dbus_message_unref (reply);

	return result;	
}

/** @} */
