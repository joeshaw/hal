/***************************************************************************
 * CVSID: $Id$
 *
 * hal_monitor.c : monitor mode for watching stuff about devices
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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define _GNU_SOURCE 1
#include <linux/fcntl.h>
#include <linux/kdev_t.h>

#include <sys/signal.h>

#include <errno.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <libsysfs.h>

#include <libhal/libhal.h>

#include "main.h"
#include "hal_monitor.h"

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
 *  @return                     FALSE if there was no changes to /etc/mtab
 *                              since last invocation or an error occured
 */
static dbus_bool_t read_etc_mtab()
{
    int fd;
    char buf[256];
    FILE* f;
    struct stat stat_buf;

    num_mount_points=0;

    fd = open("/etc/mtab", O_RDONLY);

    if( fd==-1 )
    {
        printf("Cannot open /etc/mtab\n");
        return FALSE;
    }
    
    if( fstat(fd, &stat_buf)!=0 )
    {
        printf("Cannot fstat /etc/mtab fd, errno=%d\n", errno);
        return FALSE;
    }

    if( etc_mtab_mtime==stat_buf.st_mtime )
    {
        /*printf("No modification, etc_mtab_mtime=%d\n", etc_mtab_mtime);*/
        return FALSE;
    }

    etc_mtab_mtime = stat_buf.st_mtime;

    /*printf("Modification, etc_mtab_mtime=%d\n", etc_mtab_mtime);*/

    f = fdopen(fd, "r");

    if( f==NULL )
    {
        printf("Cannot fdopen /etc/mtab fd\n");
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
 *  @param  setup_watcher       Monitor /etc/mtab
 */
void etc_mtab_process_all_block_devices(dbus_bool_t setup_watcher)
{
    int i, j;
    struct mount_point_s* mp;
    int etc_mtab_num_block_devices;
    char** etc_mtab_block_devices;

    /* Start or continue watching /etc */
    if( setup_watcher && !have_setup_watcher )
    {
        have_setup_watcher = TRUE;

        signal(SIGIO, sigio_handler);
        etc_fd = open("/etc", O_RDONLY);
        fcntl(etc_fd, F_NOTIFY, DN_MODIFY|DN_MULTISHOT);
    }

    /* Just return if /etc/mtab wasn't modified */
    if( !read_etc_mtab() )
        return;

    /* Find all HAL block devices */
    etc_mtab_block_devices = hal_manager_find_device_string_match(
        "Bus", "block", 
        &etc_mtab_num_block_devices);

    /* Process all HAL block devices */
    for(i=0; i<etc_mtab_num_block_devices; i++)
    {
        char* udi;
        int major, minor;
        dbus_bool_t found_mount_point;

        udi = etc_mtab_block_devices[i];
        major = hal_device_get_property_int(udi, "block.major");
        minor = hal_device_get_property_int(udi, "block.minor");

        printf("udi %s: major,minor=%d,%d\n", udi, major, minor);

        /* Search all mount points */
        found_mount_point = FALSE;
        for(j=0; j<num_mount_points; j++)
        {
            mp = &mount_points[j];

            if( mp->major==major && mp->minor==minor )
            {
                printf("%s mounted at %s, major=%d, minor=%d, fstype=%s\n",
                       mp->device, mp->mount_point, mp->major, mp->minor, 
                       mp->fs_type);

                /* Yay! Found a mount point; set properties accordingly */
                hal_device_set_property_string(udi, "volume.device", 
                                               mp->device);
                hal_device_set_property_string(udi, "volume.mountPoint", 
                                               mp->mount_point);
                hal_device_set_property_string(udi, "volume.fileSystem", 
                                               mp->fs_type);
                hal_device_set_property_bool(udi, "volume.isMounted", TRUE);

                found_mount_point = TRUE;
            }
        }

        /* No mount point found; (possibly) remove all information */
        if( !found_mount_point )
        {
            hal_device_set_property_bool(udi, "volume.isMounted", FALSE);
            hal_device_remove_property(udi, "volume.mountPoint");
            hal_device_remove_property(udi, "volume.fileSystem");
            hal_device_remove_property(udi, "volume.device");
        }
    }

}

/** Signal handler for watching /etc
 *
 *  @param  sig                 Signal number
 */
static void sigio_handler(int sig)
{
    printf("file changed, sig=%d, SIGIO=%d\n", sig, SIGIO);

    /** @todo FIXME: Isn't it evil to sleep in a signal handler */
    sleep(1);

    etc_mtab_process_all_block_devices(TRUE);
}


/** Enter monitor mode
 *
 *  @param  loop                G-Lib mainloop
 */
void hal_monitor_enter(GMainLoop* loop)
{
    syslog(LOG_INFO, "Entering monitor mode..");    

    etc_mtab_process_all_block_devices(TRUE);

    g_main_loop_run(loop);
}
