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
 * @defgroup Common Common HAL constants
 * @brief Common HAL constants and defines
 * @{
 */

/** The device needs a device information file */
#define HAL_STATE_NEED_DEVICE_INFO  0

/** The device is currently booting */
#define HAL_STATE_BOOTING           1

/** The device requires more information in order to be operational */
#define HAL_STATE_REQ_USER          2

/** The device experienced an error booting */
#define HAL_STATE_ERROR             3

/** The device is enabled and operational */
#define HAL_STATE_ENABLED           4

/** The device is shutting down */
#define HAL_STATE_SHUTDOWN          5

/** The device is disabled */
#define HAL_STATE_DISABLED          6

/** The device is currently unplugged */
#define HAL_STATE_UNPLUGGED         7

/** @} */

/**
 * @addtogroup LibHal
 *
 * @{
 */

/** Type for function in application code that integrates a #DBusConnection 
 *  object into it's own mainloop. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalIntegrateDBusIntoMainLoop)(DBusConnection* dbus_connection);

/** Type for callback when a device is added.
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceAdded)(const char* udi);

/** Type for callback when a device is removed. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceRemoved)(const char* udi);

/** Type for callback when a device is booting. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceBooting)(const char* udi);

/** Type for callback when a device is shutting down. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceShuttingDown)(const char* udi);

/** Type for callback when a device is disabled. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceDisabled)(const char* udi);

/** Type for callback when a device needs a device info file. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceNeedDeviceInfo)(const char* udi);

/** Type for callback when a device failed to boot due to a
 *  driver problem. 
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceBootError)(const char* udi);

/** Type for callback when a device is enabled
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceEnabled)(const char* udi);

/** Type for callback when a device needs user assistance to function properly
 *
 *  @param  udi                 Unique Device Id
 */
typedef void (*LibHalDeviceRequireUser)(const char* udi);


/** Type for callback when a property of a device changes. 
 *
 *  The device object is not updated when the callee returns, so the 
 *  application can inspect the old value through 
 *  #hal_device_get_property_string() or other functions.
 *
 *  @param  udi                 Unique Device Id
 *
 *  @param  key                 Name of the property that has changed
 *
 *  @param  value               Value of the named property that has changed.
 *                              If this is #NULL it means that the property
 *                              has been removed
 */
typedef void (*LibHalDevicePropertyChanged)(const char* udi, 
                                            const char* key,
                                            dbus_bool_t is_removed);

/** Big convenience chunk for all callback function pointers. 
 *
 *  Every function pointer can be set to #NULL to indicate that the
 *  callback is not requested.
 */
typedef struct LibHalFunctions_s
{
    /** This is called when the application needs to integrate the underlying 
     *  #DBusConnection into the main loop
     */
    LibHalIntegrateDBusIntoMainLoop    main_loop_integration;
    /** A property of a device changed  */
    LibHalDevicePropertyChanged        device_property_changed;
    /** Device added */
    LibHalDeviceAdded                  device_added;
    /** Device removed */
    LibHalDeviceRemoved                device_removed;
    /** Device is booting */
    LibHalDeviceBooting                device_booting;
    /** Device is shutting down */
    LibHalDeviceShuttingDown           device_shutting_down;
    /** Device is disabled */
    LibHalDeviceDisabled               device_disabled;
    /** Device doesn't have a driver */
    LibHalDeviceNeedDeviceInfo         device_need_device_info;
    /** Device experienced a problem booting */
    LibHalDeviceBootError              device_boot_error;
    /** Device is enabled */
    LibHalDeviceEnabled                device_enabled;
    /** Device requires user intervention to function properly*/
    LibHalDeviceRequireUser            device_req_user;
} LibHalFunctions;

int hal_initialize(const LibHalFunctions* functions);

int hal_shutdown();

char** hal_get_all_devices(int* num_devices);
dbus_bool_t hal_device_exists(const char* udi);

void hal_device_print(const char* udi);

dbus_bool_t hal_device_property_exists(const char* udi, const char* key);

char* hal_device_get_property_string(const char* udi, const char* key);
dbus_int32_t hal_device_get_property_int(const char* udi,const char* key);
double hal_device_get_property_double(const char* udi, const char* key);
dbus_bool_t hal_device_get_property_bool(const char* udi, const char* key);

dbus_bool_t hal_device_set_property_string(const char* udi, 
                                           const char* key, 
                                           const char* value);
dbus_bool_t hal_device_set_property_int(const char* udi, 
                                        const char* key, 
                                        dbus_int32_t value);
dbus_bool_t hal_device_set_property_double(const char* udi, 
                                           const char* key, 
                                           double value);
dbus_bool_t hal_device_set_property_bool(const char* udi, 
                                         const char* key, 
                                         dbus_bool_t value);
dbus_bool_t hal_device_remove_property(const char* udi, const char* key);

int hal_device_get_property_type(const char* udi, const char* key);


dbus_bool_t hal_device_disable(const char* udi);
dbus_bool_t hal_device_enable(const char* udi);

struct LibHalProperty_s;
typedef struct LibHalProperty_s LibHalProperty;

struct LibHalPropertySet_s;
typedef struct LibHalPropertySet_s LibHalPropertySet;


LibHalPropertySet* hal_device_get_all_properties(const char* udi);

void hal_free_property_set(LibHalPropertySet* set);

/** Iterator for inspecting all properties */
struct LibHalPropertySetIterator_s
{
    LibHalPropertySet* set;
    unsigned int index;
    LibHalProperty* cur_prop;
    void* reservered0;
    void* reservered1;
};

typedef struct LibHalPropertySetIterator_s LibHalPropertySetIterator;

void hal_psi_init(LibHalPropertySetIterator* iter, LibHalPropertySet* set);

dbus_bool_t hal_psi_has_more(LibHalPropertySetIterator* iter);
void hal_psi_next(LibHalPropertySetIterator* iter);

int hal_psi_get_type(LibHalPropertySetIterator* iter);

char* hal_psi_get_key(LibHalPropertySetIterator* iter);

char* hal_psi_get_string(LibHalPropertySetIterator* iter);
dbus_int32_t hal_psi_get_int(LibHalPropertySetIterator* iter);
double hal_psi_get_double(LibHalPropertySetIterator* iter);
dbus_bool_t hal_psi_get_bool(LibHalPropertySetIterator* iter);

void hal_free_string_array(char** str_array);
void hal_free_utf8(char* utf8_string);

char* hal_agent_new_device();
dbus_bool_t hal_agent_commit_to_gdl(const char* temp_udi, const char* udi);
dbus_bool_t hal_agent_remove_device(const char* udi);
dbus_bool_t hal_agent_merge_properties(const char* udi, const char* from_udi);

dbus_bool_t hal_agent_device_matches(const char* udi1, const char* udi2, 
                                     const char* namespace);

char** hal_manager_find_device_string_match(const char* key,
                                            const char* value,
                                            int* num_devices);

/** @} */

#if defined(__cplusplus)
}
#endif

#endif /* LIBHAL_H */
