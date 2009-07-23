/***************************************************************************
 * CVSID: $Id$
 *
 * ci-tracker.c : Track information about callers
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

#include "logger.h"
#include "ci-tracker.h"

/* ripped from dbus/dbus-marshal-validate.c and adapted */

/**
 * Determine wether the given character is valid as the first character
 * in a bus name.
 */
#define VALID_INITIAL_BUS_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
    ((c) == '_') || ((c) == '-'))


/**
 * Determine wether the given character is valid as a second or later
 * character in a bus name
 */
#define VALID_BUS_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
    ((c) == '_') || ((c) == '-'))

static gboolean
validate_bus_name (const char *name)
{
        int len;
        const char *s;
        const char *end;
        const char *last_dot;
        gboolean ret;

        s = name;
        len = strlen (name);
        end = name + len;
        last_dot = NULL;

        ret = FALSE;

        /* check special cases of first char so it doesn't have to be done
         * in the loop. Note we know len > 0
         */
        if (*s == ':') {
                /* unique name */
                ++s;
                while (s != end) {
                        if (*s == '.') {
                                if (G_UNLIKELY ((s + 1) == end))
                                        goto error;
                                if (G_UNLIKELY (!VALID_BUS_NAME_CHARACTER (*(s + 1))))
                                        goto error;
                                ++s; /* we just validated the next char, so skip two */
                        } else if (G_UNLIKELY (!VALID_BUS_NAME_CHARACTER (*s))) {
                                goto error;
                        }
                        ++s;
                }
                return TRUE;
        } else if (G_UNLIKELY (*s == '.')) /* disallow starting with a . */ {
                goto error;
        } else if (G_UNLIKELY (!VALID_INITIAL_BUS_NAME_CHARACTER (*s))) {
                goto error;
        } else {
                ++s;
        }
        
        while (s != end) {
                if (*s == '.') {
                        if (G_UNLIKELY ((s + 1) == end))
                                goto error;
                        else if (G_UNLIKELY (!VALID_INITIAL_BUS_NAME_CHARACTER (*(s + 1))))
                                goto error;
                        last_dot = s;
                        ++s; /* we just validated the next char, so skip two */
                } else if (G_UNLIKELY (!VALID_BUS_NAME_CHARACTER (*s))) {
                        goto error;
                }
                ++s;
        }
        
        if (G_UNLIKELY (last_dot == NULL))
                goto error;

        ret = TRUE;

error:
        if (!ret)
                HAL_INFO (("name '%s' did not validate", name));
        return ret;
}


struct CITracker_s {
        GHashTable *connection_name_to_caller_info;
        DBusConnection *dbus_connection;
};


struct CICallerInfo_s {
	unsigned long  uid;           /* uid of caller */
#ifdef HAVE_CONKIT
	pid_t  pid;                   /* process ID of caller */
	gboolean in_active_session;   /* caller is in an active session */
        gboolean is_local;            /* session is on a local seat */
	char *session_objpath;        /* obj path of ConsoleKit session */
        char *selinux_context;        /* SELinux security context */
#endif
	char *system_bus_unique_name; /* unique name of caller on the system bus */
};

static CICallerInfo *
caller_info_new (void)
{
	CICallerInfo *ci;
	ci = g_new0 (CICallerInfo, 1);
	return ci;
}

static void 
caller_info_free (CICallerInfo *ci)
{
#ifdef HAVE_CONKIT
	g_free (ci->session_objpath);
        g_free (ci->selinux_context);
#endif
	g_free (ci->system_bus_unique_name);
	g_free (ci);
}


CITracker *
ci_tracker_new (void)
{
        CITracker *cit;

        cit = g_new0 (CITracker, 1);
        return cit;
}

void
ci_tracker_set_system_bus_connection (CITracker *cit, DBusConnection *system_bus_connection)
{
        cit->dbus_connection = system_bus_connection;
}


void
ci_tracker_init (CITracker *cit)
{
	cit->connection_name_to_caller_info = g_hash_table_new_full (g_str_hash,
                                                                     g_str_equal,
                                                                     NULL, /* a pointer to a CICallerInfo object */
                                                                     (GFreeFunc) caller_info_free);
}

void 
ci_tracker_name_owner_changed (CITracker *cit,
                               const char *name, 
                               const char *old_service_name, 
                               const char *new_service_name)
{
	if (strlen (old_service_name) > 0) {
		CICallerInfo *caller_info;

		/* evict CICallerInfo from cache */
		caller_info = (CICallerInfo *) g_hash_table_lookup (cit->connection_name_to_caller_info, 
                                                                    old_service_name);
		if (caller_info != NULL) {
			g_hash_table_remove (cit->connection_name_to_caller_info, old_service_name);
			HAL_INFO (("Removing CICallerInfo object for %s", old_service_name));
		}
	}
}


#ifdef HAVE_CONKIT

static void
ci_tracker_set_active (gpointer key, gpointer value, gpointer user_data)
{
	CICallerInfo *ci = (CICallerInfo*) value;
	char *session_objpath = (char *) user_data;

	if (ci->session_objpath != NULL && strcmp (ci->session_objpath, session_objpath) == 0) {
		ci->in_active_session = TRUE;
		HAL_INFO (("Set in_active_session=TRUE in CICallerInfo for %s @ %s (uid %d, pid %d)",
			   ci->system_bus_unique_name, ci->session_objpath, ci->uid, ci->pid));
	}
}

static void
ci_tracker_clear_active (gpointer key, gpointer value, gpointer user_data)
{
	CICallerInfo *ci = (CICallerInfo*) value;
	char *session_objpath = (char *) user_data;

	if (ci->session_objpath != NULL && strcmp (ci->session_objpath, session_objpath) == 0) {
		ci->in_active_session = FALSE;
		HAL_INFO (("Set in_active_session=FALSE in CICallerInfo for %s @ %s (uid %d, pid %d)",
			   ci->system_bus_unique_name, ci->session_objpath, ci->uid, ci->pid));
	}
}

void 
ci_tracker_active_changed (CITracker *cit, const char *session_objpath, gboolean is_active)
{
	if (is_active) {
		g_hash_table_foreach (cit->connection_name_to_caller_info, 
				      ci_tracker_set_active, (gpointer) session_objpath);
	} else {
		g_hash_table_foreach (cit->connection_name_to_caller_info, 
				      ci_tracker_clear_active, (gpointer) session_objpath);
	}
}
#endif /* HAVE_CONKIT */

CICallerInfo *
ci_tracker_get_info (CITracker *cit, const char *system_bus_unique_name)
{
	CICallerInfo *ci;
	DBusError error;
#ifdef HAVE_CONKIT
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter sub_iter;
	char *dbus_session_name;
        char *str;
        int num_elems;
#endif /* HAVE_CONKIT */
	
	ci = NULL;
	
	if (system_bus_unique_name == NULL)
		goto error;

        if (!validate_bus_name (system_bus_unique_name))
                goto error;

	/*HAL_INFO (("========================="));
	  HAL_INFO (("Looking up CICallerInfo for system_bus_unique_name = %s", system_bus_unique_name));*/

	ci = g_hash_table_lookup (cit->connection_name_to_caller_info,
				  system_bus_unique_name);
	if (ci != NULL) {
		/*HAL_INFO (("(using cached information)"));*/
		goto got_caller_info;
	}
	/*HAL_INFO (("(retrieving info from system bus and ConsoleKit)"));*/
	
	ci = caller_info_new ();
	ci->system_bus_unique_name = g_strdup (system_bus_unique_name);

	dbus_error_init (&error);
	ci->uid = dbus_bus_get_unix_user (cit->dbus_connection, system_bus_unique_name, &error);
	if (ci->uid == ((unsigned long) -1) || dbus_error_is_set (&error)) {
		HAL_WARNING (("Could not get uid for connection: %s %s", error.name, error.message));
		dbus_error_free (&error);
		goto error;
	}

#ifdef HAVE_CONKIT
	message = dbus_message_new_method_call ("org.freedesktop.DBus", 
						"/org/freedesktop/DBus/Bus",
						"org.freedesktop.DBus",
						"GetConnectionUnixProcessID");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &system_bus_unique_name);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (cit->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_WARNING (("Error doing GetConnectionUnixProcessID on Bus: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		dbus_error_free (&error);
		goto error;
	}
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_get_basic (&iter, &ci->pid);
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.DBus", 
						"/org/freedesktop/DBus/Bus",
						"org.freedesktop.DBus",
						"GetConnectionSELinuxSecurityContext");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &system_bus_unique_name);
	reply = dbus_connection_send_with_reply_and_block (cit->dbus_connection, message, -1, &error);
        /* SELinux might not be enabled */
        if (dbus_error_is_set (&error) && 
            strcmp (error.name, "org.freedesktop.DBus.Error.SELinuxSecurityContextUnknown") == 0) {
                dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
                dbus_error_init (&error);
        } else if (reply == NULL || dbus_error_is_set (&error)) {
                g_warning ("Error doing GetConnectionSELinuxSecurityContext on Bus: %s: %s", error.name, error.message);
                dbus_message_unref (message);
                if (reply != NULL)
                        dbus_message_unref (reply);
		dbus_error_free (&error);
                goto error;
        } else {
                /* TODO: verify signature */
                dbus_message_iter_init (reply, &iter);
                dbus_message_iter_recurse (&iter, &sub_iter);
                dbus_message_iter_get_fixed_array (&sub_iter, (void *) &str, &num_elems);
                if (str != NULL && num_elems > 0)
                        ci->selinux_context = g_strndup (str, num_elems);
                dbus_message_unref (message);
                dbus_message_unref (reply);
        }


	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						"/org/freedesktop/ConsoleKit/Manager",
						"org.freedesktop.ConsoleKit.Manager",
						"GetSessionForUnixProcess");
	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &ci->pid);
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (cit->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_WARNING (("Error doing GetSessionForUnixProcess on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		/* OK, this is not a catastrophe; just means the caller is not a member of any session.. */
		dbus_error_free (&error);
		goto store_caller_info;
	}
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_get_basic (&iter, &dbus_session_name);
	ci->session_objpath = g_strdup (dbus_session_name);
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						ci->session_objpath,
						"org.freedesktop.ConsoleKit.Session",
						"IsActive");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (cit->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_WARNING (("Error doing IsActive on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		dbus_error_free (&error);
		goto error;
	}
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_get_basic (&iter, &ci->in_active_session);
	dbus_message_unref (message);
	dbus_message_unref (reply);


	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						ci->session_objpath,
						"org.freedesktop.ConsoleKit.Session",
						"IsLocal");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (cit->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_WARNING (("Error doing IsLocal on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		dbus_error_free (&error);
		goto error;
	}
	dbus_message_iter_init (reply, &iter);
	dbus_message_iter_get_basic (&iter, &ci->is_local);
	dbus_message_unref (message);
	dbus_message_unref (reply);
	
store_caller_info:
#endif /* HAVE_CONKIT */

	g_hash_table_insert (cit->connection_name_to_caller_info,
			     ci->system_bus_unique_name,
			     ci);

got_caller_info:
	/*HAL_INFO (("system_bus_unique_name = %s", ci->system_bus_unique_name));
	HAL_INFO (("uid                    = %d", ci->uid));
	HAL_INFO (("pid                    = %d", ci->pid));
	if (ci->session_objpath != NULL) {
		HAL_INFO (("session_objpath        = %s", ci->session_objpath));
		HAL_INFO (("in_active_session      = %d", ci->in_active_session));
	} else {
		HAL_INFO (("  (Caller is not in any session)"));
	}
	HAL_INFO (("  (keeping CICallerInfo for %d connections)", 
                   g_hash_table_size (cit->connection_name_to_caller_info)));
	HAL_INFO (("========================="));*/
	return ci;

error:
	/*HAL_INFO (("========================="));*/
	if (ci != NULL)
		caller_info_free (ci);
	return NULL;
}

uid_t
ci_tracker_caller_get_uid (CICallerInfo *ci)
{
        return ci->uid;
}

const char *
ci_tracker_caller_get_sysbus_unique_name (CICallerInfo *ci)
{
        return ci->system_bus_unique_name;
}

#ifdef HAVE_CONKIT
pid_t 
ci_tracker_caller_get_pid (CICallerInfo *ci)
{
        return ci->pid;
}

gboolean
ci_tracker_caller_is_local (CICallerInfo *ci)
{
        return ci->is_local;
}

gboolean
ci_tracker_caller_in_active_session (CICallerInfo *ci)
{
        return ci->in_active_session;
}

const char *
ci_tracker_caller_get_ck_session_path (CICallerInfo *ci)
{
        return ci->session_objpath;
}

const char *
ci_tracker_caller_get_selinux_context (CICallerInfo *ci)
{
        return ci->selinux_context;
}

#endif
