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
#include "linux_class_scsi.h"

/**
 * @defgroup HalDaemonLinuxScsi SCSI class
 * @ingroup HalDaemonLinux
 * @brief SCSI class
 * @{
 */


/** This function will compute the device uid based on other properties
 *  of the device. For SCSI hosts it is the parent name device UDI
 *  prepended with scsi_host_
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* scsi_host_compute_udi(HalDevice* d, int append_num)
{
    int i, len;
    const char* format;
    const char* pd;
    const char* name;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/scsi_host_%s";
    else
        format = "/org/freedesktop/Hal/devices/scsi_host_%s-%d";

    pd = ds_property_get_string(d, "Parent");
    len = strlen(pd);
    for(i=len-1; pd[i]!='/' && i>=0; i--)
        ;
    name = pd+i+1;

    snprintf(buf, 256, format, 
             name,
             append_num);
    
    return buf;
}


/** This function will compute the device uid based on other properties
 *  of the device. For SCSI device it is the parent name device UDI
 *  prepended with scsi_device_
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* scsi_device_compute_udi(HalDevice* d, int append_num)
{
    int i, len;
    const char* format;
    const char* pd;
    const char* name;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/scsi_device_%s";
    else
        format = "/org/freedesktop/Hal/devices/scsi_device_%s-%d";

    pd = ds_property_get_string(d, "Parent");
    len = strlen(pd);
    for(i=len-1; pd[i]!='/' && i>=0; i--)
        ;
    name = pd+i+1;

    snprintf(buf, 256, format, 
             name,
             append_num);
    
    return buf;
}


/* fwd decl */
static void visit_class_device_scsi_host_got_parent(HalDevice* parent, 
                                                    void* data1, void* data2);

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
    HalDevice* d;
    char* parent_sysfs_path;

    if( class_device->sysdevice==NULL )
    {
        HAL_INFO(("Skipping virtual class device at path %s\n", path));
        return;
    }

    /*printf("scsi_host: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path);*/

    /** @todo: see if we already got this device */

    d = ds_device_new();
    ds_property_set_string(d, "Bus", "scsi_host");
    ds_property_set_string(d, "Linux.sysfs_path", path);
    ds_property_set_string(d, "Linux.sysfs_path_device", 
                           class_device->sysdevice->path);

    /* guestimate product name */
    ds_property_set_string(d, "Product", "SCSI Host Interface");

    parent_sysfs_path = get_parent_sysfs_path(class_device->sysdevice->path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "Linux.sysfs_path_device",
        parent_sysfs_path, 
        visit_class_device_scsi_host_got_parent,
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
static void visit_class_device_scsi_host_got_parent(HalDevice* parent, 
                                                    void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    printf("parent=0x%08x\n", parent);

    if( parent!=NULL )
    {
        ds_property_set_string(d, "Parent", parent->udi);
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "isVirtual", TRUE);
    }
    else
    {
        HAL_ERROR(("No parent for SCSI device!"));
        ds_device_destroy(d);
        return;
    }

    /* Compute a proper UDI (unique device id) and try to locate a persistent
     * unplugged device or simple add this new device...
     */
    rename_and_maybe_add(d, scsi_host_compute_udi, "scsi_host");
}


/* fwd decl */
static void visit_class_device_scsi_device_got_parent(HalDevice* parent, 
                                                     void* data1, void* data2);

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
    HalDevice* d;
    char* parent_sysfs_path;

    if( class_device->sysdevice==NULL )
    {
        HAL_INFO(("Skipping virtual class device at path %s\n", path));
        return;
    }

    /*printf("scsi_device: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path);*/

    /** @todo: see if we already got this device */

    d = ds_device_new();
    ds_property_set_string(d, "Bus", "scsi_device");
    ds_property_set_string(d, "Linux.sysfs_path", path);
    ds_property_set_string(d, "Linux.sysfs_path_device", 
                           class_device->sysdevice->path);

    /* guestimate product name */
    ds_property_set_string(d, "Product", "SCSI Interface");


    parent_sysfs_path = get_parent_sysfs_path(class_device->sysdevice->path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "Linux.sysfs_path_device",
        parent_sysfs_path,
        visit_class_device_scsi_device_got_parent,
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
static void visit_class_device_scsi_device_got_parent(HalDevice* parent, 
                                                      void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    if( parent!=NULL )
    {
        ds_property_set_string(d, "Parent", parent->udi);
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "isVirtual", TRUE);
    }
    else
    {
        HAL_ERROR(("No parent for SCSI device!"));
        ds_device_destroy(d);
        return;
    }

    /* Compute a proper UDI (unique device id) and try to locate a persistent
     * unplugged device or simple add this new device...
     */
    rename_and_maybe_add(d, scsi_device_compute_udi, "scsi_device");
}

/** Init function for SCSI handling
 *
 */
void linux_class_scsi_init()
{
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void linux_class_scsi_detection_done()
{
}

/** Shutdown function for SCSI handling
 *
 */
void linux_class_scsi_shutdown()
{
}

/** @} */
