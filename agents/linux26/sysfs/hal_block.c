/***************************************************************************
 * CVSID: $Id$
 *
 * hal_block.c : Block device functions for sysfs-agent on Linux 2.6
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
#include <syslog.h>

#include "main.h"
#include "hal_block.h"

/** This function will compute the device uid based on other properties
 *  of the device. For block device it's simply block appended with the
 *  major and minor number
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* block_compute_udi(const char* udi, int append_num)
{
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/block_%d_%d";
    else
        format = "/org/freedesktop/Hal/devices/block_%d_%d-%d";

    snprintf(buf, 256, format, 
             hal_device_get_property_int(udi, "block.major"),
             hal_device_get_property_int(udi, "block.minor"),
             append_num);
    
    return buf;
}

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
    char* d;
    char* parent_udi;
    char attr_name[SYSFS_NAME_LEN];
    struct sysfs_attribute* cur;
    dbus_bool_t is_disk = FALSE;
    dbus_bool_t not_partition = FALSE;

    if( sysfs_get_classdev_attr(class_device, "dev")==NULL )
    {
        /* Must have major:minor number before we are interested */
        return;
    }

    if( class_device->sysdevice==NULL )
    {
        /* check if our parent is there; example: we are sda1 */
        parent_udi = find_parent_udi_from_sysfs_path(path, 
                                                   HAL_LINUX_HOTPLUG_TIMEOUT);
        if( parent_udi==NULL )
            return;

        /*
        printf("block: parent_udi=%s , path=%s\n", 
               parent_udi, path);
        */
    }
    else
    {
        /* Now find the corresponding physical device */
        parent_udi = find_udi_from_sysfs_path(class_device->sysdevice->path, 
                                              HAL_LINUX_HOTPLUG_TIMEOUT);
        if( parent_udi==NULL )
        {
            printf("No device corresponding to class device at path %s\n", path);
            return;
        }

        /*
        printf("block: sysdevice_path=%s, path=%s\n", 
               class_device->sysdevice->path, path);
        */
    }

    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "block");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", path);

    hal_device_add_capability(d, "block");

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
                hal_device_set_property_int(d, "block.major", major);
                hal_device_set_property_int(d, "block.minor", minor);
            }
        }
        else if( strcmp(attr_name, "size")==0 )
        {
            hal_device_set_property_int(d, "block.size",
                                        parse_dec(cur->value));
        }
        else if( strcmp(attr_name, "start")==0 )
        {
            hal_device_set_property_int(d, "block.start",
                                        parse_dec(cur->value));
        }
        else if( strcmp(attr_name, "range")==0 )
        {
            not_partition = TRUE;
        }
    }

    hal_device_set_property_bool(d, "block.isVolume", !not_partition);

    hal_device_set_property_string(d, "Parent", parent_udi);

    if( !not_partition )
    {
        /* We are a volume */
        find_and_set_physical_device(d);
        hal_device_set_property_bool(d, "isVirtual", TRUE);
        hal_device_add_capability(d, "volume");
        hal_device_set_property_string(d, "Category", "volume");

        /* block device that is a partition; e.g. a storage volume */

        /** @todo  Guestimate product name; use volume label */
        hal_device_set_property_string(d, "Product", "Volume");
    }
    else
    {
        /* We are a disk; maybe we even offer removable media */
        hal_device_set_property_string(d, "Category", "block");

        if( strcmp(hal_device_get_property_string(parent_udi, "Bus"), 
                   "ide")==0 )
        {
            const char* ide_name;
            char* model;
            char* media;
            dbus_bool_t removable_media;


            ide_name = get_last_element(path);
            
            model = read_single_line("/proc/ide/%s/model", ide_name);
            if( model!=NULL )
            {
                hal_device_set_property_string(d, "block.model", model);
                hal_device_set_property_string(d, "Product", model);
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
                hal_device_set_property_string(d, "block.media", media);

                /* Set for removable media */
                if( strcmp(media, "disk")==0 )
                {
                    hal_device_add_capability(d, "fixedMedia");
                    hal_device_add_capability(d, "fixedMedia.harddisk");
                    hal_device_set_property_string(d, "Category", 
                                                   "fixedMedia.harddisk");
                }
                else if( strcmp(media, "cdrom")==0 )
                {
                    hal_device_add_capability(d, "removableMedia");
                    hal_device_add_capability(d, "removableMedia.cdrom");
                    hal_device_set_property_string(d, "Category", 
                                                   "removableMedia.cdrom");
                    removable_media = TRUE;
                }
                else if( strcmp(media, "floppy")==0 )
                {
                    hal_device_add_capability(d, "removableMedia");
                    hal_device_add_capability(d, "removableMedia.floppy");
                    hal_device_set_property_string(d, "Category", 
                                                   "removableMedia.floppy");
                    removable_media = TRUE;
                }
                else if( strcmp(media, "tape")==0 )
                {
                    hal_device_add_capability(d, "removableMedia");
                    hal_device_add_capability(d, "removableMedia.tape");
                    hal_device_set_property_string(d, "Category", 
                                                   "removableMedia.tape");
                    removable_media = TRUE;
                }

            }

            hal_device_set_property_bool(d, "block.removableMedia", 
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

            hal_device_set_property_string(d, "block.media", "flash");

            hal_device_add_capability(d, "fixedMedia");
            hal_device_add_capability(d, "fixedMedia.flash");
            hal_device_set_property_string(d, "Category", 
                                           "fixedMedia.flash");
            
            /* guestimate product name */
            hal_device_set_property_string(d, "Product", "Disk");

            /* omit block.media! */
        }
    }

    d = rename_and_maybe_add(d, block_compute_udi, "block");
}

/** Init function for block device handling
 *
 */
void hal_block_init()
{
}

/** Shutdown function for block device handling
 *
 */
void hal_block_shutdown()
{

}

