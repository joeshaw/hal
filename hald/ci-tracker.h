/***************************************************************************
 * CVSID: $Id$
 *
 * ci-tracker.h : Track information about callers
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

#ifndef CALLER_INFO_H
#define CALLER_INFO_H

#include <unistd.h>
#include <sys/types.h>
#include <dbus/dbus.h>
#include <glib.h>

#ifdef HAVE_CONKIT
#include "ck-tracker.h"
#endif

struct CITracker_s;
typedef struct CITracker_s CITracker;

struct CICallerInfo_s;
typedef struct CICallerInfo_s CICallerInfo;

CITracker     *ci_tracker_new                          (void);
void           ci_tracker_set_system_bus_connection    (CITracker        *cit, 
                                                        DBusConnection   *system_bus_connection);
void           ci_tracker_init                         (CITracker        *cit);
void           ci_tracker_name_owner_changed           (CITracker        *cit,
                                                        const char       *name, 
                                                        const char       *old_service_name, 
                                                        const char       *new_service_name);

#ifdef HAVE_CONKIT
void           ci_tracker_active_changed               (CITracker        *cit,
                                                        const char       *session_objpath, 
                                                        gboolean          is_active);
#endif

CICallerInfo  *ci_tracker_get_info                     (CITracker        *cit,
                                                        const char       *system_bus_unique_name);

uid_t         ci_tracker_caller_get_uid                (CICallerInfo *ci);
const char   *ci_tracker_caller_get_sysbus_unique_name (CICallerInfo *ci);
#ifdef HAVE_CONKIT
pid_t         ci_tracker_caller_get_pid                (CICallerInfo *ci);
gboolean      ci_tracker_caller_is_local               (CICallerInfo *ci);
gboolean      ci_tracker_caller_in_active_session      (CICallerInfo *ci);
const char   *ci_tracker_caller_get_ck_session_path    (CICallerInfo *ci);
const char   *ci_tracker_caller_get_selinux_context    (CICallerInfo *ci);
#endif

#endif /* CALLER_INFO_H */
