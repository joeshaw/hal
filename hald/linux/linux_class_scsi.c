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
 *  of the device. For SCSI hosts it is the host number
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* scsi_host_compute_udi(HalDevice* d, int append_num)
{
    const char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/scsi_host_%d";
    else
        format = "/org/freedesktop/Hal/devices/scsi_host_%d-%d";

    snprintf(buf, 256, format, 
             ds_property_get_int(d, "scsi_host.host"),
             append_num);
    
    return buf;
}


/** This function will compute the device uid based on other properties
 *  of the device. For SCSI device it is the {host, bus, target, lun} 
 *  tupple
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* scsi_device_compute_udi(HalDevice* d, int append_num)
{
    const char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/scsi_device_%d_%d_%d_%d";
    else
        format = "/org/freedesktop/Hal/devices/scsi_device_%d_%d_%d_%d-%d";

    snprintf(buf, 256, format, 
             ds_property_get_int(d, "scsi_device.host"),
             ds_property_get_int(d, "scsi_device.bus"),
             ds_property_get_int(d, "scsi_device.target"),
             ds_property_get_int(d, "scsi_device.lun"),
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
    const char* last_elem;
    int host_num;

    if( class_device->sysdevice==NULL )
    {
        HAL_INFO(("Skipping virtual class device at path %s\n", path));
        return;
    }

    HAL_INFO(("scsi_host: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path));

    /** @todo: see if we already got this device */

    d = ds_device_new();
    ds_property_set_string(d, "info.bus", "scsi_host");
    ds_property_set_string(d, "linux.sysfs_path", path);
    ds_property_set_string(d, "linux.sysfs_path_device", 
                           class_device->sysdevice->path);

    /* Sets last_elem to host14 in path=/sys/class/scsi_host/host14 */
    last_elem = get_last_element(path);
    sscanf(last_elem, "host%d", &host_num);
    ds_property_set_int(d, "scsi_host.host", host_num);


    /* guestimate product name */
    ds_property_set_string(d, "info.product", "SCSI Host Interface");

    parent_sysfs_path = get_parent_sysfs_path(class_device->sysdevice->path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "linux.sysfs_path_device",
        parent_sysfs_path, 
        TRUE,
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
    char* new_udi = NULL;
    HalDevice* new_d = NULL;
    HalDevice* d = (HalDevice*) data1;

    /*printf("parent=0x%08x\n", parent);*/

    if( parent!=NULL )
    {
        ds_property_set_string(d, "info.parent", parent->udi);
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "info.virtual", TRUE);
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
    new_udi = rename_and_merge(d, scsi_host_compute_udi, "scsi_host");
    if( new_udi!=NULL )
    {
        new_d = ds_device_find(new_udi);
        if( new_d!=NULL )
        {
            ds_gdl_add(new_d);
        }
    }
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
    const char* last_elem;
    int host_num, bus_num, target_num, lun_num;

    if( class_device->sysdevice==NULL )
    {
        HAL_INFO(("Skipping virtual class device at path %s\n", path));
        return;
    }

    /*printf("scsi_device: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path);*/

    /** @todo: see if we already got this device */

    d = ds_device_new();
    ds_property_set_string(d, "info.bus", "scsi_device");
    ds_property_set_string(d, "linux.sysfs_path", path);
    ds_property_set_string(d, "linux.sysfs_path_device", 
                           class_device->sysdevice->path);

    /* Sets last_elem to 1:2:3:4 in path=/sys/class/scsi_host/host23/1:2:3:4 */
    last_elem = get_last_element(path);
    sscanf(last_elem, "%d:%d:%d:%d", 
           &host_num, &bus_num, &target_num, &lun_num);
    ds_property_set_int(d, "scsi_device.host", host_num);
    ds_property_set_int(d, "scsi_device.bus", bus_num);
    ds_property_set_int(d, "scsi_device.target", target_num);
    ds_property_set_int(d, "scsi_device.lun", lun_num);


    /* guestimate product name */
    ds_property_set_string(d, "info.product", "SCSI Interface");


    parent_sysfs_path = get_parent_sysfs_path(class_device->sysdevice->path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "linux.sysfs_path_device",
        parent_sysfs_path,
        TRUE,
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
    char* new_udi = NULL;
    HalDevice* new_d = NULL;
    HalDevice* d = (HalDevice*) data1;

    if( parent!=NULL )
    {
        ds_property_set_string(d, "info.parent", parent->udi);
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "info.virtual", TRUE);
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
    new_udi = rename_and_merge(d, scsi_device_compute_udi, "scsi_device");
    if( new_udi!=NULL )
    {
        new_d = ds_device_find(new_udi);
        if( new_d!=NULL )
        {
            ds_gdl_add(new_d);
        }
    }
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
