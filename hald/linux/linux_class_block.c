/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_block.c : Block device handling on Linux 2.6
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
#include "linux_class_block.h"

/** This function will compute the device uid based on other properties
 *  of the device. For block device it's simply block appended with the
 *  major and minor number
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* block_compute_udi(HalDevice* d, int append_num)
{
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/block_%d_%d";
    else
        format = "/org/freedesktop/Hal/devices/block_%d_%d-%d";

    snprintf(buf, 256, format, 
             ds_property_get_int(d, "block.major"),
             ds_property_get_int(d, "block.minor"),
             append_num);
    
    return buf;
}

/* fwd decl */
static void visit_class_device_block_got_parent(HalDevice* parent, 
                                                void* data1, void* data2);

/** Visitor function for block device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_class_device_block(const char* path, 
                              struct sysfs_class_device* class_device)
{
    HalDevice* d;
    char* parent_sysfs_path;
    char attr_name[SYSFS_NAME_LEN];
    struct sysfs_attribute* cur;
    dbus_bool_t is_disk = FALSE;
    dbus_bool_t not_partition = FALSE;

    if( sysfs_get_classdev_attr(class_device, "dev")==NULL )
    {
        /* Must have major:minor number before we are interested */
        /*LOG_INFO(("Block device with sysfs path %s doesn't have major:minor",
          path));*/
        return;
    }

    d = ds_device_new();
    ds_property_set_string(d, "Bus", "block");
    ds_property_set_string(d, "Linux.sysfs_path", path);
    ds_property_set_string(d, "Linux.sysfs_path_device", path);

    ds_add_capability(d, "block");

    dlist_for_each_data(sysfs_get_classdev_attributes(class_device), cur,
                        struct sysfs_attribute)
    {
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        if( strcmp(attr_name, "dev")==0 )
        {
            int major, minor;
            if( sscanf(cur->value, "%d:%d", &major, &minor)==2 )
            {
                is_disk = TRUE;
                ds_property_set_int(d, "block.major", major);
                ds_property_set_int(d, "block.minor", minor);
            }
        }
        else if( strcmp(attr_name, "size")==0 )
        {
            ds_property_set_int(d, "block.size",
                                        parse_dec(cur->value));
        }
        else if( strcmp(attr_name, "start")==0 )
        {
            ds_property_set_int(d, "block.start",
                                        parse_dec(cur->value));
        }
        else if( strcmp(attr_name, "range")==0 )
        {
            not_partition = TRUE;
        }
    }
    ds_property_set_bool(d, "block.isVolume", !not_partition);


    if( class_device->sysdevice==NULL )
    {
        /* there is no sys device corresponding to us.. this means we
         * must be the child of another block device ie. a partition */

        /* check if our parent is there; example: if we are sda1 then
         * our parent is sda
         */

        parent_sysfs_path = get_parent_sysfs_path(path);
    }
    else
    {
        /* There is a sys device corresponding to us; This is normally an
         * IDE interface or a SCSI device */

        /* Now find the corresponding physical device */

        parent_sysfs_path = class_device->sysdevice->path;
    }

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string("Linux.sysfs_path_device",
                                             parent_sysfs_path, 
                                         visit_class_device_block_got_parent,
                                             (void*) d, NULL, 
                                             is_probing ? 0 :
                                             HAL_LINUX_HOTPLUG_TIMEOUT);
}

/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void visit_class_device_block_got_parent(HalDevice* parent, 
                                                void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    if( parent==NULL )
    {
        LOG_ERROR(("No parent for block device!"));
        ds_device_destroy(d);
        return;
    }

    ds_property_set_string(d, "Parent", parent->udi);

    if( ds_property_get_bool(d, "block.isVolume") )
    {
        /* We are a volume */
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "isVirtual", TRUE);
        ds_add_capability(d, "volume");
        ds_property_set_string(d, "Category", "volume");

        /* block device that is a partition; e.g. a storage volume */

        /** @todo  Guestimate product name; use volume label */
        ds_property_set_string(d, "Product", "Volume");
    }
    else
    {
        /* We are a disk; maybe we even offer removable media */
        ds_property_set_string(d, "Category", "block");

        if( strcmp(ds_property_get_string(parent, "Bus"), "ide")==0 )
        {
            const char* ide_name;
            char* model;
            char* media;
            dbus_bool_t removable_media;

            ide_name = get_last_element(
                ds_property_get_string(d, "Linux.sysfs_path"));
            
            model = read_single_line("/proc/ide/%s/model", ide_name);
            if( model!=NULL )
            {
                ds_property_set_string(d, "block.model", model);
                ds_property_set_string(d, "Product", model);
            }

            removable_media = FALSE;
            
            /* According to the function proc_ide_read_media() in 
             * drivers/ide/ide-proc.c in the Linux sources, media
             * can only assume "disk", "cdrom", "tape", "floppy", "UNKNOWN"
             */

            /** @todo Given media 'cdrom', how do we determine whether
             *        it's a DVD or a CD-RW or both? Given floppy how
             *        do we determine it's LS120?
             */

            media = read_single_line("/proc/ide/%s/media", ide_name);
            if( media!=NULL )
            {
                ds_property_set_string(d, "block.media", media);

                /* Set for removable media */
                if( strcmp(media, "disk")==0 )
                {
                    ds_add_capability(d, "fixedMedia");
                    ds_add_capability(d, "fixedMedia.harddisk");
                    ds_property_set_string(d, "Category", 
                                                   "fixedMedia.harddisk");
                }
                else if( strcmp(media, "cdrom")==0 )
                {
                    ds_add_capability(d, "removableMedia");
                    ds_add_capability(d, "removableMedia.cdrom");
                    ds_property_set_string(d, "Category", 
                                                   "removableMedia.cdrom");
                    removable_media = TRUE;
                }
                else if( strcmp(media, "floppy")==0 )
                {
                    ds_add_capability(d, "removableMedia");
                    ds_add_capability(d, "removableMedia.floppy");
                    ds_property_set_string(d, "Category", 
                                                   "removableMedia.floppy");
                    removable_media = TRUE;
                }
                else if( strcmp(media, "tape")==0 )
                {
                    ds_add_capability(d, "removableMedia");
                    ds_add_capability(d, "removableMedia.tape");
                    ds_property_set_string(d, "Category", 
                                                   "removableMedia.tape");
                    removable_media = TRUE;
                }

            }

            ds_property_set_bool(d, "block.removableMedia", 
                                         removable_media);
            
        }
        else
        {
            /** @todo: block device on non-IDE device; how to find the
             *         name and the media-type? Right now we just assume
             *         that the disk is fixed and of type flash.. This hack
             *         does 'the right thing' on IDE-systems where the
             *         user attach a USB storage device.
             *
             *         We should special case for at least SCSI once this
             *         information is easily accessible in kernel 2.6.
             *       
             */

            ds_property_set_string(d, "block.media", "flash");

            ds_add_capability(d, "fixedMedia");
            ds_add_capability(d, "fixedMedia.flash");
            ds_property_set_string(d, "Category", 
                                           "fixedMedia.flash");
            
            /* guestimate product name */
            ds_property_set_string(d, "Product", "Disk");

            /* omit block.media! */
        }
    }

    rename_and_maybe_add(d, block_compute_udi, "block");
}

/** Init function for block device handling
 *
 */
void linux_class_block_init()
{
}

/** Shutdown function for block device handling
 *
 */
void linux_class_block_shutdown()
{

}

