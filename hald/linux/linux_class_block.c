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
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#define _GNU_SOURCE 1
#include <linux/fcntl.h>
#include <linux/kdev_t.h>


#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"
#include "linux_class_block.h"

/* fwd decl */
static void etc_mtab_process_all_block_devices(dbus_bool_t force);

/**
 * @defgroup HalDaemonLinuxBlock Block class
 * @ingroup HalDaemonLinux
 * @brief Block class
 * @{
 */

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
        /*HAL_INFO(("Block device with sysfs path %s doesn't have major:minor",
          path));*/
        return;
    }

    d = ds_device_new();
    ds_property_set_string(d, "info.bus", "block");
    ds_property_set_string(d, "linux.sysfs_path", path);
    ds_property_set_string(d, "linux.sysfs_path_device", path);

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
    ds_property_set_bool(d, "block.is_volume", !not_partition);

    /** @todo FIXME is a block always 512 bytes?? Must check kernel source */
    ds_property_set_int(d, "block.block_size", 512);

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
    ds_device_async_find_by_key_value_string("linux.sysfs_path_device",
                                             parent_sysfs_path, 
                                             TRUE,
                                         visit_class_device_block_got_parent,
                                             (void*) d, 
                                            /*(void*) parent_sysfs_path*/NULL, 
                                             is_probing ? 0 :
                                             HAL_LINUX_HOTPLUG_TIMEOUT);

    /*HAL_INFO(("*** finding parent_sysfs_path=%s, 0x%08x, callback=0x%08x", 
              parent_sysfs_path, parent_sysfs_path, 
              visit_class_device_block_got_parent));
    */
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
    char* new_udi = NULL;
    HalDevice* new_d = NULL;
    HalDevice* d = (HalDevice*) data1;

    HAL_INFO(("data2=0x%08x, d=0x%08x, d->udi=%s, parent->udi=%s, parent->in_gdl=%d", data2, d, d->udi, parent!=NULL ? parent->udi : "no parent", parent!=NULL ? parent->in_gdl : 42 ));

    if( parent==NULL )
    {
        HAL_WARNING(("No parent for block device!"));
        ds_device_destroy(d);
        return;
    }

    ds_property_set_string(d, "info.parent", parent->udi);

    if( ds_property_get_bool(d, "block.is_volume") )
    {
        /* We are a volume */
        find_and_set_physical_device(d);
        ds_property_set_bool(d, "info.virtual", TRUE);
        ds_add_capability(d, "volume");
        ds_property_set_string(d, "info.category", "volume");

        /* block device that is a partition; e.g. a storage volume */

        /** @todo  Guestimate product name; use volume label */
        ds_property_set_string(d, "info.product", "Volume");

    }
    else
    {
        /* We are a disk; maybe we even offer removable media */
        ds_property_set_string(d, "info.category", "block");

        if( strcmp(ds_property_get_string(parent, "info.bus"), "ide")==0 )
        {
            const char* ide_name;
            char* model;
            char* media;
            dbus_bool_t removable_media;

            ide_name = get_last_element(
                ds_property_get_string(d, "linux.sysfs_path"));
            
            model = read_single_line("/proc/ide/%s/model", ide_name);
            if( model!=NULL )
            {
                ds_property_set_string(d, "storage.model", model);
                ds_property_set_string(d, "info.product", model);
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
                ds_property_set_string(d, "storage.media", media);

                /* Set for removable media */
                if( strcmp(media, "disk")==0 )
                {
                    ds_add_capability(d, "storage");
                    ds_property_set_string(d, "info.category", 
                                           "storage");
                }
                else if( strcmp(media, "cdrom")==0 )
                {
                    ds_add_capability(d, "storage");
                    ds_add_capability(d, "storage.removable");
                    ds_property_set_string(d, "info.category", 
                                           "storage.removable");
                    removable_media = TRUE;
                }
                else if( strcmp(media, "floppy")==0 )
                {
                    ds_add_capability(d, "storage");
                    ds_add_capability(d, "storage.removable");
                    ds_property_set_string(d, "info.category", 
                                           "storage.removable");
                    removable_media = TRUE;
                }
                else if( strcmp(media, "tape")==0 )
                {
                    ds_add_capability(d, "storage");
                    ds_add_capability(d, "storage.removable");
                    ds_property_set_string(d, "info.category", 
                                           "storage.removable");
                    removable_media = TRUE;
                }

            }

            ds_property_set_bool(d, "storage.has_removable_media", 
                                 removable_media);
            
        }
        else
        {
            /** @todo block device on non-IDE device; how to find the
             *         name and the media-type? Right now we just assume
             *         that the disk is fixed and of type flash.. This hack
             *         does 'the right thing' on IDE-systems where the
             *         user attach a USB storage device.
             *
             *         We should special case for at least SCSI once this
             *         information is easily accessible in kernel 2.6.
             *       
             */

            ds_property_set_string(d, "storage.media", "flash");

            ds_add_capability(d, "storage");
            ds_property_set_string(d, "info.category", 
                                   "storage");
            
            /* guestimate product name */
            ds_property_set_string(d, "info.product", "Disk");

            /* omit block.media! */
        }
    }

    /* check /etc/mtab, forces reload of the file */
    etc_mtab_process_all_block_devices(TRUE);

    new_udi = rename_and_merge(d, block_compute_udi, "block");
    if( new_udi!=NULL )
    {
        new_d = ds_device_find(new_udi);
        if( new_d!=NULL )
        {
            linux_class_block_check_if_ready_to_add(new_d);
        }
    }
}

/** Check if all the required properties are in place so we can announce
 *  this device to the world.
 *
 *  @param  d                   Device
 */
void linux_class_block_check_if_ready_to_add(HalDevice* d)
{
    const char* parent;
    const char* device_file;

    /* we know, a'priori, that the only thing we possibly get later
     * is block.device, so just check for the presence of this
     */

    /* but do check that we already got our parent sorted out to avoid
     * a race between udev add and find parent */
    parent = ds_property_get_string(d, "info.parent");
    if( parent==NULL )
        return;

    device_file = ds_property_get_string(d, "block.device");
    HAL_INFO(("Entering, udi=%s, device_file=%s", d->udi, device_file));

    if( device_file!=NULL && strcmp(device_file, "")!=0 )
    {
        ds_gdl_add(d);
    }
}



#define MOUNT_POINT_MAX 256
#define MOUNT_POINT_STRING_SIZE 128

/** Structure for holding mount point information */
struct mount_point_s
{
    int major;                                 /**< Major device number */
    int minor;                                 /**< Minor device number */
    char device[MOUNT_POINT_STRING_SIZE];      /**< Device filename */
    char mount_point[MOUNT_POINT_STRING_SIZE]; /**< Mount point */
    char fs_type[MOUNT_POINT_STRING_SIZE];     /**< Filesystem type */
};

/** Array holding (valid) mount points from /etc/mtab. */
static struct mount_point_s mount_points[MOUNT_POINT_MAX];

/** Number of elements in #mount_points array */
static int num_mount_points;

static int etc_fd = -1;


/** Process a line in /etc/mtab. The given string will be modifed by
 *  this function.
 *
 *  @param  s                   Line of /etc/mtab
 */
static void etc_mtab_process_line(char* s)
{
    int i;
    char* p;
    char* delim = " \t\n";
    char buf[256];
    char* bufp = buf;
    struct stat stat_buf;
    int major = 0;
    int minor = 0;
    char* device = NULL;
    char* mount_point = NULL;
    char* fs_type = NULL;

    i=0;
    p = strtok_r(s, delim, &bufp);
    while( p!=NULL )
    {
        /*printf("token: '%s'\n", p);*/
        switch(i)
        {
        case 0:
            if( strcmp(p, "none")==0 )
                return;
            if( p[0]!='/' )
                return;
            device = p;
            /* Find major/minor for this device */

            if( stat(p, &stat_buf)!=0 )
            {
                return;
            }
            major = MAJOR(stat_buf.st_rdev);
            minor = MINOR(stat_buf.st_rdev);
            break;

        case 1:
            mount_point = p;
            break;

        case 2:
            fs_type = p;
            break;

        case 3:
            break;

        case 4:            
            break;

        case 5:
            break;
        }

        p = strtok_r(NULL, delim, &bufp);
        i++;
    }

    /** @todo  FIXME: Use a linked list or something that doesn't restrict
     *         us like this
     */
    if( num_mount_points==MOUNT_POINT_MAX )
        return;

    mount_points[num_mount_points].major = major;
    mount_points[num_mount_points].minor = minor;
    strncpy(mount_points[num_mount_points].device, device, 
            MOUNT_POINT_STRING_SIZE);
    strncpy(mount_points[num_mount_points].mount_point, mount_point, 
            MOUNT_POINT_STRING_SIZE);
    strncpy(mount_points[num_mount_points].fs_type, fs_type, 
            MOUNT_POINT_STRING_SIZE);

    num_mount_points++;
}

/** Last mtime when /etc/mtab was processed */
static time_t etc_mtab_mtime = 0;


/** Reads /etc/mtab and fill out #mount_points and #num_mount_points 
 *  variables accordingly
 *
 *  This function holds the file open for further access
 *
 *  @param  force               Force reading of mtab
 *  @return                     FALSE if there was no changes to /etc/mtab
 *                              since last invocation or an error occured
 */
static dbus_bool_t read_etc_mtab(dbus_bool_t force)
{
    int fd;
    char buf[256];
    FILE* f;
    struct stat stat_buf;

    num_mount_points=0;

    fd = open("/etc/mtab", O_RDONLY);

    if( fd==-1 )
    {
        HAL_ERROR(("Cannot open /etc/mtab"));
        return FALSE;
    }
    
    if( fstat(fd, &stat_buf)!=0 )
    {
        HAL_ERROR(("Cannot fstat /etc/mtab fd, errno=%d", errno));
        return FALSE;
    }

    if( !force && etc_mtab_mtime==stat_buf.st_mtime )
    {
        /*printf("No modification, etc_mtab_mtime=%d\n", etc_mtab_mtime);*/
        return FALSE;
    }

    etc_mtab_mtime = stat_buf.st_mtime;

    /*printf("Modification, etc_mtab_mtime=%d\n", etc_mtab_mtime);*/

    f = fdopen(fd, "r");

    if( f==NULL )
    {
        HAL_ERROR(("Cannot fdopen /etc/mtab fd"));
        return FALSE;
    }

    while( !feof(f) )
    {
        if( fgets(buf, 256, f)==NULL )
            break;
        /*printf("got line: '%s'\n", buf);*/
        etc_mtab_process_line(buf);
    }
    
    fclose(f);

    close(fd);

    return TRUE;
}

static void sigio_handler(int sig);

/** Global to see if we have setup the watcher on /etc */
static dbus_bool_t have_setup_watcher = FALSE;

/** Load /etc/mtab and process all HAL block devices and set properties
 *  according to mount status. Also, optionally, sets up a watcher to do
 *  this whenever /etc/mtab changes
 *
 *  @param  force               Force reading of mtab
 */
static void etc_mtab_process_all_block_devices(dbus_bool_t force)
{
    int i;
    const char* bus;
    HalDevice* d;
    int major, minor;
    dbus_bool_t found_mount_point;
    struct mount_point_s* mp;
    HalDeviceIterator diter;

    /* Start or continue watching /etc */
    if( !have_setup_watcher )
    {
        have_setup_watcher = TRUE;

        signal(SIGIO, sigio_handler);
        etc_fd = open("/etc", O_RDONLY);
        fcntl(etc_fd, F_NOTIFY, DN_MODIFY|DN_MULTISHOT);
    }

    /* Just return if /etc/mtab wasn't modified */
    if( !read_etc_mtab(force) )
        return;

    HAL_INFO(("/etc/mtab changed, processing all block devices"));

    /* Iterate over all HAL devices */
    for(ds_device_iter_begin(&diter);
        ds_device_iter_has_more(&diter);
        ds_device_iter_next(&diter))
    {

        d = ds_device_iter_get(&diter);

        bus = ds_property_get_string(d, "info.bus");
        if( bus==NULL || strcmp(bus, "block")!=0 )
            continue;

        major = ds_property_get_int(d, "block.major");
        minor = ds_property_get_int(d, "block.minor");

        /* Search all mount points */
        found_mount_point = FALSE;
        for(i=0; i<num_mount_points; i++)
        {
            mp = &mount_points[i];

            if( mp->major==major && mp->minor==minor )
            {
                HAL_INFO((
                    "%s mounted at %s, major:minor=%d:%d, fstype=%s, udi=%s",
                    mp->device, mp->mount_point, mp->major, mp->minor, 
                    mp->fs_type, d->udi));
                
                property_atomic_update_begin();

                /* Yay! Found a mount point; set properties accordingly */
                ds_property_set_string(d, "block.device", mp->device);
                ds_property_set_string(d, "block.mount_point",mp->mount_point);
                ds_property_set_string(d, "block.fstype", mp->fs_type);
                ds_property_set_bool(d, "block.is_mounted", TRUE);

                property_atomic_update_end();

                found_mount_point = TRUE;
                break;
            }
        }

        /* No mount point found; (possibly) remove all information */
        if( !found_mount_point )
        {
            property_atomic_update_begin();

            ds_property_set_bool(d, "block.is_mounted", FALSE);
            ds_property_set_string(d, "block.mount_point", "");
            ds_property_set_string(d, "block.fstype", "");
            if( !ds_property_exists(d, "block.device") )
            {
                /** @todo Invoke udev! */

                ds_property_set_string(d, "block.device", "fixme-invoke-udev");
            }

            property_atomic_update_end();
        }        
    }
}


/** Signal handler for watching /etc
 *
 *  @param  sig                 Signal number
 */
static void sigio_handler(int sig)
{
    HAL_INFO(("Directory /etc changed"));

    /** @todo FIXME: It's evil to sleep in a signal handler, yes? */
    usleep(250*1000);

    /* don't force reloading of /etc/mtab */
    etc_mtab_process_all_block_devices(FALSE);
}



/** Init function for block device handling
 *
 */
void linux_class_block_init()
{
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void linux_class_block_detection_done()
{
    etc_mtab_process_all_block_devices(TRUE);
}

/** Shutdown function for block device handling
 *
 */
void linux_class_block_shutdown()
{

}

