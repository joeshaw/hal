/***************************************************************************
 * CVSID: $Id$
 *
 * access-check.h : Checks whether a D-Bus caller have access
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

#ifndef ACCESS_CHECK_H
#define ACCESS_CHECK_H

#include "device.h"
#include "ci-tracker.h"

gboolean access_check_caller_is_root_or_hal         (CITracker   *cit,
                                                     const char  *caller_unique_sysbus_name);
gboolean access_check_message_caller_is_root_or_hal (CITracker   *cit,
                                                     DBusMessage *message);
gboolean access_check_caller_have_access_to_device  (CITracker   *cit,
                                                     HalDevice   *device,
                                                     const char  *action,
                                                     const char  *caller_unique_sysbus_name,
                                                     int         *polkit_result_out);
gboolean access_check_caller_locked_out             (CITracker   *cit,
                                                     HalDevice   *device,
                                                     const char  *caller_unique_sysbus_name,
                                                     const char  *interface_name);
gboolean access_check_locked_by_others              (CITracker   *cit,
                                                     HalDevice   *device,
                                                     const char  *caller_unique_sysbus_name,
                                                     const char  *interface_name);

#endif /* ACCESS_CHECK_H */
