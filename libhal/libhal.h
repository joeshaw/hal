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
#endif

/**
 * @addtogroup LibHal
 *
 * @{
 */

#ifndef DOXYGEN_SHOULD_SKIP_THIS
typedef struct LibHalContext_s LibHalContext;
#endif /* DOXYGEN_SHOULD_SKIP_THIS */

/** Type for function in application code that integrates a DBusConnection 
 *  object into it's own mainloop. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalIntegrateDBusIntoMainLoop) (LibHalContext *ctx,
						 DBusConnection *
						 dbus_connection);

/** Type for callback when a device is added.
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceAdded) (LibHalContext *ctx, const char *udi);

/** Type for callback when a device is removed. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceRemoved) (LibHalContext *ctx, const char *udi);

/** Type for callback when a device got a new capability
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability of the device
 */
typedef void (*LibHalNewCapability) (LibHalContext *ctx, 
				     const char *udi,
				     const char *capability);

/** Type for callback when a device lost a capability
 *
 *  @param  udi                 Unique Device Id
 *  @param  capability          Capability of the device
 */
typedef void (*LibHalLostCapability) (LibHalContext *ctx, 
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


/** Big convenience chunk for all callback function pointers. 
 *
 *  Every function pointer can be set to NULL to indicate that the
 *  callback is not requested.
 */
typedef struct LibHalFunctions_s {
	/** This is called when the application needs to integrate the 
	 *  underlying DBusConnection into the main loop
	 */
	LibHalIntegrateDBusIntoMainLoop main_loop_integration;

	/** Device added */
	LibHalDeviceAdded device_added;

	/** Device removed */
	LibHalDeviceRemoved device_removed;

	/** Device got a new capability */
	LibHalNewCapability device_new_capability;

	/** Device got a new capability */
	LibHalLostCapability device_lost_capability;

	/** A property of a device changed  */
	LibHalDevicePropertyModified device_property_modified;

	/** A non-continous event on the device occured  */
	LibHalDeviceCondition device_condition;

} LibHalFunctions;


LibHalContext *hal_initialize (const LibHalFunctions * cb_functions,
			       dbus_bool_t use_cache);

int hal_shutdown (LibHalContext *ctx);

void hal_ctx_set_user_data(LibHalContext *ctx, void *user_data);

void* hal_ctx_get_user_data(LibHalContext *ctx);

char **hal_get_all_devices (LibHalContext *ctx, int *num_devices);
dbus_bool_t hal_device_exists (LibHalContext *ctx, const char *udi);

void hal_device_print (LibHalContext *ctx, const char *udi);

dbus_bool_t hal_device_property_exists (LibHalContext *ctx, 
					const char *udi,
					const char *key);

char *hal_device_get_property_string (LibHalContext *ctx, const char *udi,
				      const char *key);

dbus_int32_t hal_device_get_property_int (LibHalContext *ctx, 
					  const char *udi,
					  const char *key);

double hal_device_get_property_double (LibHalContext *ctx, 
				       const char *udi,
				       const char *key);

dbus_bool_t hal_device_get_property_bool (LibHalContext *ctx, 
					  const char *udi,
					  const char *key);

dbus_bool_t hal_device_set_property_string (LibHalContext *ctx, 
					    const char *udi,
					    const char *key,
					    const char *value);

dbus_bool_t hal_device_set_property_int (LibHalContext *ctx, 
					 const char *udi,
					 const char *key,
					 dbus_int32_t value);

dbus_bool_t hal_device_set_property_double (LibHalContext *ctx, 
					    const char *udi,
					    const char *key,
					    double value);

dbus_bool_t hal_device_set_property_bool (LibHalContext *ctx, 
					  const char *udi,
					  const char *key,
					  dbus_bool_t value);

dbus_bool_t hal_device_remove_property (LibHalContext *ctx, 
					const char *udi,
					const char *key);

int hal_device_get_property_type (LibHalContext *ctx, 
				  const char *udi,
				  const char *key);


#ifndef DOXYGEN_SHOULD_SKIP_THIS
struct LibHalProperty_s;
typedef struct LibHalProperty_s LibHalProperty;

struct LibHalPropertySet_s;
typedef struct LibHalPropertySet_s LibHalPropertySet;
#endif /* DOXYGEN_SHOULD_SKIP_THIS */


LibHalPropertySet *hal_device_get_all_properties (LibHalContext *ctx, 
						  const char *udi);

void hal_free_property_set (LibHalPropertySet * set);

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

void hal_psi_init (LibHalPropertySetIterator * iter,
		   LibHalPropertySet * set);

dbus_bool_t hal_psi_has_more (LibHalPropertySetIterator * iter);
void hal_psi_next (LibHalPropertySetIterator * iter);

int hal_psi_get_type (LibHalPropertySetIterator * iter);

char *hal_psi_get_key (LibHalPropertySetIterator * iter);

char *hal_psi_get_string (LibHalPropertySetIterator * iter);
dbus_int32_t hal_psi_get_int (LibHalPropertySetIterator * iter);
double hal_psi_get_double (LibHalPropertySetIterator * iter);
dbus_bool_t hal_psi_get_bool (LibHalPropertySetIterator * iter);

void hal_free_string_array (char **str_array);
void hal_free_string (char *str);

char *hal_agent_new_device (LibHalContext *ctx);
dbus_bool_t hal_agent_commit_to_gdl (LibHalContext *ctx,
				     const char *temp_udi,
				     const char *udi);
dbus_bool_t hal_agent_remove_device (LibHalContext *ctx, 
				     const char *udi);
dbus_bool_t hal_agent_merge_properties (LibHalContext *ctx,
					const char *target_udi,
					const char *source_udi);

dbus_bool_t hal_agent_device_matches (LibHalContext *ctx,
				      const char *udi1,
				      const char *udi2,
				      const char *namespace);

char **hal_manager_find_device_string_match (LibHalContext *ctx,
					     const char *key,
					     const char *value,
					     int *num_devices);


dbus_bool_t hal_device_add_capability (LibHalContext *ctx,
				       const char *udi,
				       const char *capability);

dbus_bool_t hal_device_query_capability (LibHalContext *ctx,
					 const char *udi,
					 const char *capability);

char **hal_find_device_by_capability (LibHalContext *ctx,
				      const char *capability,
				      int *num_devices);

int hal_device_property_watch_all (LibHalContext *ctx);
int hal_device_add_property_watch (LibHalContext *ctx, 
				   const char *udi);
int hal_device_remove_property_watch (LibHalContext *ctx, 
				      const char *udi);

/** @} */

#if defined(__cplusplus)
}
#endif

#endif /* LIBHAL_H */
