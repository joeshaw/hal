/***************************************************************************
 * CVSID: $Id$
 *
 * access-check.c : Checks whether a D-Bus caller have access
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
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
#include <glib.h>

#include "hald.h"
#include "logger.h"
#include "access-check.h"

/**
 * access_check_caller_is_root_or_hal:
 * @cit: the CITracker object
 * @caller_unique_sysbus_name: The unique system bus connection name (e.g. ":1.43") of the caller
 *
 * Check if the caller has uid 0 (root) or the uid of the hal user.
 *
 * Returns: TRUE iff the caller has uid 0 (root) or the uid of the hal user.
 */
gboolean
access_check_caller_is_root_or_hal (CITracker *cit, const char *caller_unique_sysbus_name)
{
	gboolean ret;
	CICallerInfo *ci;

	ret = FALSE;

	ci = ci_tracker_get_info (cit, caller_unique_sysbus_name);
	if (ci == NULL) {
		goto out;
	}

	HAL_INFO (("uid for caller is %ld", ci_tracker_caller_get_uid (ci)));

	if (ci_tracker_caller_get_uid (ci) != 0 && ci_tracker_caller_get_uid (ci) != geteuid ()) {
		HAL_WARNING (("uid %d is not privileged", ci_tracker_caller_get_uid (ci)));
		goto out;
	}

	ret = TRUE;

out:
	return ret;
}

/**
 * access_check_message_caller_is_root_or_hal:
 * @cit: the CITracker object
 * @message: the message from the caller
 *
 * Check if the sender of the message has uid 0 (root) or the uid of the hal user.
 *
 * Returns: TRUE iff the sender of the message has uid 0 (root) or the uid of the hal user.
 */
gboolean
access_check_message_caller_is_root_or_hal (CITracker *cit, DBusMessage *message)
{
        gboolean ret;
	const char *user_base_svc;

	ret = FALSE;

	user_base_svc = dbus_message_get_sender (message);
	if (user_base_svc == NULL) {
		HAL_WARNING (("Cannot determine base service of caller"));
		goto out;
	}

	ret = access_check_caller_is_root_or_hal (cit, user_base_svc);

out:
	return ret;
}

/**
 * access_check_caller_have_access_to_device:
 * @cit: the CITracker object
 * @device: The device to check for
 * @privilege: the type of access; right now this can be #NULL or
 * "lock"; will be replaced by PolicyKit privileges in the future
 * @caller_unique_sysbus_name: The unique system bus connection
 * name (e.g. ":1.43") of the caller
 *
 * Determine if a given caller should have access to a device. This
 * depends on how the security is set up and may change according to
 * how the system is configured.
 *
 * If the privilege parameter is #NULL, it means "check if the caller
 * can access D-Bus interfaces". If it is "lock" it means "check if
 * the caller can lock an interface on the device".
 *
 * If ConsoleKit is used this function currently will return TRUE when
 * privilege is #NULL if, and only if, the caller is in an active
 * local session. If privilege is "lock" the only difference is that
 * the it will always return TRUE for the root computer device
 * object. This is used to ensure that any caller can always lock the
 * SystemPowerManagement interface cf. the "Locking Guidelines"
 * section of the HAL spec.
 *
 * (TODO: once PolicyKit and multi-seat is properly supported, this
 * result from this function will also depend on what seat the device
 * belongs to and what seat the caller's session belongs to.)
 *
 * If ConsoleKit is not used, this function will just return TRUE; the
 * OS vendor is supposed to have locked down access to HAL through OS
 * specific mechanisms and/or D-Bus security configuration directives.
 *
 * By convention uid 0 and the hal user will have access to all
 * devices.
 *
 * Returns: TRUE iff the caller have access to the device.
 */
gboolean
access_check_caller_have_access_to_device (CITracker *cit, HalDevice *device, const char *privilege, const char *caller_unique_sysbus_name)
#ifdef HAVE_CONKIT
{
        gboolean ret;
        CICallerInfo *ci;

        ret = FALSE;

        ci = ci_tracker_get_info (cit, caller_unique_sysbus_name);
        if (ci == NULL) {
                HAL_ERROR (("Cannot get caller info for %s", caller_unique_sysbus_name));
                goto out;
        }

        if (ci_tracker_caller_get_uid (ci) == 0 ||
            ci_tracker_caller_get_uid (ci) == geteuid ()) {
                ret = TRUE;
                goto out;
        }

        /* must be tracked by ConsoleKit */
        if (ci_tracker_caller_get_ck_session_path (ci) == NULL) {
                goto out;
        }

        /* require caller to be local */
        if (!ci_tracker_caller_is_local (ci))
                goto out;

        /* allow inactive sessions to lock interfaces on root computer device object */
        if (privilege != NULL && 
            strcmp (privilege, "lock") == 0 && 
            strcmp (hal_device_get_udi (device), "/org/freedesktop/Hal/devices/computer") == 0) {
                ret = TRUE;
                goto out;
        }
        
        /* require caller to be in active session */
        if (!ci_tracker_caller_in_active_session (ci))
                goto out;
                
        ret = TRUE;
out:
        return ret;
}
#else /* HAVE_CONKIT */
{
        return TRUE;
}
#endif

/**
 * access_check_caller_locked_out:
 * @cit: the CITracker object
 * @device: The device to check for
 * @caller_unique_sysbus_name: The unique system bus connection name (e.g. ":1.43") of the caller
 * @interface_name: the interface to check for
 *
 * This method determines if a caller is locked out to access a given
 * interface on a given device. A caller A is locked out of an
 * interface IFACE on a device object DEVICE if, and only if 
 *
 * 1. Another caller B is holding a lock on the interface IFACE on
 *    DEVICE and A don't have either a global lock on IFACE or a lock
 *    on IFACE on DEVICE; or 
 *
 * 2. Another caller B is holding the global lock on the interface
 *    IFACE and B has access to DEVICE and and A don't have either a
 *    global lock on IFACE or a lock on IFACE on DEVICE.
 *
 * In other words, a caller A can grab a global lock, but that doesn't
 * mean A can lock other clients out of devices that A doesn't have
 * access to. Specifically a caller is never locked out if he has
 * locked an interface either globally or on the device in
 * question. However, if two clients have a lock on a device, then
 * both can access it. To ensure that everyone else is locked out, a
 * caller needs to use an exclusive lock.
 * 
 * Returns: TRUE iff the caller is locked out
 */
gboolean
access_check_caller_locked_out (CITracker   *cit,
                                HalDevice   *device,
                                const char  *caller_unique_sysbus_name,
                                const char  *interface_name)
{
        int n;
        gboolean ret;
        char *global_lock_name;
        char **holders;
        char **global_holders;
        HalDevice *computer;
        gboolean is_locked;
        gboolean is_locked_by_self;

        global_lock_name = NULL;
        holders = NULL;
        global_holders = NULL;
        ret = TRUE;

	computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	if (computer == NULL)
		computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	if (computer == NULL)
		goto out;

        global_lock_name = g_strdup_printf ("Global.%s", interface_name);

        holders = hal_device_get_lock_holders (device, interface_name);
        global_holders = hal_device_get_lock_holders (computer, global_lock_name);

        /* check if there are other holders than us - these are
         * assumed to have access to the device since they got to hold
         * the lock in the first place. 
         */
        is_locked = FALSE;
        is_locked_by_self = FALSE;
        if (holders != NULL) {
                for (n = 0; holders[n] != NULL; n++) {
                        is_locked = TRUE;
                        if (strcmp (holders[n], caller_unique_sysbus_name) == 0) {
                                is_locked_by_self = TRUE;
                                /* this is good enough; we are holding the lock ourselves */
                                ret = FALSE;
                                goto out;
                        }
                }
        }

        if (global_holders != NULL) {
                for (n = 0; global_holders[n] != NULL; n++) {
                        if (strcmp (global_holders[n], caller_unique_sysbus_name) == 0) {
                                /* we are holding the global lock... */
                                if (access_check_caller_have_access_to_device (cit, device, NULL, global_holders[n])) {
                                        /* only applies if the caller can access the device... */
                                        is_locked_by_self = TRUE;
                                        /* this is good enough; we are holding the lock ourselves */
                                        ret = FALSE;
                                        goto out;
                                }
                        } else {
                                /* Someone else is holding the global lock.. check if that someone
                                 * actually have access to the device...
                                 */
                                if (access_check_caller_have_access_to_device (cit, device, NULL, global_holders[n])) {
                                        /* They certainly do. Mark as locked. */
                                        is_locked = TRUE;
                                }
                        }
                }
        }

        if (is_locked && !is_locked_by_self) {
                /* Yup, there's someone else... can't do it Sally */
                HAL_INFO (("Caller '%s' is locked out of interface '%s' on device '%s' "
                           "because someone else got a lock on the interface on the device",
                           caller_unique_sysbus_name,
                           interface_name,
                           hal_device_get_udi (device)));
                goto out;
        }

        /* done all the checks so we're not locked out */
        ret = FALSE;

out:
        g_strfreev (global_holders);
        g_strfreev (holders);
        g_free (global_lock_name);
        return ret;
}



/**
 * access_check_locked_by_others:
 * @cit: the CITracker object
 * @device: The device to check for
 * @caller_unique_sysbus_name: The unique system bus connection name (e.g. ":1.43") of the caller
 * @interface_name: the interface to check for
 *
 * This method determines other processes than the caller holds a lock
 * on the given device.
 * 
 * Returns: TRUE iff other processes is holding a lock on the device
 */
gboolean
access_check_locked_by_others (CITracker   *cit,
                               HalDevice   *device,
                               const char  *caller_unique_sysbus_name,
                               const char  *interface_name)
{
        int n;
        gboolean ret;
        char *global_lock_name;
        char **holders;
        char **global_holders;
        HalDevice *computer;

        global_lock_name = NULL;
        holders = NULL;
        global_holders = NULL;
        ret = TRUE;

	computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
	if (computer == NULL)
		computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
	if (computer == NULL)
		goto out;

        global_lock_name = g_strdup_printf ("Global.%s", interface_name);

        holders = hal_device_get_lock_holders (device, interface_name);
        global_holders = hal_device_get_lock_holders (computer, global_lock_name);

        if (holders != NULL) {
                for (n = 0; holders[n] != NULL; n++) {
                        if (caller_unique_sysbus_name == NULL || /* <-- callers using direct connection to hald */
                            strcmp (holders[n], caller_unique_sysbus_name) != 0) {
                                /* someone else is holding the lock */
                                goto out;
                        }
                }
        }

        if (global_holders != NULL) {
                for (n = 0; global_holders[n] != NULL; n++) {
                        if (caller_unique_sysbus_name == NULL || /* <-- callers using direct connection to hald */
                            strcmp (global_holders[n], caller_unique_sysbus_name) != 0) {
                                /* someone else is holding the global lock... */
                                if (access_check_caller_have_access_to_device (cit, device, NULL, global_holders[n])) {
                                        /* ... and they can can access the device */
                                        goto out;
                                }
                        }
                }
        }

        /* done all the checks so noone else is locking... */
        ret = FALSE;

out:
        g_strfreev (global_holders);
        g_strfreev (holders);
        g_free (global_lock_name);
        return ret;
}

