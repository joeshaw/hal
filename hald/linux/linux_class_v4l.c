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

/* fwd decl */
static void visit_class_device_v4l_got_sysdevice(HalDevice* parent, 
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
    char* sysdevice_sysfs_path;
	char* product_name = NULL;
    char attr_name[SYSFS_NAME_LEN];
    char v4l_class[32];
    const char* last_elem;
    int adapter_num;
    int len;
    int i;
    char buf[256];
    char dev_file[256];

    if( class_device->sysdevice==NULL )
    {
        HAL_INFO(("Skipping virtual class device at path %s\n", path));
        return;
    }

    HAL_INFO(("v4l: sysdevice_path=%s, path=%s\n", class_device->sysdevice->path, path));

    /** @todo: see if we already got this device */

    /* Sets last_elem to radio0 in path=/sys/class/video4linux/radio0 */
    last_elem = get_last_element(path);
    sscanf(last_elem, "%32[a-z]%d", v4l_class, &adapter_num);

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

    /* Ask udev about the device file if we are probing.. otherwise we'll just
     * receive a dbus message from udev later */
    dev_file[0] = '\0';
    if( is_probing )
    {
        int i;
        int sysfs_mount_path_len;
        char sysfs_path_trunc[SYSFS_NAME_LEN];
        char* udev_argv[7] = {udevinfo_path(), "-r", "-q", "name", "-p",
                              sysfs_path_trunc, NULL};
        char* udev_stdout;
        char* udev_stderr;
        int udev_exitcode;

        /* compute truncated sysfs path */
        sysfs_mount_path_len = strlen(sysfs_mount_path);
        if( strlen(path)>sysfs_mount_path_len )
        {
            strncpy(sysfs_path_trunc, path + sysfs_mount_path_len, 
                    SYSFS_NAME_LEN);
        }
        HAL_INFO(("*** sysfs_path_trunc = '%s'", sysfs_path_trunc));
        
        /* Now invoke udev */
        if( udev_argv[0] == NULL || g_spawn_sync("/",
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
            HAL_ERROR(("Couldn't invoke udevinfo"));
            goto error;
        }
        
        if( udev_exitcode!=0 )
        {
            HAL_ERROR(("udevinfo returned %d", udev_exitcode));
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

        strncat(dev_file, udev_stdout, 256);
                
        /** @todo FIXME free udev_stdout, udev_stderr? */        
    }
    else
    {
error:
    }

    HAL_INFO(("device file = '%s'", dev_file));

    d = ds_device_new();
    ds_add_capability(d, "v4l");
    ds_property_set_string(d, "info.category", "v4l");
    if( strcmp(v4l_class, "vbi")==0 )
    {
        ds_property_set_string(d, "v4l.vbi.linux.sysfs_path", path);
        ds_property_set_int(d, "v4l.vbi.adapter", adapter_num);
        ds_property_set_string(d, "v4l.vbi.device", dev_file);

        if ( product_name==NULL )
            ds_property_set_string(d, "v4l.vbi.name", "V4L vbi");
        else
            ds_property_set_string(d, "v4l.vbi.name", product_name);

        ds_add_capability(d, "v4l.vbi");
    }
    else if( strcmp(v4l_class, "video")==0 )
    {
        ds_property_set_string(d, "v4l.video.linux.sysfs_path", path);
        ds_property_set_int(d, "v4l.video.adapter", adapter_num);
        ds_property_set_string(d, "v4l.video.device", dev_file);

        if ( product_name==NULL )
            ds_property_set_string(d, "v4l.video.name", "V4L video");
        else
            ds_property_set_string(d, "v4l.video.name", product_name);

        ds_add_capability(d, "v4l.video");
    }
    else if( strcmp(v4l_class, "radio")==0 )
    {
        ds_property_set_string(d, "v4l.radio.linux.sysfs_path", path);
        ds_property_set_int(d, "v4l.radio.adapter", adapter_num);
        ds_property_set_string(d, "v4l.radio.device", dev_file);

        if ( product_name==NULL )
            ds_property_set_string(d, "v4l.radio.name", "V4L radio");
        else
            ds_property_set_string(d, "v4l.radio.name", product_name);

        ds_add_capability(d, "v4l.radio");
    }
 
    sysdevice_sysfs_path = class_device->sysdevice->path;

    /* Find sysdevice; this happens asynchronously as our sysdevice might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "linux.sysfs_path_device",
        sysdevice_sysfs_path, 
        TRUE,
        visit_class_device_v4l_got_sysdevice,
        (void*) d, NULL, 
        is_probing ? 0 :
        HAL_LINUX_HOTPLUG_TIMEOUT);

    free(sysdevice_sysfs_path);
}

/** Callback when the sysdevice is found or if there is no sysdevice.. This is
 *  where we get added to the GDL..
 *
 *  @param  sysdevice              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void visit_class_device_v4l_got_sysdevice(HalDevice* sysdevice, 
                                                 void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    /*printf("sysdevice=0x%08x\n", sysdevice);*/

    if( sysdevice==NULL )
    {
        HAL_ERROR(("No sysdevice for V4L device!"));
        ds_device_destroy(d);
        return;
    }
    else
    {
        ds_device_merge(sysdevice, d);
    }

    ds_device_destroy(d);
}

/** Init function for V4L adapter class handling
 *
 */
void linux_class_v4l_init()
{
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
