/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_net.c : Network device functions on Linux 2.6
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

#include <net/if_arp.h> /* for ARPHRD_ETHER etc. */

#include "../logger.h"
#include "../device_store.h"
#include "linux_class_net.h"


/* fwd decl */
static void visit_class_device_net_got_sysdevice(HalDevice* parent, 
                                                 void* data1, void* data2);

/** Visitor function for net device.
 *
 *  This function parses the attributes present and merges more information
 *  into the HAL device this class device points to
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_class_device_net(const char* path, 
                            struct sysfs_class_device* class_device)
{
    int i;
    int len;
    HalDevice* d;
    struct sysfs_attribute* cur;
    char attr_name[SYSFS_NAME_LEN];
    char* addr_store = NULL;
    int media_type = 0;
    char* media;

    if( class_device->sysdevice==NULL )
    {
        LOG_WARNING(("Net class device at sysfs path %s doesn't have "
                     "sysdevice", path));
        return;
    }

    d = ds_device_new();
    ds_property_set_string(d, "net.interface", class_device->name);
    ds_property_set_string(d, "net.linux.sysfs_path", path);

    dlist_for_each_data(sysfs_get_classdev_attributes(class_device), cur,
                        struct sysfs_attribute)
    {
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        /* strip whitespace */
        len = strlen(cur->value);
        for(i=len-1; i>=0 && isspace(cur->value[i]); --i)
            cur->value[i] = '\0';

        if( strcmp(attr_name, "address")==0 )
        {
            addr_store = cur->value;
        }
        else if( strcmp(attr_name, "type")==0 )
        {
            media_type = parse_dec(cur->value);
        }
    }

    if( addr_store!=NULL && media_type==ARPHRD_ETHER )
    {
        unsigned int a5, a4, a3 ,a2, a1, a0;

        ds_property_set_string(d, "net.ethernet.macAddr", addr_store);

        if( sscanf(addr_store, "%x:%x:%x:%x:%x:%x",
                   &a5, &a4, &a3, &a2, &a1, &a0)==6 )
        {
            dbus_uint32_t mac_upper, mac_lower;

            mac_upper = (a5<<16)|(a4<<8)|a3;
            mac_lower = (a2<<16)|(a1<<8)|a0;

            ds_property_set_int(d, "net.ethernet.macAddrUpper24",
                                        (dbus_int32_t) mac_upper);
            ds_property_set_int(d, "net.ethernet.macAddrLower24",
                                        (dbus_int32_t) mac_lower);
        }
    }


    /* check for driver */
    if( class_device->driver!=NULL )
    {
        ds_property_set_string(d, "linux.driver", 
                                       class_device->driver->name);        
    }


    ds_property_set_int(d, "net.arpProtoHwId", 
                                media_type);

    /* Always set capabilities as the last thing the addition of a 
     * capability triggers a signal to other apps using HAL, monitoring
     * daemons for instance
     */

    ds_property_set_string(d, "Category", "net");
    ds_add_capability(d, "net");

    /* type is decimal according to net/core/net-sysfs.c and it
     * assumes values from /usr/include/net/if_arp.h. Either
     * way we store both the 
     */
    switch(media_type)
    {
    case ARPHRD_NETROM: 
        media="NET/ROM pseudo"; 
        break;
    case ARPHRD_ETHER: 
        media="Ethernet"; 
        break;
    case ARPHRD_EETHER: 
        media="Experimenal Ethernet"; 
        ds_add_capability(d, "net.ethernet");
        break;
    case ARPHRD_AX25: 
        media="AX.25 Level 2"; 
        break;
    case ARPHRD_PRONET: 
        media="PROnet tokenring"; 
        ds_add_capability(d, "net.tokenring");
        break;
    case ARPHRD_CHAOS: 
        media="Chaosnet"; 
        break;
    case ARPHRD_IEEE802: 
        media="IEEE802"; 
        break;
    case ARPHRD_ARCNET: 
        media="ARCnet"; 
        break;
    case ARPHRD_APPLETLK: 
        media="APPLEtalk"; 
        break;
    case ARPHRD_DLCI: 
        media="Frame Relay DLCI"; 
        break;
    case ARPHRD_ATM: 
        media="ATM"; 
        ds_add_capability(d, "net.atm");
        break;
    case ARPHRD_METRICOM: 
        media="Metricom STRIP (new IANA id)"; 
        break;
    case ARPHRD_IEEE1394: 
        media="IEEE1394 IPv4 - RFC 2734"; 
        break;
    default: 
        media="Unknown"; 
        break;
    }
    ds_property_set_string(d, "net.media", media);

    /* Find the physical; this happens asynchronously as it might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string(
        "Linux.sysfs_path_device",
        class_device->sysdevice->path, 
        visit_class_device_net_got_sysdevice,
        (void*) d, NULL, 
        is_probing ? 0 :
        HAL_LINUX_HOTPLUG_TIMEOUT);
}

/** Callback when the sysdevice is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void visit_class_device_net_got_sysdevice(HalDevice* sysdevice, 
                                                 void* data1, void* data2)
{
    HalDevice* d = (HalDevice*) data1;

    if( sysdevice==NULL )
    {
        LOG_WARNING(("Sysdevice for a class net device never appeared!"));
    }
    else
    {
        /* merge information from temporary device into the physical
         * device 
         */
        ds_device_merge(sysdevice, d);
    }

    /* get rid of tempoary device; it was only a placeholder after all */
    ds_device_destroy(d);
}


/** Init function for block device handling
 *
 */
void linux_class_net_init()
{
}

/** Shutdown function for block device handling
 *
 */
void linux_class_net_shutdown()
{
}

