/***************************************************************************
 * CVSID: $Id$
 *
 * linux_usb.c : USB handling on Linux 2.6
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2004 Novell, Inc.
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
#include "linux_ieee1394.h"

/**
 * @defgroup HalDaemonLinuxIeee1394 IEEE1394
 * @ingroup HalDaemonLinux
 * @brief IEEE1394
 * @{
 */


/** This functions computes the device UDI based on other properties
 *  of the device.  For IEEE1394, this uses the bus identifier.
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* ieee1394_compute_udi(HalDevice* d, int append_num)
{
    const char* serial;
    const char* format;
    static char buf[256];

    if (append_num == -1) {
	    snprintf(buf, 256, "/org/freedesktop/Hal/devices/ieee1394_%s",
		     ds_property_get_string(d, "linux.sysfs_bus_id"));
    } else {
	    snprintf(buf, 256, "/org/freedesktop/Hal/devices/ieee1394_%s-%d",
		     ds_property_get_string(d, "linux.sysfs_bus_id"),
		     append_num);
    }
    
    return buf;
}

/** Callback when the parent is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void visit_device_ieee1394_got_parent(HalDevice* parent, 
					     void* data1, void* data2)
{
    char* new_udi = NULL;
    HalDevice* new_d = NULL;
    int bus_number;
    const char* bus_id;
    HalDevice* d = (HalDevice*) data1;

    if( parent!=NULL )
    {
        ds_property_set_string(d, "info.parent", parent->udi);
    }
    else
    {
        /* An IEEE1394 device should always have a parent! */
        HAL_WARNING(("No parent for IEEE1394 device!"));
    }

    new_udi = rename_and_merge(d, ieee1394_compute_udi, "ieee1394");
    if( new_udi!=NULL )
    {
        new_d = ds_device_find(new_udi);
        if( new_d!=NULL )
        {
            ds_gdl_add(new_d);
        }
    }
}

static void
extract_properties (struct sysfs_device *device, HalDevice *d)
{
    struct sysfs_attribute* cur;
    char attr_name[SYSFS_NAME_LEN];
    int len, i;

    dlist_for_each_data(sysfs_get_device_attributes(device), cur,
                        struct sysfs_attribute)
    {
        
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        /* strip whitespace */
        len = strlen(cur->value);
        for(i=len-1; i>=0 && isspace(cur->value[i]); --i)
            cur->value[i] = '\0';

        /*printf("attr_name=%s -> '%s'\n", attr_name, cur->value);*/
        
	if (strcmp (attr_name, "is_root") == 0) {
            struct sysfs_link *link;

	    link = sysfs_get_directory_link (device->directory,
					     "host_id");
		
	    if (link != NULL) {
	        struct sysfs_device *real_device;
		    
		real_device = sysfs_open_device (link->target);
		
		extract_properties (real_device, d);
	    }

	} else if (strcmp (attr_name, "model_name") == 0) {
            ds_property_set_string (d, "info.product", cur->value);

	} else if (strcmp (attr_name, "vendor_name") == 0) {
	    ds_property_set_string (d, "info.vendor", cur->value);

	} else if (strcmp (attr_name, "capabilities") == 0) {
		ds_property_set_int (d, "ieee1394.capabilities",
				     parse_hex(cur->value));

	} else if (strcmp (attr_name, "guid") == 0) {
		/* FIXME: This is a 64-bit integer value */
		ds_property_set_string (d, "ieee1394.guid",
					cur->value);

	} else if (strcmp (attr_name, "guid_vendor_id") == 0) {
		ds_property_set_int (d, "ieee1394.guid_vendor_id",
				     parse_hex(cur->value));

	} else if (strcmp (attr_name, "guid_vendor_oui") == 0) {
		ds_property_set_string (d, "ieee1394.guid_vendor_oui",
					cur->value);

	} else if (strcmp (attr_name, "model_id") == 0) {
		ds_property_set_int (d, "ieee1394.model_id",
				     parse_hex(cur->value));

	} else if (strcmp (attr_name, "model_name") == 0) {
		ds_property_set_string (d, "ieee1394.model_name",
					cur->value);

	} else if (strcmp (attr_name, "nodeid") == 0) {
		ds_property_set_int (d, "ieee1394.nodeid",
				     parse_hex(cur->value));

	} else if (strcmp (attr_name, "specifier_id") == 0) {
		ds_property_set_int (d, "ieee1394.specifier_id",
				     parse_hex(cur->value));

	} else if (strcmp (attr_name, "vendor_id") == 0) {
		ds_property_set_int (d, "ieee1394.vendor_id",
				     parse_hex(cur->value));

	} else if (strcmp (attr_name, "vendor_name") == 0) {
		ds_property_set_string (d, "ieee1394.vendor_name",
					cur->value);

	} else if (strcmp (attr_name, "vendor_oui") == 0) {
		ds_property_set_string (d, "ieee1394.vendor_oui",
					cur->value);

	} else if (strcmp (attr_name, "version") == 0) {
		ds_property_set_int (d, "ieee1394.version",
				     parse_hex (cur->value));
	} 

    } /* for all attributes */
}

static void
add_capabilities (HalDevice *d)
{
    int specifier_id;

    specifier_id = ds_property_get_int (d, "ieee1394.specifier_id");

    /*
     * These specifier id values are taken from the modules.ieee1394
     * file and header files in linux/drivers/ieee1394 in the kernel
     * source.
     */

    if (specifier_id == 0x00609e) {
        ds_add_capability(d, "storage_controller");
	ds_property_set_string(d, "info.category", "storage_controller");
    } else if (specifier_id == 0x00005e) {
	ds_add_capability(d, "net");
	ds_add_capability(d, "net.ethernet");
	ds_property_set_string(d, "info.category", "net");
    }
}

/** Visitor function for IEEE1394 device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_device_ieee1394(const char* path, struct sysfs_device *device)
{
    dbus_bool_t is_interface;
    HalDevice* d;
    int vendor_id=0;
    int product_id=0;
    char* vendor_name;
    char* product_name;
    char* vendor_name_kernel = NULL;
    char* product_name_kernel = NULL;
    const char* driver;
    char* parent_sysfs_path;
    char numeric_name[32];

    /*printf("usb: %s, bus_id=%s\n", path, device->bus_id);*/

    if( device->directory==NULL || device->directory->attributes==NULL )
        return;

    d = ds_device_new();
    ds_property_set_string(d, "info.bus", "ieee1394");
    ds_property_set_string(d, "linux.sysfs_path", path);
    ds_property_set_string(d, "linux.sysfs_bus_id", device->bus_id);
    ds_property_set_string(d, "linux.sysfs_path_device", path);

    /*printf("*** created udi=%s for path=%s\n", d, path);*/

    /* set driver */
    driver = drivers_lookup(path);
    if( driver!=NULL )
        ds_property_set_string(d, "linux.driver", driver);
    
    extract_properties (device, d);

    add_capabilities (d);

    parent_sysfs_path = get_parent_sysfs_path(path);

    /* Find parent; this happens asynchronously as our parent might
     * be added later. If we are probing this can't happen so the
     * timeout is set to zero in that event..
     */
    ds_device_async_find_by_key_value_string("linux.sysfs_path_device",
                                             parent_sysfs_path,
                                             TRUE,
                                             visit_device_ieee1394_got_parent,
                                             (void*) d, NULL, 
                                             is_probing ? 0 :
                                             HAL_LINUX_HOTPLUG_TIMEOUT);

    free(parent_sysfs_path);
}

/** Init function for IEEE1394 handling
 *
 */
void linux_ieee1394_init()
{

    /* get all drivers under /sys/bus/ieee1394/drivers */
    drivers_collect("ieee1394");
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void linux_ieee1394_detection_done()
{
}

/** Shutdown function for IEEE1394 handling
 *
 */
void linux_ieee1394_shutdown()
{
}

/** @} */
