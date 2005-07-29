/***************************************************************************
 * CVSID: $Id$
 *
 * hal_ide.c : IDE functions for sysfs-agent on Linux 2.6
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>

#include "main.h"
#include "hal_ide.h"


/** This function will compute the device uid based on other properties
 *  of the device. For an IDE device it's simply ide appended with the
 *  channel and sub_channel
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* ide_compute_udi(const char* udi, int append_num)
{
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/ide_%d_%d";
    else
        format = "/org/freedesktop/Hal/devices/ide_%d_%d-%d";

    snprintf(buf, 256, format, 
             hal_device_get_property_int(udi, "ide.channel"),
             hal_device_get_property_int(udi, "ide.subChannel"),
             append_num);
    return buf;
}


/** This function will compute the device uid based on other properties
 *  of the device. For an IDE host device it's simply ide_host appended with
 *  the host number
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* ide_host_compute_udi(const char* udi, int append_num)
{
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/ide_host_%d";
    else
        format = "/org/freedesktop/Hal/devices/ide_host_%d-%d";

    snprintf(buf, 256, format, 
             hal_device_get_property_int(udi, "ide_host.number"),
             append_num);
    return buf;
}

/** Visitor function for IDE host.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_device_ide_host(const char* path, struct sysfs_device *device)
{
    char* d;
    char* parent_udi;
    int ide_host_number;

    /*printf("ide_host: %s\n", path);*/
    if( sscanf(device->bus_id, "ide%d", &ide_host_number)!=1 )
    {
        printf("Couldn't find ide_host_number in %s\n",
               device->bus_id);
        return;
    }

    /*printf("ide_host_number=%d\n", ide_host_number);*/

    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "ide_host");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", path);
    hal_device_set_property_int(d, "ide_host.number", ide_host_number);

    /* guestimate product name */
    if( ide_host_number==0 )
        hal_device_set_property_string(d, "Product", "Primary IDE channel");
    else if( ide_host_number==1 )
        hal_device_set_property_string(d, "Product", "Secondary IDE channel");
    else
        hal_device_set_property_string(d, "Product", "IDE channel");

    /* Compute parent */
    parent_udi = find_parent_udi_from_sysfs_path(path, 
                                                 HAL_LINUX_HOTPLUG_TIMEOUT);
    if( parent_udi!=NULL )
    {
        hal_device_set_property_string(d, "Parent", parent_udi);
        find_and_set_physical_device(d);
        hal_device_set_property_bool(d, "isVirtual", TRUE);
    }

    d = rename_and_maybe_add(d, ide_host_compute_udi, "ide_host");
}

/** Visitor function for IDE.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_device_ide(const char* path, struct sysfs_device *device)
{
    char* d;
    char* parent_udi;
    int channel;
    int sub_channel;

    /*printf("ide: %s\n", path);*/
    if( sscanf(device->bus_id, "%d.%d", &channel, &sub_channel)!=2 )
    {
        printf("Couldn't find channel and sub_channel in %s\n",
               device->bus_id);
        return;
    }

    /*printf("channel=%d, sub_channel=%d\n", channel, sub_channel);*/

    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "ide");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", path);
    hal_device_set_property_int(d, "ide.channel", channel);
    hal_device_set_property_int(d, "ide.subChannel", sub_channel);

    /* guestimate product name */
    if( sub_channel==0 )
        hal_device_set_property_string(d, "Product", "Master IDE Interface");
    else
        hal_device_set_property_string(d, "Product", "Slave IDE Interface");

    /* Compute parent */
    parent_udi = find_parent_udi_from_sysfs_path(path, 
                                                 HAL_LINUX_HOTPLUG_TIMEOUT);
    if( parent_udi!=NULL )
    {
        hal_device_set_property_string(d, "Parent", parent_udi);
        find_and_set_physical_device(d);
        hal_device_set_property_bool(d, "isVirtual", TRUE);
    }

    d = rename_and_maybe_add(d, ide_compute_udi, "ide");
}

/** Init function for IDE handling
 *
 */
void hal_ide_init()
{
}

/** Shutdown function for IDE handling
 *
 */
void hal_ide_shutdown()
{
}
