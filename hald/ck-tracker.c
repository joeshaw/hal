/***************************************************************************
 * CVSID: $Id$
 *
 * ck-tracker.c : Track seats and sessions via ConsoleKit
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
#include "ck-tracker.h"

struct CKSeat_s {
	int refcount;
	char *seat_objpath;           /* obj path of ConsoleKit seat */
	GSList *sessions;             /* list of sessions on this seat */
};

struct CKSession_s {
	int refcount;
	char *session_objpath;        /* obj path of ConsoleKit session */
	CKSeat *seat;                 /* seat that session belongs to */
	gboolean is_active;           /* whether the session is active */
	uid_t user;                   /* user id of the user the session belongs to */
	gboolean is_local;            /* whether the session is on a local seat */
	char *hostname;               /* name/address of the host the session is being displayed on */
};

struct CKTracker_s {
	int refcount;
	DBusConnection *dbus_connection;
	void *user_data;
	CKSeatAddedCB seat_added_cb;
	CKSeatRemovedCB seat_removed_cb;
	CKSessionAddedCB session_added_cb;
	CKSessionRemovedCB session_removed_cb;
	CKSessionActiveChangedCB session_active_changed_cb;
	CKServiceDisappearedCB service_disappeared_cb;
	CKServiceAppearedCB service_appeared_cb;
	GSList *seats;
	GSList *sessions;
};

static CKSession *
ck_session_new (const char *session_objpath)
{
	CKSession *session;
	session = g_new0 (CKSession, 1);
	session->refcount = 1;
	session->session_objpath = g_strdup (session_objpath);
	session->seat = NULL;
	session->hostname = NULL;
	session->is_local = FALSE;
	return session;
}

static void 
ck_session_unref (CKSession *session)
{
	session->refcount--;
	if (session->refcount == 0) {
		g_free (session->hostname);
		g_free (session->session_objpath);
		g_free (session);
	}
}

static void 
ck_session_ref (CKSession *session)
{
	session->refcount++;
}

static CKSeat *
ck_seat_new (const char *seat_objpath)
{
	CKSeat *seat;
	seat = g_new0 (CKSeat, 1);
	seat->refcount = 1;
	seat->seat_objpath = g_strdup (seat_objpath);
	seat->sessions = NULL;
	return seat;
}

static void
ck_seat_attach_session (CKSeat *seat, CKSession *session)
{
	ck_session_ref (session);
	session->seat = seat;
	seat->sessions = g_slist_append (seat->sessions, session);
}

static void
ck_seat_detach_session (CKSeat *seat, CKSession *session)
{
	seat->sessions = g_slist_remove (seat->sessions, session);
	session->seat = NULL;
	ck_session_unref (session);
}

static void 
ck_seat_unref (CKSeat *seat)
{
	seat->refcount--;
	if (seat->refcount == 0) {
		GSList *i;

		/* effictively detach all sessions without manually removing each element */
		for (i = seat->sessions; i != NULL; i = g_slist_next (i)) {
			CKSession *session = (CKSession *) i->data;
			session->seat = NULL;
			ck_session_unref (session);
		}
		g_slist_free (seat->sessions);

		g_free (seat->seat_objpath);	
		g_free (seat);
	}
}

#if 0
static void 
ck_seat_ref (CKSeat *seat)
{
	seat->refcount++;
}
#endif

static gboolean
ck_seat_get_info (CKTracker *tracker, CKSeat *seat)
{
	/* currently no interesting info from CK to get */
	return TRUE;
}

static gboolean
ck_session_get_info (CKTracker *tracker, CKSession *session)
{
	gboolean ret;
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	char *hostname;

	ret = FALSE;

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						session->session_objpath,
						"org.freedesktop.ConsoleKit.Session",
						"IsActive");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (tracker->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_ERROR (("Error doing Session.IsActive on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto error;
	}
	if (!dbus_message_get_args (reply, NULL,
				    DBUS_TYPE_BOOLEAN, &(session->is_active),
				    DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Invalid IsActive reply from CK"));
		goto error;
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						session->session_objpath,
						"org.freedesktop.ConsoleKit.Session",
						"IsLocal");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (tracker->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_ERROR (("Error doing Session.IsLocal on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto error;
	}
	if (!dbus_message_get_args (reply, NULL,
				    DBUS_TYPE_BOOLEAN, &(session->is_local),
				    DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Invalid IsLocal reply from CK"));
		goto error;
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						session->session_objpath,
						"org.freedesktop.ConsoleKit.Session",
						"GetRemoteHostName");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (tracker->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_ERROR (("Error doing Session.GetRemoteHostName on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto error;
	}
	if (!dbus_message_get_args (reply, NULL,
				    DBUS_TYPE_STRING, &hostname,
				    DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Invalid GetRemoteHostName reply from CK"));
		goto error;
	}
	session->hostname = g_strdup (hostname);
	dbus_message_unref (message);
	dbus_message_unref (reply);

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						session->session_objpath,
						"org.freedesktop.ConsoleKit.Session",
						"GetUnixUser");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (tracker->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_ERROR (("Error doing Session.GetUnixUser on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto error;
	}
	if (!dbus_message_get_args (reply, NULL,
#ifdef HAVE_CK_0_3
				    DBUS_TYPE_UINT32, &(session->user),
#else
				    DBUS_TYPE_INT32, &(session->user),
#endif
				    DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Invalid GetUnixUser reply from CK"));
		goto error;
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

	HAL_INFO (("Got active state (%s) and uid %d on session '%s'",
		   session->is_active ? "ACTIVE" : "INACTIVE",
		   session->user,
		   session->session_objpath));

	ret = TRUE;

error:
	if (dbus_error_is_set (&error))
		dbus_error_free (&error);
	return ret;
}

static gboolean
ck_tracker_init_get_sessions_for_seat (CKTracker *tracker, CKSeat *seat)
{
	gboolean ret;
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;

	ret = FALSE;

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						seat->seat_objpath,
						"org.freedesktop.ConsoleKit.Seat",
						"GetSessions");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (tracker->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_ERROR (("Error doing GetSeats on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto error;
	}

	dbus_message_iter_init (reply, &iter);
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
		HAL_WARNING (("Expecting an array from GetSessions on ConsoleKit."));
		dbus_message_unref (message);
		dbus_message_unref (reply);
		goto error;
	}
	dbus_message_iter_recurse (&iter, &iter_array);
	while (dbus_message_iter_get_arg_type (&iter_array) == DBUS_TYPE_OBJECT_PATH) {
		const char *session_objpath;
		CKSession *session;

		dbus_message_iter_get_basic (&iter_array, &session_objpath);
		HAL_INFO (("got session '%s' for seat '%s'", session_objpath, seat->seat_objpath));

		session = ck_session_new (session_objpath);

		/* get information: is_active etc. */
		if (!ck_session_get_info (tracker, session)) {
			HAL_ERROR (("Could not get information for session '%s'", session_objpath));
			dbus_message_unref (message);
			dbus_message_unref (reply);
			goto error;
		}

		ck_seat_attach_session (seat, session);

		tracker->sessions = g_slist_prepend (tracker->sessions, session);

		dbus_message_iter_next (&iter_array);
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

	HAL_INFO (("Got all sessions on seat '%s'", seat->seat_objpath));

	ret = TRUE;

error:
	if (dbus_error_is_set (&error))
		dbus_error_free (&error);

	return ret;
}

static gboolean
ck_tracker_init_get_seats_and_sessions (CKTracker *tracker)
{
	gboolean ret;
	DBusError error;
	DBusMessage *message;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter iter_array;

	ret = FALSE;

	/* first build array of existing seats and sessions */

	message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
						"/org/freedesktop/ConsoleKit/Manager",
						"org.freedesktop.ConsoleKit.Manager",
						"GetSeats");
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (tracker->dbus_connection, message, -1, &error);
	if (reply == NULL || dbus_error_is_set (&error)) {
		HAL_ERROR (("Error doing GetSeats on ConsoleKit: %s: %s", error.name, error.message));
		dbus_message_unref (message);
		if (reply != NULL)
			dbus_message_unref (reply);
		goto error;
	}

	dbus_message_iter_init (reply, &iter);
	if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
		HAL_WARNING (("Expecting an array from GetSeats on ConsoleKit."));
		dbus_message_unref (message);
		dbus_message_unref (reply);
		goto error;
	}
	dbus_message_iter_recurse (&iter, &iter_array);
	while (dbus_message_iter_get_arg_type (&iter_array) == DBUS_TYPE_OBJECT_PATH) {
		const char *seat_objpath;
		CKSeat *seat;

		dbus_message_iter_get_basic (&iter_array, &seat_objpath);
		HAL_INFO (("got seat '%s'", seat_objpath));

		seat = ck_seat_new (seat_objpath);

		/* get information */
		if (!ck_seat_get_info (tracker, seat)) {
			HAL_ERROR (("Could not get information for seat '%s'", seat_objpath));
			dbus_message_unref (message);
			dbus_message_unref (reply);
			goto error;
		}

		/* for each seat, get the sessions */
		if (!ck_tracker_init_get_sessions_for_seat (tracker, seat)) {
			HAL_ERROR (("Could not get sessions for seat '%s'", seat_objpath));
			dbus_message_unref (message);
			dbus_message_unref (reply);
			goto error;
		}

		tracker->seats = g_slist_prepend (tracker->seats, seat);

		dbus_message_iter_next (&iter_array);
	}
	dbus_message_unref (message);
	dbus_message_unref (reply);

	HAL_INFO (("Got seats"));

	ret = TRUE;
error:
	if (dbus_error_is_set (&error))
		dbus_error_free (&error);

	return ret;
}

void
ck_tracker_ref (CKTracker *tracker)
{
	tracker->refcount++;
}

static void
ck_tracker_remove_all_seats_and_sessions (CKTracker *tracker)
{
	GSList *i;
	
	for (i = tracker->sessions; i != NULL; i = g_slist_next (i)) {
		CKSession *session = (CKSession *) i->data;
		ck_session_unref (session);
	}
	g_slist_free (tracker->sessions);
	tracker->sessions = NULL;
	
	for (i = tracker->seats; i != NULL; i = g_slist_next (i)) {
		CKSeat *seat = (CKSeat *) i->data;
		ck_seat_unref (seat);
	}
	g_slist_free (tracker->seats);
	tracker->seats = NULL;
}

void
ck_tracker_unref (CKTracker *tracker)
{
	tracker->refcount--;

	if (tracker->refcount == 0) {
		ck_tracker_remove_all_seats_and_sessions (tracker);
		tracker->dbus_connection = NULL;
	}
}

void
ck_tracker_set_system_bus_connection (CKTracker *tracker, DBusConnection *system_bus_connection)
{
	tracker->dbus_connection = system_bus_connection;
}

void
ck_tracker_set_user_data (CKTracker *tracker, void *user_data)
{
	tracker->user_data = user_data;
}

void
ck_tracker_set_seat_added_cb (CKTracker *tracker, CKSeatAddedCB cb)
{
	tracker->seat_added_cb = cb;
}

void
ck_tracker_set_seat_removed_cb (CKTracker *tracker, CKSeatRemovedCB cb)
{
	tracker->seat_removed_cb = cb;
}

void
ck_tracker_set_session_added_cb (CKTracker *tracker, CKSessionAddedCB cb)
{
	tracker->session_added_cb = cb;
}

void
ck_tracker_set_session_removed_cb (CKTracker *tracker, CKSessionRemovedCB cb)
{
	tracker->session_removed_cb = cb;
}

void
ck_tracker_set_session_active_changed_cb (CKTracker *tracker, CKSessionActiveChangedCB cb)
{
	tracker->session_active_changed_cb = cb;
}

void
ck_tracker_set_service_disappeared_cb  (CKTracker *tracker, CKServiceDisappearedCB cb)
{
	tracker->service_disappeared_cb = cb;
}

void
ck_tracker_set_service_appeared_cb  (CKTracker *tracker, CKServiceAppearedCB cb)
{
	tracker->service_appeared_cb = cb;
}

void
ck_tracker_process_system_bus_message (CKTracker *tracker, DBusMessage *message)
{
	const char *seat_objpath;
	const char *session_objpath;
	dbus_bool_t session_is_active;
	GSList *i;
	CKSeat *seat;
	CKSession *session;

	/*HAL_INFO (("In ck_tracker_process_system_bus_message objpath=%s interface=%s method=%s", 
		   dbus_message_get_path (message), 
		   dbus_message_get_interface (message),
		   dbus_message_get_member (message)));*/

	/* TODO: also handle SeatRemoved and SeatAdded */

	if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Manager", "SeatAdded")) {

		seat_objpath = dbus_message_get_path (message);

		if (!dbus_message_get_args (message, NULL,
#ifdef HAVE_CK_0_3
					    DBUS_TYPE_OBJECT_PATH, &seat_objpath,
#else
					    DBUS_TYPE_STRING, &seat_objpath,
#endif
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid SeatAdded signal from CK"));
			goto out;
		}

		HAL_INFO (("Received SeatAdded '%s' from CK", seat_objpath));

		seat = ck_seat_new (seat_objpath);

		/* get information */
		if (!ck_seat_get_info (tracker, seat)) {
			HAL_ERROR (("Could not get information for seat '%s'", seat_objpath));
			goto out;
		}

		tracker->seats = g_slist_prepend (tracker->seats, seat);

		if (tracker->seat_added_cb != NULL) {
			tracker->seat_added_cb (tracker, seat, tracker->user_data);
		}

	} else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Manager", "SeatRemoved")) {

		seat_objpath = dbus_message_get_path (message);

		if (!dbus_message_get_args (message, NULL,
#ifdef HAVE_CK_0_3
					    DBUS_TYPE_OBJECT_PATH, &seat_objpath,
#else
					    DBUS_TYPE_STRING, &seat_objpath,
#endif
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid SeatRemoved signal from CK"));
			goto out;
		}

		HAL_INFO (("Received SeatRemoved '%s' from CK", seat_objpath));

		for (i = tracker->seats; i != NULL; i = g_slist_next (i)) {
			seat = (CKSeat *) i->data;
			if (strcmp (seat->seat_objpath, seat_objpath) == 0) {

				tracker->seats = g_slist_remove (tracker->seats, seat);

				if (tracker->seat_removed_cb != NULL) {
					tracker->seat_removed_cb (tracker, seat, tracker->user_data);
				}

				ck_seat_unref (seat);
				break;
			}
		}
		if (i == NULL) {
			HAL_ERROR (("No such seat '%s'", seat_objpath));
		}
	} else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Seat", "SessionAdded")) {

		seat_objpath = dbus_message_get_path (message);

		if (!dbus_message_get_args (message, NULL,
#ifdef HAVE_CK_0_3
					    DBUS_TYPE_OBJECT_PATH, &session_objpath,
#else
					    DBUS_TYPE_STRING, &session_objpath,
#endif
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid SessionAdded signal from CK"));
			goto out;
		}

		HAL_INFO (("Received SessionAdded '%s' from CK on seat '%s'", session_objpath, seat_objpath));

		for (i = tracker->seats; i != NULL; i = g_slist_next (i)) {
			seat = (CKSeat *) i->data;
			if (strcmp (seat->seat_objpath, seat_objpath) == 0) {
				session = ck_session_new (session_objpath);

				/* get information: is_active etc. */
				if (!ck_session_get_info (tracker, session)) {
					HAL_ERROR (("Could not get information for session '%s'", session_objpath));
					goto out;
				}

				ck_seat_attach_session (seat, session);
				tracker->sessions = g_slist_prepend (tracker->sessions, session);

				if (tracker->session_added_cb != NULL) {
					tracker->session_added_cb (tracker, session, tracker->user_data);
				}
				break;
			}
		}
		if (i == NULL) {
			HAL_ERROR (("No seat '%s for session '%s'", seat_objpath, session_objpath));
		}
	} else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Seat", "SessionRemoved")) {

		seat_objpath = dbus_message_get_path (message);

		if (!dbus_message_get_args (message, NULL,
#ifdef HAVE_CK_0_3
					    DBUS_TYPE_OBJECT_PATH, &session_objpath,
#else
					    DBUS_TYPE_STRING, &session_objpath,
#endif
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid SessionRemoved signal from CK"));
			goto out;
		}

		HAL_INFO (("Received SessionRemoved '%s' from CK on seat '%s'", session_objpath, seat_objpath));

		for (i = tracker->sessions; i != NULL; i = g_slist_next (i)) {
			session = (CKSession *) i->data;
			if (strcmp (session->session_objpath, session_objpath) == 0) {

				if (tracker->session_removed_cb != NULL) {
					tracker->session_removed_cb (tracker, session, tracker->user_data);
				}

				if (session->seat == NULL) {
					HAL_ERROR (("Session '%s' to be removed is not attached to a seat", 
						    session_objpath));
				} else {
					ck_seat_detach_session (session->seat, session);
				}
				tracker->sessions = g_slist_remove (tracker->sessions, session);
				ck_session_unref (session);
				break;
			}
		}
		if (i == NULL) {
			HAL_ERROR (("No such session '%s'", session_objpath));
		}
	} else if (dbus_message_is_signal (message, "org.freedesktop.ConsoleKit.Session", "ActiveChanged")) {

		session_objpath = dbus_message_get_path (message);

		if (!dbus_message_get_args (message, NULL,
					    DBUS_TYPE_BOOLEAN, &session_is_active,
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid SessionRemoved signal from CK"));
			goto out;
		}

		HAL_INFO (("Received ActiveChanged to %s from CK on session '%s'", 
			   session_is_active ? "ACTIVE" : "INACTIVE", session_objpath));


		for (i = tracker->sessions; i != NULL; i = g_slist_next (i)) {
			session = (CKSession *) i->data;
			if (strcmp (session->session_objpath, session_objpath) == 0) {

				session->is_active = session_is_active;

				if (tracker->session_active_changed_cb != NULL) {
					tracker->session_active_changed_cb (tracker, session, tracker->user_data);
				}
				break;
			}
		}
		if (i == NULL) {
			HAL_ERROR (("No such session '%s'", session_objpath));
		}
	} else if (dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
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

		if (strlen (new_service_name) == 0 && strcmp (name, "org.freedesktop.ConsoleKit") == 0) {
			HAL_INFO (("uh, oh, ConsoleKit went away!"));
			ck_tracker_remove_all_seats_and_sessions (tracker);
			if (tracker->service_disappeared_cb != NULL) {
				tracker->service_disappeared_cb (tracker, tracker->user_data);
			}
		}

		if (strlen (old_service_name) == 0 && strcmp (name, "org.freedesktop.ConsoleKit") == 0) {
			HAL_INFO (("ConsoleKit reappeared!"));
			ck_tracker_init (tracker);
			if (tracker->service_appeared_cb != NULL) {
				tracker->service_appeared_cb (tracker, tracker->user_data);
			}
		}


	}

out:
	;
}

GSList *
ck_tracker_get_seats (CKTracker *tracker)
{
	return tracker->seats;
}

GSList *
ck_tracker_get_sessions (CKTracker *tracker)
{
	return tracker->sessions;
}

GSList *
ck_seat_get_sessions (CKSeat *seat)
{
	return seat->sessions;
}

char *
ck_seat_get_id (CKSeat *seat)
{
	return g_path_get_basename (seat->seat_objpath);
}

gboolean
ck_session_is_active (CKSession *session)
{
	return session->is_active;
}

CKSeat *
ck_session_get_seat (CKSession *session)
{
	return session->seat;
}

char *
ck_session_get_id (CKSession *session)
{
	return g_path_get_basename (session->session_objpath);
}

uid_t
ck_session_get_user (CKSession *session)
{
	return session->user;
}

gboolean
ck_session_is_local (CKSession *session)
{
	return session->is_local;
}

const char *
ck_session_get_hostname (CKSession *session)
{
	return session->hostname;
}

gboolean
ck_tracker_init (CKTracker *tracker)
{
	dbus_bool_t ret;

	ret = FALSE;

	if (!ck_tracker_init_get_seats_and_sessions (tracker)) {
		HAL_ERROR (("Could not get seats and sessions"));
		goto out;
	}
	
	HAL_INFO (("Got seats and sessions"));

	ret = TRUE;

out:
	return ret;
}

CKTracker *
ck_tracker_new (void)
{
	CKTracker *tracker;

	tracker = g_new0 (CKTracker, 1);
	tracker->refcount = 1;
	return tracker;
}

CKSession *
ck_tracker_find_session (CKTracker *tracker, const char *ck_session_objpath)
{
        CKSession *session;
        GSList *i;
        for (i = tracker->sessions; i != NULL; i = g_slist_next (i)) {
                session = i->data;
                if (strcmp (session->session_objpath, ck_session_objpath) == 0) {
                        goto out;
                }
        }
        session = NULL;
out:
        return session;
}
