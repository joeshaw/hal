/***************************************************************************
 * CVSID: $Id$
 *
 * hal_net.c : Network device functions for sysfs-agent on Linux 2.6
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

#include <net/if_arp.h> /* for ARPHRD_ETHER etc. */

#include "main.h"
#include "hal_net.h"

/** Visitor function for net device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_class_device_net(const char* path, 
                            struct sysfs_class_device* class_device)
{
    char* d;
    struct sysfs_attribute* cur;
    char attr_name[SYSFS_NAME_LEN];

    if( class_device->sysdevice==NULL )
    {
        return;
    }

    /* Find the physical device */
    d = find_udi_from_sysfs_path(class_device->sysdevice->path, 
                                 HAL_LINUX_HOTPLUG_TIMEOUT);
    if( d==NULL )
    {
        printf("Couldn't find physical device at sysfs path %s\n", 
               class_device->sysdevice->path);
        return;
    }

    hal_device_set_property_string(d, "net.interface", class_device->name);
    hal_device_set_property_string(d, "net.linux.sysfs_path", path);

    dlist_for_each_data(sysfs_get_classdev_attributes(class_device), cur,
                        struct sysfs_attribute)
    {
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        if( strcmp(attr_name, "type")==0 )
        {
            int type = parse_dec(cur->value);
            char* media;

            /* type is decimal according to net/core/net-sysfs.c and it
             * assumes values from /usr/include/net/if_arp.h. Either
             * way we store both the 
             */
            switch(type)
            {
            case ARPHRD_NETROM: media="NET/ROM pseudo"; break;
            case ARPHRD_ETHER: media="Ethernet"; break;
            case ARPHRD_EETHER: media="Experimenal Ethernet"; break;
            case ARPHRD_AX25: media="AX.25 Level 2"; break;
            case ARPHRD_PRONET: media="PROnet tokenring"; break;
            case ARPHRD_CHAOS: media="Chaosnet"; break;
            case ARPHRD_IEEE802: media="ATM"; break;
            case ARPHRD_ARCNET: media="ARCnet"; break;
            case ARPHRD_APPLETLK: media="APPLEtalk"; break;
            case ARPHRD_DLCI: media="Frame Relay DLCI"; break;
            case ARPHRD_ATM: media="ATM"; break;
            case ARPHRD_METRICOM: media="Metricom STRIP (new IANA id)"; break;
            case ARPHRD_IEEE1394: media="IEEE1394 IPv4 - RFC 2734"; break;
            default: media="Unknown"; break;
            }

            hal_device_set_property_string(d, "net.media", media);
            hal_device_set_property_int(d, "net.arp_proto_hw_id", 
                                        type);
        }
    }

    /* check for driver */
    if( class_device->driver!=NULL )
    {
        hal_device_set_property_string(d, "linux.driver", 
                                       class_device->driver->name);        
    }

}

/** Init function for block device handling
 *
 */
void hal_net_init()
{
}

/** Shutdown function for block device handling
 *
 */
void hal_net_shutdown()
{
}

