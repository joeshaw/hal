/***************************************************************************
 * CVSID: $Id$
 *
 * osspec.c : HAL backend for Solaris
 *
 * Copyright (C) 2005 Sun Microsystems
 * Author: Alvaro Lopez Ortega <alvaro@sun.com>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../hald_dbus.h"
#include "../device_info.h"
#include "../util.h"


void
osspec_init (void)
{
}

void 
osspec_probe (void)
{
}

gboolean
osspec_device_rescan (HalDevice *d)
{
	   return FALSE;
}

gboolean
osspec_device_reprobe (HalDevice *d)
{
	   return FALSE;
}

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

