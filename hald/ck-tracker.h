/***************************************************************************
 * CVSID: $Id$
 *
 * ck-tracker.h : Track seats and sessions via ConsoleKit
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

#ifndef CK_TRACKER_H
#define CK_TRACKER_H

#include <unistd.h>
#include <sys/types.h>
#include <dbus/dbus.h>
#include <glib.h>

struct CKTracker_s;
typedef struct CKTracker_s CKTracker;
struct CKSeat_s;
typedef struct CKSeat_s CKSeat;
struct CKSession_s;
typedef struct CKSession_s CKSession;

typedef void (*CKSeatAddedCB) (CKTracker *tracker, CKSeat *seat, void *user_data);
typedef void (*CKSeatRemovedCB) (CKTracker *tracker, CKSeat *seat, void *user_data);
typedef void (*CKSessionAddedCB) (CKTracker *tracker, CKSession *session, void *user_data);
typedef void (*CKSessionRemovedCB) (CKTracker *tracker, CKSession *session, void *user_data);
typedef void (*CKSessionActiveChangedCB) (CKTracker *tracker, CKSession *session, void *user_data);
typedef void (*CKServiceDisappearedCB) (CKTracker *tracker, void *user_data);
typedef void (*CKServiceAppearedCB) (CKTracker *tracker, void *user_data);

CKTracker  *ck_tracker_new                        (void);
void        ck_tracker_set_system_bus_connection     (CKTracker *tracker, DBusConnection *system_bus_connection);
void        ck_tracker_set_user_data                 (CKTracker *tracker, void *user_data);
void        ck_tracker_set_seat_added_cb             (CKTracker *tracker, CKSeatAddedCB cb);
void        ck_tracker_set_seat_removed_cb           (CKTracker *tracker, CKSeatRemovedCB cb);
void        ck_tracker_set_session_added_cb          (CKTracker *tracker, CKSessionAddedCB cb);
void        ck_tracker_set_session_removed_cb        (CKTracker *tracker, CKSessionRemovedCB cb);
void        ck_tracker_set_session_active_changed_cb (CKTracker *tracker, CKSessionActiveChangedCB cb);
void        ck_tracker_set_service_disappeared_cb (CKTracker *tracker, CKServiceDisappearedCB cb);
void        ck_tracker_set_service_appeared_cb    (CKTracker *tracker, CKServiceAppearedCB cb);
/* TODO: also handle seat_added, seat_removed */
gboolean    ck_tracker_init                          (CKTracker *tracker);

/* Forward DBusMessage signals for 
 *
 *  org.freedesktop.ConsoleKit.Seat.SessionAdded
 *  org.freedesktop.ConsoleKit.Seat.SessionRemoved
 *  org.freedesktop.ConsoleKit.Session.ActiveChanged
 *
 * on all objects on ConsoleKit to this function. 
 *
 *  TODO: need org.freedesktop.DBus.NameOwnerChanged from D-Bus to notice CK going away
 *
 * TODO: provide convenience method for letting CKTracker setup matches
 * with the bus here via dbus_connection_add_filter() and
 * dbus_bus_add_match(). E.g. just like libhal.
 */
void        ck_tracker_process_system_bus_message (CKTracker *tracker, DBusMessage *message);

void        ck_tracker_ref                        (CKTracker *tracker);
void        ck_tracker_unref                      (CKTracker *tracker);

GSList     *ck_tracker_get_seats                  (CKTracker *tracker);
GSList     *ck_tracker_get_sessions               (CKTracker *tracker);

CKSession  *ck_tracker_find_session               (CKTracker *tracker, const char *ck_session_objpath);

GSList     *ck_seat_get_sessions                  (CKSeat *seat);
char 	   *ck_seat_get_id                        (CKSeat *seat);

gboolean    ck_session_is_active                  (CKSession *session);
CKSeat     *ck_session_get_seat                   (CKSession *session);
char 	   *ck_session_get_id                     (CKSession *session);
uid_t       ck_session_get_user                   (CKSession *session);
gboolean    ck_session_is_local                   (CKSession *session);
const char *ck_session_get_hostname               (CKSession *session);


#endif /* CK_TRACKER_H */
