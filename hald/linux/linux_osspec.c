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
#include "linux_i2c.h"
#include "linux_usb.h"
#include "linux_ide.h"
#include "linux_class_block.h"
#include "linux_class_scsi.h"
#include "linux_class_i2c_adapter.h"
#include "linux_class_v4l.h"
#include "linux_class_net.h"
#include "linux_class_input.h"

#include "libsysfs/libsysfs.h"

/**
 * @defgroup HalDaemonLinux Linux 2.6 support
 * @ingroup HalDaemon
 * @brief Device detection and monitoring code for version 2.6 of Linux
 * @{
 */


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
    else if( strcmp(class_device->classname, "i2c-adapter")==0 )
        visit_class_device_i2c_adapter(path, class_device);
    else if( strcmp(class_device->classname, "video4linux")==0 )
        visit_class_device_v4l(path, class_device);
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
        HAL_ERROR(("Error opening class %s\n", class_name));
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
    struct sysfs_class* cls;
    struct sysfs_class_device* class_device;

    device = sysfs_open_device(path);
    if( device==NULL )
        DIE(("Coulnd't get sysfs device object for path %s", path));

/*
    printf("############\n");
    printf("############    %s  busid=%s\n", device->bus, device->bus_id);
    printf("############\n");
*/

    if( device->bus!=NULL )
    {
        if( strcmp(device->bus, "pci")==0 )
            visit_device_pci(path, device);
        else if( strcmp(device->bus, "usb")==0 )
            visit_device_usb(path, device);
/*
        else if( strcmp(device->bus, "ieee1394")==0 )
            visit_device_ieee1394(path, device);
*/
        else if( strcmp(device->bus, "ide")==0 )
            visit_device_ide(path, device);
        /** @todo This is a hack; is there such a thing as an ide_host? */
        else if( strncmp(device->bus_id, "ide", 3)==0 )
            visit_device_ide_host(path, device);
        else if( strcmp(device->bus, "i2c")==0 )
            visit_device_i2c(path, device);
		else if( strncmp(device->bus_id, "i2c", 3)==0 )
        {
            /* @todo FIXME we need to add the i2c-adapter class
             * devices here, otherwise the I2C devices have nowhere to
             * go
             */
            cls = sysfs_open_class("i2c-adapter");
            if (cls != NULL)
            {
                if( cls->devices!=NULL )
                {
                    dlist_for_each_data(cls->devices, class_device, struct sysfs_class_device)
                    {
                       printf("device->bus_id = %s, cls->name = %s\n", device->bus_id, class_device->name);
                       if ( strcmp(device->bus_id, class_device->name) == 0 )
                            visit_class_device_i2c_adapter(class_device->path, class_device);
                    }
                }
                sysfs_close_class(cls);
            }
        }
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
    DBusError error;

    /* get mount path for sysfs */
    rc = sysfs_get_mnt_path(sysfs_mount_path, SYSFS_PATH_MAX);
    if( rc!=0 )
    {
        DIE(("Couldn't get mount path for sysfs"));
    }
    HAL_INFO(("Mountpoint for sysfs is %s", sysfs_mount_path));

    linux_pci_init();
    linux_usb_init();
    /*linux_ieee1394_init();*/
    linux_ide_init();
    linux_class_v4l_init();
    linux_class_scsi_init();
    linux_class_block_init();
    linux_class_net_init();

    /* Add match for signals from udev */
    dbus_error_init(&error);
    dbus_bus_add_match(dbus_connection, 
                       "type='signal',"
                       "interface='org.kernel.udev.NodeMonitor',"
                       /*"sender='org.kernel.udev'," until D-BUS is fixed*/
                       "path='/org/kernel/udev/NodeMonitor'", &error);
    if( dbus_error_is_set(&error) )
    {
        HAL_WARNING(("Cannot subscribe to udev signals, error=%s", 
                     error.message));
    }
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

    /* visit class devices in /sys/class/video4linux */
    visit_class("video4linux", TRUE);

    /* visit class devices in /sys/class/scsi_host */
    visit_class("scsi_host", FALSE);

    /* visit class devices in /sys/class/scsi_host */
    visit_class("scsi_device", FALSE);

    /* visit all block devices */
    visit_class("block", TRUE);

    /* visit all net devices */
    visit_class("net", TRUE);

    /* input devices are not yet in sysfs */
    linux_class_input_probe();

    is_probing = FALSE;

    /* Notify various device and class types that detection is done, so 
     * they can do some (optional) batch processing
     */
    linux_pci_detection_done();
    linux_usb_detection_done();
    /*linux_ieee1394_detection_done();*/
    linux_ide_detection_done();
    linux_class_block_detection_done();
    linux_class_input_detection_done();
    linux_class_net_detection_done();
    linux_class_scsi_detection_done();
}

/** Maximum length of string parameter in hotplug input events */
#define HOTPLUG_INPUT_MAX 128

/** Handle a org.freedesktop.Hal.HotplugEvent message. This message
 *  origins from the hal.hotplug program, tools/linux/hal_hotplug.c,
 *  and is basically just a D-BUS-ification of the hotplug event.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult handle_hotplug(DBusConnection* connection,
                                        DBusMessage* message)
{
    DBusMessageIter iter;
    DBusMessageIter dict_iter;
    dbus_bool_t is_add;
    char* subsystem;
    char input_name[HOTPLUG_INPUT_MAX];
    char input_phys[HOTPLUG_INPUT_MAX];
    char input_key[HOTPLUG_INPUT_MAX];
    int input_ev=0;
    int input_rel=0;
    int input_abs=0;
    int input_led=0;
    char sysfs_devpath[SYSFS_PATH_MAX];

    sysfs_devpath[0] = '\0';
    input_name[0] = input_phys[0] = input_key[0] = '\0';

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

    is_add = FALSE;

    for( ;dbus_message_iter_has_next(&dict_iter); 
         dbus_message_iter_next(&dict_iter))
    {
        char* key;
        char* value;
        
        key = dbus_message_iter_get_dict_key(&dict_iter);
        value = dbus_message_iter_get_string(&dict_iter);

        /*printf("  key/value = %s |-> %s\n", key, value);*/

        if( strcmp(key, "ACTION")==0 )
        {
            if( strcmp(value, "add")==0 )
            {
                is_add = TRUE;
            }
        }
        else if( strcmp(key, "DEVPATH")==0 )
        {
            strncpy(sysfs_devpath, sysfs_mount_path, SYSFS_PATH_MAX);
            strncat(sysfs_devpath, value, SYSFS_PATH_MAX);
        }
        else if( strcmp(key, "NAME")==0 )
            strncpy(input_name, value, HOTPLUG_INPUT_MAX);
        else if( strcmp(key, "PHYS")==0 )
            strncpy(input_phys, value, HOTPLUG_INPUT_MAX);
        else if( strcmp(key, "KEY")==0 )
            strncpy(input_key, value, HOTPLUG_INPUT_MAX);
        else if( strcmp(key, "EV")==0 )
            input_ev = parse_dec(value);
        else if( strcmp(key, "REL")==0 )
            input_rel = parse_hex(value);
        else if( strcmp(key, "ABS")==0 )
            input_abs = parse_hex(value);
        else if( strcmp(key, "LED")==0 )
            input_led = parse_hex(value);
    }

    HAL_INFO(("HotplugEvent %s, subsystem=%s devpath=%s", 
              (is_add ? "add" : "remove"), subsystem,
              sysfs_devpath[0]!='\0' ? sysfs_devpath : "(none)"));

    if( sysfs_devpath[0]!='\0' && 
        (strcmp(subsystem, "usb")==0 ||
         strcmp(subsystem, "pci")==0 ||
         /*strcmp(subsystem, "ieee1394")==0 ||*/
         strcmp(subsystem, "i2c")==0))
    {

        if( is_add )
        {
            HAL_INFO(("Adding device @ sysfspath %s", sysfs_devpath));
            visit_device(sysfs_devpath, FALSE);
        }
        else
        {
            HalDevice* d;

            d = ds_device_find_by_key_value_string("linux.sysfs_path",
                                                   sysfs_devpath, TRUE);
            if( d==NULL )
            {
                HAL_WARNING(("Couldn't remove device @ %s on hotplug remove",
                             sysfs_devpath));
            }
            else
            {
                HAL_INFO(("Removing device @ sysfspath %s, udi %s", 
                          sysfs_devpath, d->udi));

                if( ds_property_exists(d, "info.persistent") &&
                    ds_property_get_bool(d, "info.persistent") )
                {
                    ds_property_set_bool(d, "info.not_available", TRUE);
                    /* Remove enough specific details so we are not found
                     * by child devices when being plugged in again.. 
                     */
                    ds_property_remove(d, "info.parent");
                    ds_property_remove(d, "info.physical_device");
                    ds_property_remove(d, "linux.sysfs_path");
                    ds_property_remove(d, "linux.sysfs_path_device");
                    HAL_INFO(("Device %s is persistent, so not removed",
                              d->udi));
                }
                else
                {
                    ds_device_destroy(d);
                }
            }
        }
    }
    else if( sysfs_devpath[0]!='\0' && 
             (strcmp(subsystem, "net")==0 ||
              strcmp(subsystem, "block")==0 ||
              strcmp(subsystem, "scsi_host")==0 ||
              strcmp(subsystem, "scsi_device")==0 ||
              strcmp(subsystem, "i2c-adapter")==0 ||
              strcmp(subsystem, "video4linux")==0 ))
    {
        if( is_add )
        {
            HAL_INFO(("Adding classdevice @ sysfspath %s", sysfs_devpath));
            visit_class_device(sysfs_devpath, FALSE);
        }
        else
        {
            HalDevice* d;
            d = ds_device_find_by_key_value_string("linux.sysfs_path",
                                                   sysfs_devpath, TRUE);
            if( d==NULL )
            {
                HAL_WARNING(("Couldn't remove device @ %s on hotplug remove",
                             sysfs_devpath));
            }
            else
            {
                if( strcmp(subsystem, "block")==0 )
                    linux_class_block_removed(d);

                HAL_INFO(("Removing classdevice @ sysfspath %s, udi %s", 
                          sysfs_devpath, d->udi));
                ds_device_destroy(d);
            }
        }
    }
    else if( strcmp(subsystem, "input")==0 )
    {
        if( is_add )
        {
            /** @todo FIXME when kernel 2.6 got input devices in sysfs
             *        this terrible hack can be removed
             */
            linux_class_input_handle_hotplug_add(input_name, input_phys, 
                                                 input_key, 
                                                 input_ev, input_rel, 
                                                 input_abs, input_led);
        }
        else
        {
            /* This block is left intentionally blank, since input class
             * devices simply attach to other (physical) devices and when
             * they disappear, the input part also goes away
             */
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* fwd decl */
static void handle_udev_node_created_found_device(HalDevice* d, 
                                                  void* data1, void* data2);

/** Handle a org.freedesktop.Hal.HotplugEvent message. This message
 *  origins from the hal.hotplug program, tools/linux/hal_hotplug.c,
 *  and is basically just a D-BUS-ification of the hotplug event.
 *
 *  @param  connection          D-BUS connection
 *  @param  message             Message
 *  @return                     What to do with the message
 */
static DBusHandlerResult handle_udev_node_created(DBusConnection* connection,
                                                  DBusMessage* message)
{
    char* filename;
    char* sysfs_path;
    char sysfs_dev_path[SYSFS_PATH_MAX];

    if( dbus_message_get_args(message, NULL, 
                              DBUS_TYPE_STRING, &filename,
                              DBUS_TYPE_STRING, &sysfs_path,
                              DBUS_TYPE_INVALID) )
    {
        strncpy(sysfs_dev_path, sysfs_mount_path, SYSFS_PATH_MAX);
        strncat(sysfs_dev_path, sysfs_path, SYSFS_PATH_MAX);
        HAL_INFO(("udev NodeCreated: %s %s\n", filename, sysfs_dev_path));
        
        /* Find block device; this happens asynchronously as our it might
         * be added later..
         */
        ds_device_async_find_by_key_value_string(
            "linux.sysfs_path_device",
            sysfs_dev_path, 
            FALSE, /* note: it doesn't need to be in the GDL */
            handle_udev_node_created_found_device,
            (void*) filename, NULL, 
            HAL_LINUX_HOTPLUG_TIMEOUT);

        /* NOTE NOTE NOTE: we will free filename in async result function */

        dbus_free(sysfs_path);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** Callback when the block device is found or if there is none..
 *
 *  @param  d                   Async Return value from the find call
 *  @param  data1               User data, in this case the filename
 *  @param  data2               User data
 */
static void handle_udev_node_created_found_device(HalDevice* d, 
                                                  void* data1, void* data2)
{
    char* filename = (char*) data1;

    if( d!=NULL )
    {
        HAL_INFO(("Setting block.device=%s for udi=%s", filename, d->udi));
        ds_property_set_string(d, "block.device", filename);
        linux_class_block_check_if_ready_to_add(d);
    }
    else
    {
        HAL_WARNING(("No HAL device corresponding to device %s", filename));
    }

    dbus_free(filename);
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
    HAL_INFO(("obj_path=%s interface=%s method=%s", 
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
        return handle_hotplug(connection, message);
    }
    else if( dbus_message_is_signal(message, 
                                    "org.kernel.udev.NodeMonitor",
                                    "NodeCreated") )
    {
        return handle_udev_node_created(connection, message);
    }
    else if( dbus_message_is_signal(message, "org.kernel.udev.NodeMonitor",
                                    "NodeDeleted") )
    {
        /* This is left intentionally blank since this it means that a
         * block device is removed and we'll catch that other places
         */
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** @} */
