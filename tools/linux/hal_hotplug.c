/***************************************************************************
 * CVSID: $Id$
 *
 * hal_hotplug.c : Tiny program to transform a linux-hotplug event into
 *                 a D-BUS message
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mntent.h>
#include <syslog.h>

#include <dbus/dbus.h>

/**
 * @defgroup HalMisc  Misc tools for HAL
 * @brief  Misc. tools for HAL
 */


/**
 * @defgroup HalLinuxHotplug  HAL hotplug helper for Linux
 * @ingroup HalMisc
 * @brief A short program for translating linux-hotplug events into
 *        D-BUS messages. The messages are sent to the HAL daemon.
 * @{
 */


static char* sysfs_mnt_path[255];

/** Get the mount path for sysfs. A side-effect is that sysfs_mnt_path
 *  is set on success.
 *
 *  @return                     0 on success, negative on error
 */
static int get_sysfs_mnt_path()
{

    FILE *mnt;
    struct mntent *mntent;
    int ret = 0;
    size_t dirlen = 0;
    
    if ((mnt = setmntent("/proc/mounts", "r")) == NULL) {
	return -1;
    }

    while (ret == 0 && dirlen == 0 && (mntent = getmntent(mnt)) != NULL) {
	if (strcmp(mntent->mnt_type, "sysfs") == 0) {
	    dirlen = strlen(mntent->mnt_dir);
	    if (dirlen <= (255 - 1)) {
		strcpy(sysfs_mnt_path, mntent->mnt_dir);
	    } else {
		ret = -1;
	    }
	}
    }
    endmntent(mnt);

    if (dirlen == 0 && ret == 0) {
	ret = -1;
    }
    return ret;
}

static char* file_list_usb[] = {"idProduct",
				"idVendor",
				"bcdDevice",
				"bMaxPower",
				/*"serial",*/
				"bmAttributes",
				"manufacturer",
				"product",
				"bDeviceClass",
				"bDeviceSubClass",
				"bDeviceProtocol",
				"bNumConfigurations",
				"bConfigurationValue",
				"bNumInterfaces",
				NULL};

static char* file_list_usbif[] = {"bInterfaceClass",
				  "bInterfaceSubClass",
				  "bInterfaceProtocol",
				  "bInterfaceNumber",
				  NULL};

static char* file_list_scsi_device[] = {NULL};

static char* file_list_scsi_host[] = {NULL};

static char* file_list_block[] = {"dev",
				  "size",
				  NULL};

static char* file_list_pci[] = {"device", 
				"vendor", 
				"subsystem_device",
				"subsystem_vendor",
				"class",
				NULL};

static int wait_for_sysfs_info(char* devpath, char* hotplug_type)
{
    size_t devpath_len;
    char** file_list;
    int num_tries;
    int rc;
    struct stat stat_buf;
    char* file;
    int i;
    char path[255];

    syslog(LOG_NOTICE, "waiting for %s info at %s", 
	   hotplug_type, devpath);

    devpath_len = strlen(devpath);

    file_list = NULL;

    if( strcmp(hotplug_type, "pci")==0 )
    {
	file_list = file_list_pci;
    }
    else if( strcmp(hotplug_type, "usb")==0 )
    {
	int is_interface = 0;

	for(i=devpath_len-1; devpath[i]!='/' && i>0; --i)
	{
	    if( devpath[i]==':' )
	    {
		is_interface = TRUE;
		break;
	    }
	}

	if( is_interface )
	{
	    syslog(LOG_NOTICE, "%s is an USB interface", devpath);
	    file_list = file_list_usbif;
	}
	else
	    file_list = file_list_usb;
    }
    else if( strcmp(hotplug_type, "scsi_device")==0 )
    {
	file_list = file_list_scsi_device;
    }
    else if( strcmp(hotplug_type, "scsi_host")==0 )
    {
	file_list = file_list_scsi_host;
    }
    else if( strcmp(hotplug_type, "block")==0 )
    {
	file_list = file_list_block;
    }

    if( file_list==NULL )
    {
	syslog(LOG_WARNING, "Dont know how to wait for %s at %s; "
	       "sleeping 1000 ms", hotplug_type, devpath);
	usleep(1000*1000);
	return -1;
    }

    num_tries=0;

try_again:
    if( num_tries>0 )
    {
	usleep(100*1000);
    }

    if( num_tries==20*10 )
    {
	syslog(LOG_NOTICE, "timed out for %s (waited %d ms)", 
	       devpath, num_tries*100);
	return -1;
    }
    
    num_tries++;
    
    /* first, check directory */
    strncpy(path, sysfs_mnt_path, 255);
    strncat(path, devpath, 255);

    /*printf("path0 = %s\n", path);*/

    rc = stat(path, &stat_buf);
    /*printf("rc0 = %d\n", rc);*/
    if( rc!=0 )
	goto try_again;
    
    /* second, check each requested file */
    for(i=0; file_list[i]!=NULL; i++)
    {
	file = file_list[i];
	
	strncpy(path, sysfs_mnt_path, 255);
	strncat(path, devpath, 255);
	strncat(path, "/", 255);
	strncat(path, file, 255);

	/*printf("path1 = %s\n", path);*/

	rc = stat(path, &stat_buf);

	/*printf("rc1 = %d\n", rc);*/

	if( rc!=0 )
	    goto try_again;
    }

    syslog(LOG_NOTICE, "got info for %s (waited %d ms)", 
	   devpath, (num_tries-1)*100);

    return 0;
}


/** Entry point
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @param  envp                Environment
 *  @return                     Exit code
 */
int main(int argc, char* argv[], char* envp[])
{
    int i, j, len;
    char* str;
    char* hotplug_type;
    char* devpath;
    int is_add;
    DBusError error;
    DBusConnection* sysbus_connection;
    DBusMessage* message;
    DBusMessageIter iter;
    DBusMessageIter iter_dict;

    if( argc!=2 )
        return 1;

    if( get_sysfs_mnt_path()!=0 )
	return 1;

    openlog("hal.hotplug", LOG_PID, LOG_USER);

    /* Connect to a well-known bus instance, the system bus */
    dbus_error_init(&error);
    sysbus_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if( sysbus_connection==NULL ) 
        return 1;

    /* service, object, interface, member */
    message = dbus_message_new_method_call(
        "org.freedesktop.Hal",
        "/org/freedesktop/Hal/Linux/Hotplug", 
        "org.freedesktop.Hal.Linux.Hotplug",
        "HotplugEvent");

    /* not interested in a reply */
    dbus_message_set_no_reply(message, TRUE);

    hotplug_type = argv[1];
    devpath = NULL;

    is_add = FALSE;
    
    dbus_message_iter_init(message, &iter);
    dbus_message_iter_append_string(&iter, hotplug_type);
    dbus_message_iter_append_dict(&iter, &iter_dict);
    for(i=0; envp[i]!=NULL; i++)
    {
        str = envp[i];
        len = strlen(str);
        for(j=0; j<len && str[j]!='='; j++)
            ;
        str[j]='\0';

        dbus_message_iter_append_dict_key(&iter_dict, str);
        dbus_message_iter_append_string(&iter_dict, str+j+1);

	if( strcmp(str, "DEVPATH")==0 )
	{
	    devpath = str+j+1;
	}
	else if( strcmp(str, "ACTION")==0 )
	{
	    if( strcmp(str+j+1, "add")==0 )
	    {
		is_add = TRUE;
	    }
	}
    }

    if( devpath!=NULL && is_add )
    {
	int rc;

	/* wait for information to be published in sysfs */
	rc = wait_for_sysfs_info(devpath, hotplug_type);
	if( rc!=0 )
	{
	    /** @todo handle error */
	}
    }
    else
    {
	/* Do some sleep here so the kernel have time to publish it's
	 * stuff in sysfs
	 */
	/*usleep(1000*1000);*/
    }

    usleep(1000*1000);

    if ( !dbus_connection_send(sysbus_connection, message, NULL) )
        return 1;

    dbus_message_unref(message);        
    dbus_connection_flush(sysbus_connection);

    /* Do some sleep here so messages are not lost.. */
    usleep(500*1000);

    dbus_connection_disconnect(sysbus_connection);

    return 0;
}

/** @} */
