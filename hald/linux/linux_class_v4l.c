/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_v4l.c : V4L functions for sysfs-agent on Linux 2.6
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

#include <glib.h>

#include "../logger.h"
#include "../device_store.h"
#include "linux_class_v4l.h"

/**
 * @defgroup HalDaemonLinuxV4lAdapter V4L adapter class
 * @ingroup HalDaemonLinux
 * @brief V4L class
 * @{
 */

/** udev root directory, e.g. "/udev/" */
static char udev_root[256];

/** This function will compute the device uid based on other properties
 *  of the device. For V4L devices it is the adapter number.
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* v4l_compute_udi(HalDevice* d, int append_num)
{
    const char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/video4linux_%s_%d";
    else
        format = "/org/freedesktop/Hal/devices/video4linux_%s_%d-%d";

    snprintf(buf, 256, format, 
             ds_property_get_string(d, "video4linux.class"),
             ds_property_get_int(d, "video4linux.adapter"),
             append_num);
    
    return buf;
}


/* fwd decl */
static void visit_class_device_v4l_got_parent(HalDevice* parent, 
                                              void* data1, void* data2);

/** Visitor function for V4L devices.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_class_device_v4l(const char* path, 
                            struct sysfs_class_device* class_device)
{
    HalDevice* d;
    struct sysfs_attribute* cur;
    char* parent_sysfs_path;
	char* product_name = NULL;
    char attr_name[SYSFS_NAME_LEN];
    char v4l_class[32];
    const char* last_elem;
    int adapter_num;
    int len;
    int i;

    if( class_device->sysdevice==NULL )
    {
        HAL_INFO(("Skipping virtual class device at path %s\n", path));
        return;
    }

    HAL_INFO(("v4l: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path));

    /** @todo: see if we already got this device */

    d = ds_device_new();
    ds_property_set_string(d, "info.bus", "video4linux");
    ds_property_set_string(d, "linux.sysfs_path", path);
    ds_property_set_string(d, "linux.sysfs_path_device", path);

    /* Sets last_elem to radio0 in path=/sys/class/video4linux/radio0 */
    last_elem = get_last_element(path);
    sscanf(last_elem, "%32[a-z]%d", v4l_class, &adapter_num);
    ds_property_set_int(d, "video4linux.adapter", adapter_num);
    ds_property_set_string(d, "video4linux.class", v4l_class);

    /* guestimate product name */
    dlist_for_each_data(sysfs_get_classdev_attributes(class_device), cur,
                        struct sysfs_attribute)
    {       
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;
        
        /* strip whitespace */
        len = strlen(cur->value);
        for(i=len-1; isspace(cur->value[i]); --i)
            cur->value[i] = '\0';
        
        /*printf("attr_name=%s -> '%s'\n", attr_name, cur->value);*/
        
        if( strcmp(attr_name, "name")==0 )
           product_name = cur->value;
    }

    if ( product_name==NULL )
        ds_property_set_string(d, "video4linux.name", "Video4Linux Interface");
    else
        ds_property_set_string(d, "video4linux.name", product_name);

    /* More info available from http://bytesex.org/v4l/spec-single/v4l2.html */
    if (strcmp("radio", v4l_class) == 0)
        ds_property_set_string(d, "info.product", "Video4Linux Radio Interface");
    else if (strcmp("video", v4l_class) == 0)
        ds_property_set_string(d, "info.product", "Video4Linux Video Input Interface");
    else if (strcmp("vout", v4l_class) == 0)
        ds_property_set_string(d, "info.product", "Video4Linux Video Output Interface");
    else if (strcmp("vbi", v4l_class) == 0)
        ds_property_set_string(d, "info.product", "Video4Linux VBI Interface");
    else 
        ds_property_set_string(d, "info.product", "Video4Linux Interface");

    /* Ask udev about the device file if we are probing.. otherwise we'll just
     * receive a dbus message from udev later */
    if( is_probing )
    {
        int i;
        int sysfs_mount_path_len;
        char sysfs_path_trunc[SYSFS_NAME_LEN];
        char* udev_argv[4] = {"/sbin/udev", "-q", 
                              sysfs_path_trunc, NULL};
        char* udev_stdout;
        char* udev_stderr;
        int udev_exitcode;
        char dev_file[256];

        /* compute truncated sysfs path */
        sysfs_mount_path_len = strlen(sysfs_mount_path);
        if( strlen(path)>sysfs_mount_path_len )
        {
            strncpy(sysfs_path_trunc, path + sysfs_mount_path_len, 
                    SYSFS_NAME_LEN);
        }
        HAL_INFO(("*** sysfs_path_trunc = '%s'", sysfs_path_trunc));
        
        /* Now invoke udev */
        if( g_spawn_sync("/",
                         udev_argv,
                         NULL,
                         0,
                         NULL,
                         NULL,
                         &udev_stdout,
                         &udev_stderr,
                         &udev_exitcode,
			 NULL)!=TRUE )
        {
            HAL_ERROR(("Couldn't invoke /sbin/udev"));
            goto error;
        }
        
        if( udev_exitcode!=0 )
        {
            HAL_ERROR(("/sbin/udev returned %d", udev_exitcode));
            goto error;
        }
        
        /* sanitize string returned by udev */
        for(i=0; udev_stdout[i]!=0; i++)
        {
            if( udev_stdout[i]=='\r' || udev_stdout[i]=='\n' )
            {
                udev_stdout[i]=0;
                break;
            }
        }

        strncpy(dev_file, udev_root, 256);
        strncat(dev_file, udev_stdout, 256);
        
        HAL_INFO(("device file = '%s'", dev_file));
        
        ds_property_set_string(d, "video4linux.device", dev_file);
        
        /** @todo FIXME free udev_stdout, udev_stderr? */        
    }
    else
    {
error:
        ds_property_set_string(d, "video4linux.device", "");
    }
 
    parent_sysfs_path = class_device->sysdevice->path;

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "linux.sysfs_path_device",
        parent_sysfs_path, 
        TRUE,
        visit_class_device_v4l_got_parent,
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
static void visit_class_device_v4l_got_parent(HalDevice* parent, 
                                              void* data1, void* data2)
{
    char v4l_capability[SYSFS_NAME_LEN];
    char* new_udi = NULL;
    char* parent_driver;
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
        HAL_ERROR(("No parent for V4L device!"));
        ds_device_destroy(d);
        return;
    }

    /* Add the appropriate v4l capability to our parent device */
    snprintf(v4l_capability, SYSFS_NAME_LEN, "multimedia.%s", ds_property_get_string(d, "video4linux.class"));
	ds_add_capability(parent, v4l_capability);

    /* Use the driver from our parent */
    parent_driver = ds_property_get_string(parent, "linux.driver");
    if ( parent_driver != NULL )
        ds_property_set_string(d, "linux.driver", parent_driver);

    /* Compute a proper UDI (unique device id) and try to locate a persistent
     * unplugged device or simple add this new device...
     */
    new_udi = rename_and_merge(d, v4l_compute_udi, "video4linux");
    if( new_udi!=NULL )
    {
        new_d = ds_device_find(new_udi);
        if( new_d!=NULL )
        {
            ds_gdl_add(new_d);
        }
    }
}

/** Find udev root directory (e.g. '/udev/') by invoking '/sbin/udev -r'.
 *  If this fails, we default to /udev/.
 *
 */
static void get_udev_root()
{
    int i;
    char* udev_argv[3] = {"/sbin/udev", "-r", NULL};
    char* udev_stdout;
    char* udev_stderr;
    int udev_exitcode;

    strncpy(udev_root, "/udev/", 256);

    /* Invoke udev */
    if( g_spawn_sync("/",
                     udev_argv,
                     NULL,
                     0,
                     NULL,
                     NULL,
                     &udev_stdout,
                     &udev_stderr,
                     &udev_exitcode,
		     NULL)!=TRUE )
    {
        HAL_ERROR(("Couldn't invoke /sbin/udev -r"));
        return;
    }
    
    if( udev_exitcode!=0 )
    {
        HAL_ERROR(("/sbin/udev -r returned %d", udev_exitcode));
        return;
    }
    
    /* sanitize string returned by udev */
    for(i=0; udev_stdout[i]!=0; i++)
    {
        if( udev_stdout[i]=='\r' || udev_stdout[i]=='\n' )
        {
            udev_stdout[i]=0;
            break;
        }
    }
    
    strncpy(udev_root, udev_stdout, 256);
        
    HAL_INFO(("udev root = '%s'", udev_root));
        
    /** @todo FIXME free udev_stdout, udev_stderr? */        
}

/** Init function for V4L adapter class handling
 *
 */
void linux_class_v4l_init()
{
    get_udev_root();
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void linux_class_v4l_detection_done()
{
}

/** Shutdown function for V4L adapter class handling
 *
 */
void linux_class_v4l_shutdown()
{
}

/** @} */
