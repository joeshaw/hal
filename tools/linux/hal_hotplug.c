/***************************************************************************
 * CVSID: $Id$
 *
 * hal_hotplug.c : Tiny program to transform a linux-hotplug event into
 *                 a D-BUS message
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

/**
 * @defgroup HalMisc  Misc tools for HAL
 * @brief  Misc. tools for HAL
 */


/**
 * @defgroup HalLinuxHotplug  HAL hotplug helper for Linux
 * @ingroup HalMisc
 * @brief A short program for translating linux-hotplug events into
 *        D-BUS messages. The messages are sent to the HAL daemon.
 * @{
 */

/** Entry point
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @param  envp                Environment
 *  @return                     Exit code
 */
int main(int argc, char* argv[], char* envp[])
{
    int i, j, len;
    char* str;
    DBusError error;
    DBusConnection* sysbus_connection;
    DBusMessage* message;
    DBusMessageIter iter;
    DBusMessageIter iter_dict;

    if( argc!=2 )
        return 1;

    /* Connect to a well-known bus instance, the system bus */
    dbus_error_init(&error);
    sysbus_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if( sysbus_connection==NULL ) 
        return 1;

    /* service, object, interface, member */
    message = dbus_message_new_method_call(
        "org.freedesktop.Hal",
        "/org/freedesktop/Hal/Linux/Hotplug", 
        "org.freedesktop.Hal.Linux.Hotplug",
        "HotplugEvent");

    /* not interested in a reply */
    dbus_message_set_no_reply(message, TRUE);
    
    dbus_message_iter_init(message, &iter);
    dbus_message_iter_append_string(&iter, argv[1]);
    dbus_message_iter_append_dict(&iter, &iter_dict);
    for(i=0; envp[i]!=NULL; i++)
    {
        str = envp[i];
        len = strlen(str);
        for(j=0; j<len && str[j]!='='; j++)
            ;
        str[j]='\0';

        dbus_message_iter_append_dict_key(&iter_dict, str);
        dbus_message_iter_append_string(&iter_dict, str+j+1);
    }

    /* Do some sleep here so the kernel have time to publish it's
     * stuff in sysfs
     */
    usleep(1000*1000);

    if ( !dbus_connection_send(sysbus_connection, message, NULL) )
        return 1;

    dbus_message_unref(message);        
    dbus_connection_flush(sysbus_connection);

    /* Do some sleep here so messages are not lost.. */
    usleep(500*1000);

    dbus_connection_disconnect(sysbus_connection);

    return 0;
}

/** @} */
