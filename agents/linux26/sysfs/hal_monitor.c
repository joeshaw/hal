/***************************************************************************
 * CVSID: $Id$
 *
 * hal_monitor.c : monitor mode for watching stuff about devices
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 *
 * Some parts of this file is based on mii-diag.c, Copyright 1997-2003 by
 * Donald Becker <becker@scyld.com>
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

typedef unsigned short u16;
typedef unsigned int u32;


#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

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

        /*printf("udi %s: major,minor=%d,%d\n", udi, major, minor);*/

        /* Search all mount points */
        found_mount_point = FALSE;
        for(j=0; j<num_mount_points; j++)
        {
            mp = &mount_points[j];

            if( mp->major==major && mp->minor==minor )
            {
                /*
                printf("%s mounted at %s, major=%d, minor=%d, fstype=%s\n",
                       mp->device, mp->mount_point, mp->major, mp->minor, 
                       mp->fs_type);
                */

                /* Yay! Found a mount point; set properties accordingly */
                hal_device_set_property_string(udi, "block.device", 
                                               mp->device);
                hal_device_set_property_string(udi, "block.mountPoint", 
                                               mp->mount_point);
                hal_device_set_property_string(udi, "block.fileSystem", 
                                               mp->fs_type);
                hal_device_set_property_bool(udi, "block.isMounted", TRUE);

                found_mount_point = TRUE;
            }
        }

        /* No mount point found; (possibly) remove all information */
        if( !found_mount_point )
        {
            hal_device_set_property_bool(udi, "block.isMounted", FALSE);
            hal_device_remove_property(udi, "block.mountPoint");
            hal_device_remove_property(udi, "block.fileSystem");
        }
    }

}

/** Signal handler for watching /etc
 *
 *  @param  sig                 Signal number
 */
static void sigio_handler(int sig)
{
    /*printf("file changed, sig=%d, SIGIO=%d\n", sig, SIGIO);*/

    /** @todo FIXME: Isn't it evil to sleep in a signal handler? */
    sleep(1);

    etc_mtab_process_all_block_devices(TRUE);
}


/** Maximum length of Unique Device Identifer */
#define HAL_UDI_MAXSIZE 256

/** Structure for holding watching information for an ethernet interface */
typedef struct ethmon_interface_s
{
    char udi[HAL_UDI_MAXSIZE];          /**< UDI of device */
    int skfd;                           /**< File descriptor for socket */
    struct ifreq ifr;                   /**< Structure used in ioctl() */
    int new_ioctl_nums;                 /**< Is the new ioctl being used? */
    dbus_uint16_t status_word_baseline; /**< Last status word read */
    struct ethmon_interface_s* next;    /**< Pointer to next element */
} ethmon_interface;

/** Head of linked list of ethernet interfaces to watch */
static ethmon_interface* ethmon_list_head = NULL;

/** Read a word from the MII transceiver management registers 
 *
 *  @param  iface               Which interface
 *  @param  location            Which register
 *  @return                     Word that is read
 */
static dbus_uint16_t ethmon_mdio_read(ethmon_interface* iface, int location)
{
    dbus_uint16_t *data = (dbus_uint16_t *)(&(iface->ifr.ifr_data));

    data[1] = location;

    if( ioctl(iface->skfd, 
              iface->new_ioctl_nums ? 0x8948 : SIOCDEVPRIVATE+1, 
              &(iface->ifr)) < 0)
    {
        fprintf(stderr, "SIOCGMIIREG on %s failed: %s\n", iface->ifr.ifr_name,
                strerror(errno));
        return -1;
    }
    return data[3];
}

/** Check whether status has changed.
 *
 *  @param  iface               Which interface
 */
static void ethmon_process(ethmon_interface* iface)
{
    dbus_uint16_t status_word;
    dbus_uint16_t link_word;
    dbus_uint16_t status_word_new;

    /*printf("iface = 0x%0x\n", iface);*/

    status_word_new = ethmon_mdio_read(iface, 1);
    if( status_word_new!=iface->status_word_baseline )
    {
        iface->status_word_baseline = status_word_new;

        syslog(LOG_INFO, "Ethernet link status change on hal udi %s)",
               iface->udi);

        /* Read status_word again since some bits may be sticky */
        status_word = ethmon_mdio_read(iface, 1);

        /* Refer to http://www.scyld.com/diag/mii-status.html for
         * the full explanation of the numbers
         *
         * 0x8000  Capable of 100baseT4.
         * 0x7800  Capable of 10/100 HD/FD (most common).
         * 0x0040  Preamble suppression permitted.
         * 0x0020  Autonegotiation complete.
         * 0x0010  Remote fault.
         * 0x0008  Capable of Autonegotiation.
         * 0x0004  Link established ("sticky"* on link failure)
         * 0x0002  Jabber detected ("sticky"* on transmit jabber)
         * 0x0001  Extended MII register exist.
         *
         */

        if( (status_word&0x0016)==0x0004 )
        {
            hal_device_set_property_int(iface->udi, 
                                         "net.ethernet.link", TRUE);
        }
        else
        {
            hal_device_set_property_int(iface->udi, 
                                         "net.ethernet.link", FALSE);
        }

        /* Read link word
         *
         * 0x8000  Link partner can send more info.
         * 0x4000  Link partner got our advertised abilities.
         * 0x2000  Fault detected by link partner (uncommon).
         * 0x0400  Flow control supported (currently uncommon)
         * 0x0200  100baseT4 supported (uncommon)
         * 0x0100  100baseTx-FD (full duplex) supported
         * 0x0080  100baseTx supported
         * 0x0040  10baseT-FD supported
         * 0x0020  10baseT supported
         * 0x001F  Protocol selection bits, always 0x0001 for Ethernet.
         */
        link_word = ethmon_mdio_read(iface, 1);

        if( link_word&0x0300 )
        {
            hal_device_set_property_int(iface->udi, "net.ethernet.rate", 
                                        100*1000*1000);
        }
        if( link_word&0x60 )
        {
            hal_device_set_property_int(iface->udi, "net.ethernet.rate", 
                                        10*1000*1000);
        }

    }
}

/** Timeout handler for processing status on all watched interfaces
 *
 *  @param  data                User data when setting up timer
 */
static gboolean ethmon_timeout(gpointer data)
{
    ethmon_interface* iface;

    for(iface=ethmon_list_head; iface!=NULL; iface=iface->next)
        ethmon_process(iface);

    return TRUE;
}

/** Add a watch for a HAL device; it must be a net.ethernet capable.
 *
 *  @param  udi                 UDI of device
 */
static void ethmon_add(const char* udi)
{
    char* interface_name;
    ethmon_interface* iface;

    iface = malloc(sizeof(ethmon_interface));
    if( iface==NULL )
        DIE(("No memory"));

    interface_name = hal_device_get_property_string(udi, "net.interface");
    if( interface_name==NULL )
    {
        fprintf(stderr, "device '%s' does not have net.interface\n", udi);
        free(iface);
        return;
    }

    strncpy(iface->udi, udi, HAL_UDI_MAXSIZE);

    snprintf(iface->ifr.ifr_name, IFNAMSIZ, interface_name);

    /* Open a basic socket. */
    if( (iface->skfd = socket(AF_INET, SOCK_DGRAM,0))<0 )
    {
        fprintf(stderr, "cannot open socket on interface %s; errno=%d\n", 
                interface_name, errno);
        free(iface);
        return;
    }

    if( ioctl(iface->skfd, 0x8947, &(iface->ifr))>=0 )
    {
        iface->new_ioctl_nums = 1;
    } 
    else if( ioctl(iface->skfd, SIOCDEVPRIVATE, &(iface->ifr))>=0 )
    {
        iface->new_ioctl_nums = 0;
    } 
    else
    {
        fprintf(stderr, "SIOCGMIIPHY on %s failed: %s\n", iface->ifr.ifr_name,
                strerror(errno));
        (void) close(iface->skfd);
        free(iface);
        return;
    }

    iface->status_word_baseline = 0x5555;


    ethmon_process(iface);

    iface->next = ethmon_list_head;
    ethmon_list_head = iface;
}

/** Remove watch for a HAL device
 *
 */
static void ethmon_remove(const char* udi)
{
    ethmon_interface* iface;
    ethmon_interface* iface_prev = NULL;

    for(iface=ethmon_list_head; iface!=NULL; iface=iface->next)
    {
        if( strcmp(udi, iface->udi)==0 )
        {
            if( iface_prev!=NULL )
            {
                iface_prev->next = iface->next;
            }
            else
            {
                ethmon_list_head = iface->next;
            }

            close(iface->skfd);
            free(iface);
        }

        iface_prev = iface;
    }
}

/** Scan all HAL devices of with capability net.ethernet and set link
 *  properties. Also, optionally, this function sets up a watcher to
 *  act on link changes.
 *
 *  @param  setup_watcher       Monitor link status
 */
static void ethernet_process_all_devices(dbus_bool_t setup_watcher)
{
    int i;
    int num_ethernet_devices;
    char** ethernet_devices;

    ethernet_devices = hal_find_device_by_capability("net.ethernet", 
                                                     &num_ethernet_devices);
    /*printf("num: %d\n", num_ethernet_devices);*/
    for(i=0; i<num_ethernet_devices; i++)
    {
        /*printf("device %d/%d : %s\n", 
          i, num_ethernet_devices, ethernet_devices[i]);*/
        ethmon_add(ethernet_devices[i]);
    }

    hal_free_string_array(ethernet_devices);

    /** @todo FIXME Get rid of glib dependency */
    g_timeout_add(1000, ethmon_timeout, NULL);
}

/** Callback when a HAL device is added
 *
 *  @param  udi                 UDI of device
 */
static void device_added(const char* udi)
{
    /*printf("in device_added, udi=%s\n", udi);*/
    if( hal_device_query_capability(udi, "net.ethernet") )
        ethmon_add(udi);
}

/** Callback when a HAL device got a new capability
 *
 *  @param  udi                 UDI of device
 *  @param  cap                 Capability, e.g. net.ethernet, input.mouse etc.
 */
static void device_new_capability(const char* udi, const char* cap)
{
    /*printf("*** in device_new_capability, udi=%s, cap=%s\n", udi, cap);*/
    if( hal_device_query_capability(udi, "net.ethernet") )
        ethmon_add(udi);
}

/** Callback when a HAL device have been removed
 *
 *  @param  udi                 UDI of device
 */
static void device_removed(const char* udi)
{
    /*printf("in device_removed, udi=%s\n", udi);*/
    ethmon_remove(udi);
}


static DBusHandlerResult udev_filter_func(DBusConnection* connection,
                                          DBusMessage*    message,
                                          void*           user_data)
{  
    char* filename;
    char* sysfs_path;
    char sysfs_dev_path[SYSFS_PATH_MAX];
    char* udi;
    const char* object_path;
    DBusError error;

    dbus_error_init(&error);

    object_path = dbus_message_get_path(message);

    /*printf("*** in udev_filter_func, object_path=%s\n", object_path);*/

    if( dbus_message_is_signal(message, "org.kernel.udev.NodeMonitor",
                               "NodeCreated") )
    {
        if( dbus_message_get_args(message, &error, 
                                  DBUS_TYPE_STRING, &filename,
                                  DBUS_TYPE_STRING, &sysfs_path,
                                  DBUS_TYPE_INVALID) )
        {
            strncpy(sysfs_dev_path, sysfs_mount_path, SYSFS_PATH_MAX);
            strncat(sysfs_dev_path, sysfs_path, SYSFS_PATH_MAX);
            printf("NodeCreated: %s %s\n", filename, sysfs_dev_path);

            udi = find_udi_from_sysfs_path(sysfs_dev_path, 
                                     HAL_LINUX_HOTPLUG_TIMEOUT);
            if( udi!=NULL )
            {
                hal_device_set_property_string(udi, "block.device", filename);
            }
        }
    }
    else if( dbus_message_is_signal(message, "org.kernel.udev.NodeMonitor",
                                    "NodeDeleted") )
    {
        if( dbus_message_get_args(message, &error, 
                                  DBUS_TYPE_STRING, &filename,
                                  DBUS_TYPE_STRING, &sysfs_path,
                                  DBUS_TYPE_INVALID) )
        {
            /* This is left intentionally blank since this means that a
             * block device is removed and we'll catch that other places..

            strncpy(sysfs_dev_path, sysfs_mount_path, SYSFS_PATH_MAX);
            strncat(sysfs_dev_path, sysfs_path, SYSFS_PATH_MAX);
            printf("NodeDeleted: %s %s\n", filename, sysfs_dev_path);

            udi = find_udi_from_sysfs_path(sysfs_dev_path, 
                                     HAL_LINUX_HOTPLUG_TIMEOUT);
            if( udi!=NULL )
            {
                hal_device_remove_property(udi, "block.device");
            }
            */
        }
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}


static void setup_udev_listener()
{
    DBusError error;
    DBusConnection* connection;

    /* Add filter for listening to udev */
    if( !dbus_connection_add_filter(dbus_connection, 
                                    udev_filter_func, NULL, NULL) )
    {
        fprintf(stderr, "%s %d : Error creating connection handler\r\n",
                __FILE__, __LINE__);
        // TODO: clean up
        return 1;
    }
    
    dbus_error_init(&error);
    dbus_bus_add_match(dbus_connection, 
                       "type='signal',"
                       "interface='org.kernel.udev.NodeMonitor',"
                       /*"sender='org.kernel.udev',"*/
                       "path='/org/kernel/udev/NodeMonitor'", &error);

    if( dbus_error_is_set(&error) )
    {
        fprintf(stderr, "%s %d : Error subscribing to signals, "
                "error=%s\r\n",
                __FILE__, __LINE__, error.message);
        // TODO: clean up
        return 1;
    }
}

/** Enter monitor mode
 *
 *  @param  loop                G-Lib mainloop
 */
void hal_monitor_enter(GMainLoop* loop)
{
    DBusError error;

    syslog(LOG_INFO, "Entering monitor mode..");    

    /* Find possible mount point for block devices and setup 
     * a watcher */
    etc_mtab_process_all_block_devices(TRUE);

    /* Process all ethernet devices for link and media detection
     * and setup a watcher
     */
    ethernet_process_all_devices(TRUE);

    hal_functions.device_added   = device_added;
    hal_functions.device_removed = device_removed;
    hal_functions.device_new_capability = device_new_capability;

    /* Setup listener for udev signals */
    setup_udev_listener();



    g_main_loop_run(loop);
}
