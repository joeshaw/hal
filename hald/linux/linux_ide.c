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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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

#include "../logger.h"
#include "../device_store.h"
#include "linux_ide.h"


/** This function will compute the device uid based on other properties
 *  of the device. For an IDE device it's simply ide appended with the
 *  channel and sub_channel
 *
 *  @param  d                   The HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* ide_compute_udi(HalDevice* d, int append_num)
{
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/ide_%d_%d";
    else
        format = "/org/freedesktop/Hal/devices/ide_%d_%d-%d";

    snprintf(buf, 256, format, 
             ds_property_get_int(d, "ide.channel"),
             ds_property_get_int(d, "ide.subChannel"),
             append_num);
    return buf;
}


/** This function will compute the device uid based on other properties
 *  of the device. For an IDE host device it's simply ide_host appended with
 *  the host number
 *
 *  @param  d                   The HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* ide_host_compute_udi(HalDevice* d, int append_num)
{
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/ide_host_%d";
    else
        format = "/org/freedesktop/Hal/devices/ide_host_%d-%d";

    snprintf(buf, 256, format, 
             ds_property_get_int(d, "ide_host.number"),
             append_num);
    return buf;
}

/* fwd decl */
static void visit_device_ide_host_got_parent(HalDevice* parent, 
                                             void* data1, void* data2);

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
    HalDevice* d;
    int ide_host_number;
    char* parent_sysfs_path;

    /*printf("ide_host: %s\n", path);*/
    if( sscanf(device->bus_id, "ide%d", &ide_host_number)!=1 )
    {
        LOG_ERROR(("Couldn't find ide_host_number in %s\n",
                   device->bus_id));
        return;
    }

    /*printf("ide_host_number=%d\n", ide_host_number);*/

    d = ds_device_new();
    ds_property_set_string(d, "Bus", "ide_host");
    ds_property_set_string(d, "Linux.sysfs_path", path);
    ds_property_set_string(d, "Linux.sysfs_path_device", path);
    ds_property_set_int(d, "ide_host.number", ide_host_number);

    /* guestimate product name */
    if( ide_host_number==0 )
        ds_property_set_string(d, "Product", "Primary IDE channel");
    else if( ide_host_number==1 )
        ds_property_set_string(d, "Product", "Secondary IDE channel");
    else
        ds_property_set_string(d, "Product", "IDE channel");


    parent_sysfs_path = get_parent_sysfs_path(path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string("Linux.sysfs_path_device",
                                             parent_sysfs_path, 
                                             visit_device_ide_host_got_parent,
                                             (void*) d, NULL, 
                                             is_probing ? 0 :
                                             HAL_LINUX_HOTPLUG_TIMEOUT);

    free(parent_sysfs_path);
}

/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void visit_device_ide_host_got_parent(HalDevice* parent, 
                                             void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    if( parent!=NULL )
    {
        ds_property_set_string(d, "Parent", parent->udi);
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "isVirtual", TRUE);
    }

    /* Compute a proper UDI (unique device id) and try to locate a persistent
     * unplugged device or simple add this new device...
     */
    rename_and_maybe_add(d, ide_host_compute_udi, "ide_host");
}


/* fwd decl */
static void visit_device_ide_got_parent(HalDevice* parent, 
                                        void* data1, void* data2);

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
    HalDevice* d;
    int channel;
    int sub_channel;
    char* parent_sysfs_path;

    /*printf("ide: %s\n", path);*/
    if( sscanf(device->bus_id, "%d.%d", &channel, &sub_channel)!=2 )
    {
        LOG_ERROR(("Couldn't find channel and sub_channel in %s\n",
                   device->bus_id));
        return;
    }

    /*printf("channel=%d, sub_channel=%d\n", channel, sub_channel);*/

    d = ds_device_new();
    ds_property_set_string(d, "Bus", "ide");
    ds_property_set_string(d, "Linux.sysfs_path", path);
    ds_property_set_string(d, "Linux.sysfs_path_device", path);
    ds_property_set_int(d, "ide.channel", channel);
    ds_property_set_int(d, "ide.subChannel", sub_channel);

    /* guestimate product name */
    if( sub_channel==0 )
        ds_property_set_string(d, "Product", "Master IDE Interface");
    else
        ds_property_set_string(d, "Product", "Slave IDE Interface");

    parent_sysfs_path = get_parent_sysfs_path(path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string("Linux.sysfs_path_device",
                                             parent_sysfs_path, 
                                             visit_device_ide_got_parent,
                                             (void*) d, NULL, 
                                             is_probing ? 0 :
                                             HAL_LINUX_HOTPLUG_TIMEOUT);

    free(parent_sysfs_path);
}

/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void visit_device_ide_got_parent(HalDevice* parent, 
                                        void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    if( parent!=NULL )
    {
        ds_property_set_string(d, "Parent", parent->udi);
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "isVirtual", TRUE);
    }

    /* Compute a proper UDI (unique device id) and try to locate a persistent
     * unplugged device or simple add this new device...
     */
    rename_and_maybe_add(d, ide_compute_udi, "ide");
}


/** Init function for IDE handling
 *
 */
void linux_ide_init()
{
}

/** Shutdown function for IDE handling
 *
 */
void linux_ide_shutdown()
{
}
