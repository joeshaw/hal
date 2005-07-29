/***************************************************************************
 * CVSID: $Id$
 *
 * hal_scsi.c : SCSI functions for sysfs-agent on Linux 2.6
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
#include "hal_scsi.h"


/** This function will compute the device uid based on other properties
 *  of the device. For SCSI hosts it is the parent name device UDI
 *  prepended with scsi_host_
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* scsi_host_compute_udi(const char* udi, int append_num)
{
    char* format;
    char* pd;
    char* name;
    int i, len;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/scsi_host_%s";
    else
        format = "/org/freedesktop/Hal/devices/scsi_host_%s-%d";

    pd = hal_device_get_property_string(udi, "Parent");
    len = strlen(pd);
    for(i=len-1; pd[i]!='/' && i>=0; i--)
        ;
    name = pd+i+1;

    snprintf(buf, 256, format, 
             name,
             append_num);

    free(pd);
    
    return buf;
}


/** This function will compute the device uid based on other properties
 *  of the device. For SCSI device it is the parent name device UDI
 *  prepended with scsi_device_
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* scsi_device_compute_udi(const char* udi, int append_num)
{
    char* format;
    char* pd;
    char* name;
    int i, len;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/scsi_device_%s";
    else
        format = "/org/freedesktop/Hal/devices/scsi_device_%s-%d";

    pd = hal_device_get_property_string(udi, "Parent");
    len = strlen(pd);
    for(i=len-1; pd[i]!='/' && i>=0; i--)
        ;
    name = pd+i+1;

    snprintf(buf, 256, format, 
             name,
             append_num);

    free(pd);
    
    return buf;
}

/** Visitor function for SCSI host.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_class_device_scsi_host(const char* path, 
                                  struct sysfs_class_device* class_device)
{
    char* d;
    char* parent_udi;

    if( class_device->sysdevice==NULL )
    {
        printf("Skipping virtual class device at path %s\n", path);
        return;
    }

    /*printf("scsi_host: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path);*/

    /** @todo: see if we already got this device */

    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "scsi_host");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", 
                                   class_device->sysdevice->path);

    /* guestimate product name */
    hal_device_set_property_string(d, "Product", "SCSI Host Interface");

    /* Compute parent */
    parent_udi = find_parent_udi_from_sysfs_path(
        class_device->sysdevice->path, HAL_LINUX_HOTPLUG_TIMEOUT);

    if( parent_udi!=NULL )
    {
        hal_device_set_property_string(d, "Parent", parent_udi);
        find_and_set_physical_device(d);
        hal_device_set_property_bool(d, "isVirtual", TRUE);
    }

    d = rename_and_maybe_add(d, scsi_host_compute_udi, "scsi_host");
}


/** Visitor function for SCSI device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_class_device_scsi_device(const char* path, 
                                    struct sysfs_class_device* class_device)
{
    char* d;
    char* parent_udi;

    if( class_device->sysdevice==NULL )
    {
        printf("Skipping virtual class device at path %s\n", path);
        return;
    }

    /*printf("scsi_device: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path);*/

    /** @todo: see if we already got this device */

    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "scsi_device");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", 
                                   class_device->sysdevice->path);

    /* guestimate product name */
    hal_device_set_property_string(d, "Product", "SCSI Interface");

    /* Compute parent */
    parent_udi = find_parent_udi_from_sysfs_path(
        class_device->sysdevice->path, HAL_LINUX_HOTPLUG_TIMEOUT);
    if( parent_udi!=NULL )
    {
        hal_device_set_property_string(d, "Parent", parent_udi);
        find_and_set_physical_device(d);
        hal_device_set_property_bool(d, "isVirtual", TRUE);
    }

    d = rename_and_maybe_add(d, scsi_device_compute_udi, "scsi_device");
}

/** Init function for SCSI handling
 *
 */
void hal_scsi_init()
{
}

/** Shutdown function for SCSI handling
 *
 */
void hal_scsi_shutdown()
{
}
