/***************************************************************************
 * CVSID: $Id$
 *
 * probe.c : Handling hardware of on Linux, using kernel 2.6
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

#include <dbus/dbus.h>

#include "../osspec.h"
#include "../logger.h"

#include "linux_common.h"
#include "linux_pci.h"
#include "linux_usb.h"
#include "linux_ide.h"
#include "linux_class_block.h"
#include "linux_class_scsi.h"
#include "linux_class_net.h"
#include "linux_class_input.h"

#include "libsysfs/libsysfs.h"

/** Mount path for sysfs */
char sysfs_mount_path[SYSFS_PATH_MAX];



/** Visitor function for any class device.
 *
 *  This function determines the class of the device and call the
 *  appropriate visit_class_device_<classtype> function if matched.
 *
 *  @param  path                Sysfs-path for class device, e.g.
 *                              /sys/class/scsi_host/host7
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void visit_class_device(const char* path, dbus_bool_t visit_children)
{
    struct sysfs_class_device* class_device;
    struct sysfs_directory* subdir;

    class_device = sysfs_open_class_device(path);
    if( class_device==NULL )
        DIE(("Coulnd't get sysfs class device object for path %s", path));

/*
    printf("    visit_class_device classname=%s name=%s path=%s\n",
           class_device->classname,
           class_device->name,
           class_device->path);
*/

    if( strcmp(class_device->classname, "scsi_host")==0 )
        visit_class_device_scsi_host(path, class_device);
    else if( strcmp(class_device->classname, "scsi_device")==0 )
        visit_class_device_scsi_device(path, class_device);
    else if( strcmp(class_device->classname, "block")==0 )
        visit_class_device_block(path, class_device);
    else if( strcmp(class_device->classname, "net")==0 )
        visit_class_device_net(path, class_device);
    /* Visit children */
    if( visit_children && class_device->directory!=NULL &&
        class_device->directory->subdirs!=NULL )
    {
        dlist_for_each_data(class_device->directory->subdirs, subdir, 
                            struct sysfs_directory)
        {
            char newpath[SYSFS_PATH_MAX];
            snprintf(newpath, SYSFS_PATH_MAX, "%s/%s", path, subdir->name);
            visit_class_device(newpath, TRUE);
        }
    }

    sysfs_close_class_device(class_device);
}

/** Visit all devices of a given class
 *
 *  @param  class_name          Name of class, e.g. scsi_host or block
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void visit_class(const char* class_name, dbus_bool_t visit_children)
{
    struct sysfs_class* cls = NULL;
    struct sysfs_class_device* cur = NULL;

    cls = sysfs_open_class(class_name);
    if( cls==NULL )
    {
        fprintf(stderr, "Error opening class %s\n", class_name);
        return;
    }

    if( cls->devices!=NULL )
    {
        dlist_for_each_data(cls->devices, cur, struct sysfs_class_device)
        {
            visit_class_device(cur->path, visit_children);
        }
    }
    
    sysfs_close_class(cls);
}

/** Visitor function for any device.
 *
 *  This function determines the bus-type of the device and call the
 *  appropriate visit_device_<bustype> function if matched.
 *
 *  @param  path                Sysfs-path for device
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void visit_device(const char* path, dbus_bool_t visit_children)
{
    struct sysfs_device* device;
    struct sysfs_directory* subdir;

    device = sysfs_open_device(path);
    if( device==NULL )
        DIE(("Coulnd't get sysfs device object for path %s", path));

    printf("    %s  busid=%s\n", device->bus, device->bus_id);

    if( device->bus!=NULL )
    {
        if( strcmp(device->bus, "pci")==0 )
            visit_device_pci(path, device);
        else if( strcmp(device->bus, "usb")==0 )
            visit_device_usb(path, device);
        else if( strcmp(device->bus, "ide")==0 )
            visit_device_ide(path, device);
        /** @todo This is a hack; is there such a thing as an ide_host? */
        else if( strncmp(device->bus_id, "ide", 3)==0 )
            visit_device_ide_host(path, device);
        else 
        {
            /*printf("bus=%s path=%s\n", device->bus, path);*/
        }
        /*
        */
/*
 */
    }

    /* Visit children */
    if( visit_children && device->directory->subdirs!=NULL )
    {
        dlist_for_each_data(device->directory->subdirs, subdir, 
                            struct sysfs_directory)
        {
            char newpath[SYSFS_PATH_MAX];
            snprintf(newpath, SYSFS_PATH_MAX, "%s/%s", path, subdir->name);
            visit_device(newpath, TRUE);
        }
    }

    sysfs_close_device(device);
}



/* This function is documented in ../osspec.h */
void osspec_init(DBusConnection* dbus_connection)
{
    int rc;

    /* get mount path for sysfs */
    rc = sysfs_get_mnt_path(sysfs_mount_path, SYSFS_PATH_MAX);
    if( rc!=0 )
    {
        DIE(("Couldn't get mount path for sysfs"));
    }
    LOG_INFO(("Mountpoint for sysfs is %s", sysfs_mount_path));

    linux_pci_init();
    linux_usb_init();
    linux_ide_init();
    linux_class_scsi_init();
    linux_class_block_init();
    linux_class_net_init();

    /* input devices are not yet in sysfs */
    linux_class_input_probe();

}

/** This is set to #TRUE if we are probing and #FALSE otherwise */
dbus_bool_t is_probing;

/* This function is documented in ../osspec.h */
void osspec_probe()
{
    char path[SYSFS_PATH_MAX];
    struct sysfs_directory* current;
    struct sysfs_directory* dir;

    is_probing = TRUE;

    /* traverse /sys/devices */
    strncpy(path, sysfs_mount_path, SYSFS_PATH_MAX);
    strncat(path, SYSFS_DEVICES_DIR, SYSFS_PATH_MAX);

    dir = sysfs_open_directory(path);
    if( dir==NULL )
    {
        DIE(("Error opening sysfs directory at %s\n", path));
    }
    if( sysfs_read_directory(dir)!=0 )
    {
        DIE(("Error reading sysfs directory at %s\n", path));
    }
    if( dir->subdirs!=NULL )
    {
        dlist_for_each_data(dir->subdirs, current, struct sysfs_directory)
        {
            visit_device(current->path, TRUE);
        }
    }
    sysfs_close_directory(dir);

    /* visit class devices in /sys/class/scsi_host */
    visit_class("scsi_host", FALSE);

    /* visit class devices in /sys/class/scsi_host */
    visit_class("scsi_device", FALSE);

    /* visit all block devices */
    visit_class("block", TRUE);

    /* visit all net devices */
    visit_class("net", TRUE);

    /* Find the input devices (no yet in sysfs) */
    //hal_input_probe();

    /* Process /etc/mtab and modify block devices we indeed have mounted 
     * (dont set up the watcher)
     */
    //etc_mtab_process_all_block_devices(FALSE);

    is_probing = FALSE;
}

static DBusHandlerResult osspec_hotplug(DBusConnection* connection,
                                        DBusMessage* message)
{
    DBusMessageIter iter;
    DBusMessageIter dict_iter;
    char* subsystem;
    char sysfs_devpath[SYSFS_PATH_MAX];
    dbus_bool_t is_add;

    dbus_message_iter_init(message, &iter);

    if( dbus_message_iter_get_arg_type(&iter)!=DBUS_TYPE_STRING )
    {
        /** @todo Report error */
        dbus_message_unref(message);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    subsystem = dbus_message_iter_get_string(&iter);

    dbus_message_iter_next(&iter);
    dbus_message_iter_init_dict_iterator(&iter, &dict_iter);

    printf("subsystem = %s\n", subsystem);

    is_add = FALSE;

    for( ;dbus_message_iter_has_next(&dict_iter); 
         dbus_message_iter_next(&dict_iter))
    {
        char* key;
        char* value;
        
        key = dbus_message_iter_get_dict_key(&dict_iter);
        value = dbus_message_iter_get_string(&dict_iter);

        printf("  key/value = %s |-> %s\n", key, value);

        if( strcmp(key, "ACTION")==0 )
        {
            if( strcmp(value, "add")==0 )
            {
                is_add = TRUE;
            }
        }

        if( strcmp(key, "DEVPATH")==0 )
        {
            strncpy(sysfs_devpath, sysfs_mount_path, SYSFS_PATH_MAX);
            strncat(sysfs_devpath, value, SYSFS_PATH_MAX);
        }
    }

    if( sysfs_devpath!=NULL && 
        (strcmp(subsystem, "usb")==0 ||
         strcmp(subsystem, "pci")==0) )
    {
        if( is_add )
        {
            LOG_INFO(("Adding device @ sysfspath %s\n", sysfs_devpath));
            visit_device(sysfs_devpath, FALSE);
        }
        else
        {
            HalDevice* d;

            d = ds_device_find_by_key_value_string("Linux.sysfs_path",
                                                   sysfs_devpath);
            if( d==NULL )
            {
                LOG_WARNING(("Couldn't remove device @ %s on hotplug remove",
                             sysfs_devpath));
            }
            else
            {
                ds_device_destroy(d);
            }
        }
    }

    if( sysfs_devpath!=NULL && 
        (strcmp(subsystem, "net")==0 ||
         strcmp(subsystem, "block")==0 ||
         strcmp(subsystem, "scsi_host")==0 ||
         strcmp(subsystem, "scsi_device")==0) )
    {
        if( is_add )
        {
            LOG_INFO(("Adding device @ sysfspath %s\n", sysfs_devpath));
            visit_class_device(sysfs_devpath, FALSE);
        }
        else
        {
            HalDevice* d;
            d = ds_device_find_by_key_value_string("Linux.sysfs_path",
                                                   sysfs_devpath);
            if( d==NULL )
            {
                LOG_WARNING(("Couldn't remove device @ %s on hotplug remove",
                             sysfs_devpath));
            }
            else
            {
                ds_device_destroy(d);
            }
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Message handler for method invocations. All invocations on any object
 *  or interface is routed through this function.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @param  user_data           User data
 *  @return                     What to do with the message
 */
DBusHandlerResult osspec_filter_function(DBusConnection* connection,
                                         DBusMessage* message,
                                         void* user_data)
{
/*
    LOG_INFO(("obj_path=%s interface=%s method=%s", 
              dbus_message_get_path(message), 
              dbus_message_get_interface(message),
              dbus_message_get_member(message)));
*/

    if( dbus_message_is_method_call(
            message,
            "org.freedesktop.Hal.Linux.Hotplug",
            "HotplugEvent") &&
        strcmp(dbus_message_get_path(message), 
               "/org/freedesktop/Hal/Linux/Hotplug")==0 )
    {
        return osspec_hotplug(connection, message);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
