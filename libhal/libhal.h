/***************************************************************************
 * CVSID: $Id$
 *
 * libhal.h : HAL daemon C convenience library headers
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

#ifndef LIBHAL_H
#define LIBHAL_H

#include <dbus/dbus.h>

#if defined(__cplusplus)
extern "C" {
#if 0
} /* shut up emacs indenting */
#endif
#endif

/**
 * @addtogroup LibHal
 *
 * @{
 */

/** Possible types for properties on hal device objects */
typedef enum {
        /** Used to report error condition */
	LIBHAL_PROPERTY_TYPE_NIL     =    DBUS_TYPE_NIL,

	/** Type for 32-bit signed integer property */
	LIBHAL_PROPERTY_TYPE_INT32   =    DBUS_TYPE_INT32,

	/** Type for 64-bit unsigned integer property */
	LIBHAL_PROPERTY_TYPE_UINT64  =    DBUS_TYPE_UINT64,

	/** Type for double precision floating point property */
	LIBHAL_PROPERTY_TYPE_DOUBLE  =    DBUS_TYPE_DOUBLE,

	/** Type for boolean property */
	LIBHAL_PROPERTY_TYPE_BOOLEAN =    DBUS_TYPE_BOOLEAN,

	/** Type for UTF-8 string property */
	LIBHAL_PROPERTY_TYPE_STRING  =    DBUS_TYPE_STRING,

	/** Type for list of UTF-8 strings property */
	LIBHAL_PROPERTY_TYPE_STRLIST =     ((int) (DBUS_TYPE_STRING<<8)+('l'))
} LibHalPropertyType;

#ifndef DOXYGEN_SHOULD_SKIP_THIS
typedef struct LibHalContext_s LibHalContext;
#endif /* DOXYGEN_SHOULD_SKIP_THIS */

/** Type for function in application code that integrates a DBusConnection 
 *  object into it's own mainloop. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalIntegrateDBusIntoMainLoop) (LibHalContext *ctx,
						 DBusConnection *dbus_connection);

/** Type for callback when a device is added.
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceAdded) (LibHalContext *ctx, 
				   const char *udi);

/** Type for callback when a device is removed. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceRemoved) (LibHalContext *ctx, 
				     const char *udi);

/** Type for callback when a device gains a new capability
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability of the device
 */
typedef void (*LibHalDeviceNewCapability) (LibHalContext *ctx, 
					   const char *udi,
					   const char *capability);

/** Type for callback when a device loses a capability
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability of the device
 */
typedef void (*LibHalDeviceLostCapability) (LibHalContext *ctx, 
					    const char *udi,
					    const char *capability);

/** Type for callback when a property of a device changes. 
 *
 *  @param  udi                 Unique Device Id
 *  @param  key                 Name of the property that has changed
 *  @param  is_removed          Property removed
 *  @param  is_added            Property added
 */
typedef void (*LibHalDevicePropertyModified) (LibHalContext *ctx,
					      const char *udi,
					      const char *key,
					      dbus_bool_t is_removed,
					      dbus_bool_t is_added);

/** Type for callback when a non-continuos condition occurs on a device
 *
 *  @param  udi                 Unique Device Id
 *  @param  condition_name      Name of the condition, e.g. 
 *                              ProcessorOverheating. Consult the HAL spec
 *                              for possible conditions
 *  @param  message             D-BUS message with variable parameters
 *                              depending on condition
 */
typedef void (*LibHalDeviceCondition) (LibHalContext *ctx,
				       const char *udi,
				       const char *condition_name,
				       DBusMessage *message);


LibHalContext *libhal_ctx_new                          (void);
dbus_bool_t    libhal_ctx_set_cache                    (LibHalContext *ctx, dbus_bool_t use_cache);
dbus_bool_t    libhal_ctx_set_dbus_connection          (LibHalContext *ctx, DBusConnection *conn);
dbus_bool_t    libhal_ctx_set_user_data                (LibHalContext *ctx, void *user_data);
void*          libhal_ctx_get_user_data                (LibHalContext *ctx);
dbus_bool_t    libhal_ctx_set_device_added             (LibHalContext *ctx, LibHalDeviceAdded callback);
dbus_bool_t    libhal_ctx_set_device_removed           (LibHalContext *ctx, LibHalDeviceRemoved callback);
dbus_bool_t    libhal_ctx_set_device_new_capability    (LibHalContext *ctx, LibHalDeviceNewCapability callback);
dbus_bool_t    libhal_ctx_set_device_lost_capability   (LibHalContext *ctx, LibHalDeviceLostCapability callback);
dbus_bool_t    libhal_ctx_set_device_property_modified (LibHalContext *ctx, LibHalDevicePropertyModified callback);
dbus_bool_t    libhal_ctx_set_device_condition         (LibHalContext *ctx, LibHalDeviceCondition callback);
dbus_bool_t    libhal_ctx_init                         (LibHalContext *ctx, DBusError *error);
dbus_bool_t    libhal_ctx_shutdown                     (LibHalContext *ctx, DBusError *error);
dbus_bool_t    libhal_ctx_free                         (LibHalContext *ctx);


char        **libhal_get_all_devices (LibHalContext *ctx, int *num_devices, DBusError *error);
dbus_bool_t   libhal_device_exists   (LibHalContext *ctx, const char *udi,  DBusError *error);
dbus_bool_t   libhal_device_print    (LibHalContext *ctx, const char *udi,  DBusError *error);



dbus_bool_t libhal_device_property_exists (LibHalContext *ctx, 
					   const char *udi,
					   const char *key,
					   DBusError *error);


char *libhal_device_get_property_string (LibHalContext *ctx, 
					 const char *udi,
					 const char *key,
					 DBusError *error);

dbus_int32_t libhal_device_get_property_int (LibHalContext *ctx, 
					     const char *udi,
					     const char *key,
					     DBusError *error);

dbus_uint64_t libhal_device_get_property_uint64 (LibHalContext *ctx, 
						 const char *udi,
						 const char *key,
						 DBusError *error);

double libhal_device_get_property_double (LibHalContext *ctx, 
					  const char *udi,
					  const char *key,
					  DBusError *error);

dbus_bool_t libhal_device_get_property_bool (LibHalContext *ctx, 
					     const char *udi,
					     const char *key,
					     DBusError *error);

char **libhal_device_get_property_strlist (LibHalContext *ctx, 
					   const char *udi, 
					   const char *key,
					   DBusError *error);



dbus_bool_t libhal_device_set_property_string (LibHalContext *ctx, 
					       const char *udi,
					       const char *key,
					       const char *value,
					       DBusError *error);

dbus_bool_t libhal_device_set_property_int (LibHalContext *ctx, 
					    const char *udi,
					    const char *key,
					    dbus_int32_t value,
					    DBusError *error);

dbus_bool_t libhal_device_set_property_uint64 (LibHalContext *ctx, 
					       const char *udi,
					       const char *key,
					       dbus_uint64_t value,
					       DBusError *error);

dbus_bool_t libhal_device_set_property_double (LibHalContext *ctx, 
					       const char *udi,
					       const char *key,
					       double value,
					       DBusError *error);

dbus_bool_t libhal_device_set_property_bool (LibHalContext *ctx, 
					     const char *udi,
					     const char *key,
					     dbus_bool_t value,
					     DBusError *error);


dbus_bool_t libhal_device_property_strlist_append (LibHalContext *ctx, 
						   const char *udi,
						   const char *key,
						   const char *value,
						   DBusError *error);

dbus_bool_t libhal_device_property_strlist_prepend (LibHalContext *ctx, 
						    const char *udi,
						    const char *key,
						    const char *value,
						    DBusError *error);

dbus_bool_t libhal_device_property_strlist_remove_index (LibHalContext *ctx, 
							 const char *udi,
							 const char *key,
							 unsigned int index,
							 DBusError *error);

dbus_bool_t libhal_device_property_strlist_remove (LibHalContext *ctx, 
						   const char *udi,
						   const char *key,
						   const char *value,
						   DBusError *error);



dbus_bool_t libhal_device_remove_property (LibHalContext *ctx, 
					   const char *udi,
					   const char *key,
					   DBusError *error);

LibHalPropertyType libhal_device_get_property_type (LibHalContext *ctx, 
						    const char *udi,
						    const char *key,
						    DBusError *error);


#ifndef DOXYGEN_SHOULD_SKIP_THIS
struct LibHalProperty_s;
typedef struct LibHalProperty_s LibHalProperty;

struct LibHalPropertySet_s;
typedef struct LibHalPropertySet_s LibHalPropertySet;
#endif /* DOXYGEN_SHOULD_SKIP_THIS */


LibHalPropertySet *libhal_device_get_all_properties (LibHalContext *ctx, 
						     const char *udi,
						     DBusError *error);

void libhal_free_property_set (LibHalPropertySet *set);

/** Iterator for inspecting all properties */
struct LibHalPropertySetIterator_s {
	LibHalPropertySet *set;    /**< Property set we are iterating over */
	unsigned int index;        /**< Index into current element */
	LibHalProperty *cur_prop;  /**< Current property being visited */
	void *reservered0;         /**< Reserved for future use */
	void *reservered1;         /**< Reserved for future use */
};

#ifndef DOXYGEN_SHOULD_SKIP_THIS
typedef struct LibHalPropertySetIterator_s LibHalPropertySetIterator;
#endif /* DOXYGEN_SHOULD_SKIP_THIS */

void libhal_psi_init (LibHalPropertySetIterator *iter, LibHalPropertySet *set);

dbus_bool_t libhal_psi_has_more (LibHalPropertySetIterator *iter);
void libhal_psi_next (LibHalPropertySetIterator *iter);

LibHalPropertyType libhal_psi_get_type (LibHalPropertySetIterator *iter);

char *libhal_psi_get_key (LibHalPropertySetIterator *iter);

char *libhal_psi_get_string (LibHalPropertySetIterator *iter);
dbus_int32_t libhal_psi_get_int (LibHalPropertySetIterator *iter);
dbus_uint64_t libhal_psi_get_uint64 (LibHalPropertySetIterator *iter);
double libhal_psi_get_double (LibHalPropertySetIterator *iter);
dbus_bool_t libhal_psi_get_bool (LibHalPropertySetIterator *iter);
char **libhal_psi_get_strlist (LibHalPropertySetIterator *iter);

unsigned int libhal_string_array_length (char **str_array);

void libhal_free_string_array (char **str_array);
void libhal_free_string (char *str);


char *libhal_agent_new_device (LibHalContext *ctx, DBusError *error);

dbus_bool_t libhal_agent_commit_to_gdl (LibHalContext *ctx,
					const char *temp_udi,
					const char *udi,
					DBusError *error);

dbus_bool_t libhal_agent_remove_device (LibHalContext *ctx, 
					const char *udi,
					DBusError *error);

dbus_bool_t libhal_agent_merge_properties (LibHalContext *ctx,
					   const char *target_udi,
					   const char *source_udi,
					   DBusError *error);

dbus_bool_t libhal_agent_device_matches (LibHalContext *ctx,
					 const char *udi1,
					 const char *udi2,
					 const char *property_namespace,
					 DBusError *error);

char **libhal_manager_find_device_string_match (LibHalContext *ctx,
						const char *key,
						const char *value,
						int *num_devices,
						DBusError *error);


dbus_bool_t libhal_device_add_capability (LibHalContext *ctx,
					  const char *udi,
					  const char *capability,
					  DBusError *error);

dbus_bool_t libhal_device_query_capability (LibHalContext *ctx,
					    const char *udi,
					    const char *capability,
					    DBusError *error);

char **libhal_find_device_by_capability (LibHalContext *ctx,
					 const char *capability,
					 int *num_devices,
					 DBusError *error);

dbus_bool_t libhal_device_property_watch_all (LibHalContext *ctx,
					      DBusError *error);
dbus_bool_t libhal_device_add_property_watch (LibHalContext *ctx, 
					      const char *udi,
					      DBusError *error);
dbus_bool_t libhal_device_remove_property_watch (LibHalContext *ctx, 
						 const char *udi,
						 DBusError *error);

dbus_bool_t libhal_device_lock (LibHalContext *ctx,
				const char *udi,
				const char *reason_to_lock,
				char **reason_why_locked,
				DBusError *error);

dbus_bool_t libhal_device_unlock (LibHalContext *ctx,
				  const char *udi,
				  DBusError *error);

/** @} */

#if defined(__cplusplus)
}
#endif

#endif /* LIBHAL_H */
