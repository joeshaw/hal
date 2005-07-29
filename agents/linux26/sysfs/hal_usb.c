/***************************************************************************
 * CVSID: $Id$
 *
 * hal_usb.c : USB functions for sysfs-agent on Linux 2.6
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
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
#include "hal_usb.h"

/** Pointer to where the usb.ids file is loaded */
static char* usb_ids = NULL;

/** Length of data store at at usb_ids */
static unsigned int usb_ids_len;

/** Iterator position into usb_ids */
static unsigned int usb_ids_iter_pos;

/** Initialize the usb.ids line iterator to the beginning of the file */
static void usb_ids_line_iter_init()
{
    usb_ids_iter_pos = 0;
}

/** Maximum length of lines in usb.ids */
#define USB_IDS_MAX_LINE_LEN 512

/** Get the next line from usb.ids
 *
 *  @param  line_len            Pointer to where number of bytes in line will
 *                              be stored
 *  @return                     Pointer to the line; only valid until the
 *                              next invocation of this function
 */
static char* usb_ids_line_iter_get_line(unsigned int* line_len)
{
    unsigned int i;
    static char line[USB_IDS_MAX_LINE_LEN];

    for(i=0; 
        usb_ids_iter_pos<usb_ids_len && 
            i<USB_IDS_MAX_LINE_LEN-1 && 
            usb_ids[usb_ids_iter_pos]!='\n';
        i++, usb_ids_iter_pos++)
    {
        line[i] = usb_ids[usb_ids_iter_pos];
    }

    line[i] = '\0';
    if( line_len!=NULL )
        *line_len = i;

    usb_ids_iter_pos++;
            
    return line;
}

/** See if there are more lines to process in usb.ids
 *
 *  @return                     #TRUE iff there are more lines to process
 */
static dbus_bool_t usb_ids_line_iter_has_more()
{
    return usb_ids_iter_pos<usb_ids_len;
}

/** Find the names for a USB device.
 *
 *  The pointers returned are only valid until the next invocation of this
 *  function.
 *
 *  @param  vendor_id           USB vendor id or 0 if unknown
 *  @param  product_id          USB product id or 0 if unknown
 *  @param  vendor_name         Set to pointer of result or #NULL
 *  @param  product_name        Set to pointer of result or #NULL
 */
static void usb_ids_find(int vendor_id, int product_id,
                         char** vendor_name, char** product_name)
{
    char* line;
    unsigned int i;
    unsigned int line_len;
    unsigned int num_tabs;
    char rep_vi[8];
    char rep_pi[8];
    static char store_vn[USB_IDS_MAX_LINE_LEN];
    static char store_pn[USB_IDS_MAX_LINE_LEN];
    dbus_bool_t vendor_matched=FALSE;

    snprintf(rep_vi, 8, "%04x", vendor_id);
    snprintf(rep_pi, 8, "%04x", product_id);

    *vendor_name = NULL;
    *product_name = NULL;

    for(usb_ids_line_iter_init(); usb_ids_line_iter_has_more(); )
    {
        line = usb_ids_line_iter_get_line(&line_len);

        /* skip lines with no content */
        if( line_len<4 )
            continue;

        /* skip comments */
        if( line[0]=='#' )
            continue;

        /* count number of tabs */
        num_tabs = 0;
        for(i=0; i<line_len; i++)
        {
            if( line[i]!='\t' )
                break;
            num_tabs++;
        }

        switch( num_tabs )
        {
        case 0:
            /* vendor names */
            vendor_matched = FALSE;

            /* check vendor_id */
            if( vendor_id!=0 )
            {
                if( memcmp(line, rep_vi, 4)==0 )
                {
                    /* found it */
                    vendor_matched = TRUE;

                    for(i=4; i<line_len; i++)
                    {
                        if( !isspace(line[i]) )
                            break;
                    }
                    strncpy(store_vn, line+i, USB_IDS_MAX_LINE_LEN);
                    *vendor_name = store_vn;
                }
            }            
            break;

        case 1:
            /* product names */
            if( !vendor_matched )
                continue;

            /* check product_id */
            if( product_id!=0 )
            {
                if( memcmp(line+1, rep_pi, 4)==0 )
                {
                    /* found it */
                    for(i=5; i<line_len; i++)
                    {
                        if( !isspace(line[i]) )
                            break;
                    }
                    strncpy(store_pn, line+i, USB_IDS_MAX_LINE_LEN);
                    *product_name = store_pn;

                    /* no need to continue the search */
                    return;
                }
            }
            break;

        default:
            break;
        }
        
    }
}

/** Load the USB database used for mapping vendor, product, subsys_vendor
 *  and subsys_product numbers into names.
 *
 *  @param  path                Path of the usb.ids file, e.g. 
 *                              /usr/share/hwdata/usb.ids
 *  @return                     #TRUE if the file was succesfully loaded
 */
static dbus_bool_t usb_ids_load(const char* path)
{
    FILE* fp;
    unsigned int num_read;

    fp = fopen(path, "r");
    if( fp==NULL )
    {
        printf("couldn't open USB database at %s,", path);
        return FALSE;
    }

    fseek(fp, 0, SEEK_END);
    usb_ids_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    usb_ids = malloc(usb_ids_len);
    if( usb_ids==NULL )
    {
        printf("Couldn't allocate %d bytes for USB database file\n",
               usb_ids_len);
        return FALSE;
    }
    
    num_read = fread(usb_ids, sizeof(char), usb_ids_len, fp);
    if( usb_ids_len!=num_read )
    {
        printf("Error loading USB database file\n");
        free(usb_ids);
        usb_ids=NULL;
        return FALSE;
    }    

    return TRUE;
}

/** Free resources used by to store the USB database
 *
 *  @param                      #FALSE if the USB database wasn't loaded
 */
static dbus_bool_t usb_ids_free()
{
    if( usb_ids!=NULL )
    {
        free(usb_ids);
        usb_ids=NULL;
        return TRUE;
    }
    return FALSE;
}


/** Key information about USB devices from /proc that is not available 
 *  in sysfs
 */
typedef struct usb_proc_info_s
{
    int t_bus;               /**< Bus number */
    int t_level;             /**< Level in topology (depth) */
    int t_parent;            /**< Parent DeviceNumber */
    int t_port;              /**< Port on Parent for this device */
    int t_count;             /**< Count of devices at this level */
    int t_device;            /**< DeviceNumber */
    int t_speed_bcd;         /**< Device Speed in Mbps, encoded as BCD */
    int t_max_children;      /**< Maximum number of children */
    int d_version_bcd;       /**< USB version, encoded in BCD */

    struct usb_proc_info_s* next; /**< next element or #NULL if last */
} usb_proc_info;

/** First element in usb proc linked list */
static usb_proc_info* usb_proc_head = NULL;

/** Unique device id of the device we are working on */
static usb_proc_info* usb_proc_cur_info = NULL;

/** Find the USB virtual root hub device for a USB bus.
 *
 *  @param  bus_number          USB bus number
 *  @return                     The #usb_proc_info structure with information
 *                              retrieved from /proc or #NULL if not found
 */
static usb_proc_info* usb_proc_find_virtual_hub(int bus_number)
{
    usb_proc_info* i;
    for(i=usb_proc_head; i!=NULL; i=i->next)
    {
        if( i->t_bus==bus_number && i->t_level==0 )
            return i;
    }

    return NULL;
}


/** Find a child of a USB virtual root hub device for a USB bus.
 *
 *  @param  bus_number          USB bus number
 *  @param  port_number         The port number, starting from 1
 *  @return                     The #usb_proc_info structure with information
 *                              retrieved from /proc or #NULL if not found
 */
static usb_proc_info* usb_proc_find_virtual_hub_child(int bus_number,
                                                      int port_number)
{
    usb_proc_info* i;
    for(i=usb_proc_head; i!=NULL; i=i->next)
    {
        /* Note that /proc counts port starting from zero */
        if( i->t_bus==bus_number && i->t_level==1 && 
            i->t_port==port_number-1 )
            return i;
    }

    return NULL;
}

/** Find a child of a given hub device given a bus and port number
 *
 *  @param  bus_number           USB bus number
 *  @param  port_number          The port number, starting from 1
 *  @param  parent_device_number The Linux device number
 *  @return                      The #usb_proc_info structure with information
 *                               retrieved from /proc or #NULL if not found
 */
static usb_proc_info* usb_proc_find_on_hub(int bus_number, int port_number, 
                                           int parent_device_number)
{
    usb_proc_info* i;
    for(i=usb_proc_head; i!=NULL; i=i->next)
    {
        /* Note that /proc counts port starting from zero */
        if( i->t_bus==bus_number  && i->t_port==port_number-1 && 
            i->t_parent==parent_device_number )
            return i;
    }

    return NULL;
}


/** Parse the topology field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/usb/devices starting
 *                              with "T:"
 */
static void usb_proc_handle_topology(usb_proc_info* info, char* s)
{
    info->t_bus = find_num("Bus=", s, 10);
    info->t_level = find_num("Lev=", s, 10);
    info->t_parent = find_num("Prnt=", s, 10);
    info->t_port = find_num("Port=", s, 10);
    info->t_count = find_num("Cnt=", s, 10);
    info->t_device = find_num("Dev#=",s, 10);
    info->t_speed_bcd = find_bcd2("Spd=",s);
    info->t_max_children = find_num("MxCh=",s, 10);
}

/** Parse the device descriptor field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/usb/devices starting
 *                              with "D:"
 */
static void usb_proc_handle_device_info(usb_proc_info* info, char* s)
{
    info->d_version_bcd = find_bcd2("Ver=",s);
}


/** Called when an entry from /proc/bus/usb/devices have been parsed.
 *
 *  @param  info                Structure representing the entry
 */
static void usb_proc_device_done(usb_proc_info* info)
{
    info->next = usb_proc_head;
    usb_proc_head = info;
}



/** Parse a line from /proc/bus/usb/devices
 *
 *  @param  s                   Line from /proc/bus/usb/devices
 */
static void usb_proc_parse_line(char* s)
{
    switch( s[0] )
    {
    case 'T': /* topology; always present, indicates a new device */
        if( usb_proc_cur_info!=NULL )
        {
            // beginning of a new device, done with current
            usb_proc_device_done(usb_proc_cur_info);
        }

        usb_proc_cur_info = malloc(sizeof(usb_proc_info));

        if( usb_proc_cur_info==NULL )
            DIE(("Cannot allocated memory"));

        usb_proc_handle_topology(usb_proc_cur_info, s);
        break;

    case 'B': /* bandwidth */
        break;

    case 'D': /* device information */
        usb_proc_handle_device_info(usb_proc_cur_info, s);
        break;

    case 'P': /* more device information */
        break;

    case 'S': /* device string information */
        break;

    case 'C': /* config descriptor info */
        break;

    case 'I': /* interface descriptor info */
        break;

    case 'E': /* endpoint descriptor info */
        break;

    default:
        break;
    }
}

/** Parse /proc/bus/usb/devices
 */
static void usb_proc_parse()
{
    FILE* f;
    char buf[256];

    f = fopen("/proc/bus/usb/devices", "r");
    if( f==NULL )
    {
        DIE(("Couldn't open /proc/bus/usb/devices"));
    }

    while( !feof(f) )
    {
        fgets(buf, 256, f);
        usb_proc_parse_line(buf);
    }
    usb_proc_device_done(usb_proc_cur_info);

/*
    {
        usb_proc_info* i;
        for(i=usb_proc_head; i!=NULL; i=i->next)
        {
            printf("/p/b/u/d entry\n");
            printf("  bus               %d\n", i->t_bus);
            printf("  level             %d\n", i->t_level);
            printf("  parent            %d\n", i->t_parent);
            printf("  port              %d\n", i->t_port);
            printf("  count             %d\n", i->t_count);
            printf("  device            %d\n", i->t_device);
            printf("  speed_bcd         %x.%x (0x%06x)\n", i->t_speed_bcd>>8, 
                   i->t_speed_bcd&0xff, i->t_speed_bcd);
            printf("  max_children      %d\n", i->t_max_children);
            printf("  version_bcd       %x.%x (0x%06x)\n", i->d_version_bcd>>8,
                   i->d_version_bcd&0xff, i->d_version_bcd);
            printf("\n");
        }
    }
*/
}


/** This function will compute the device uid based on other properties
 *  of the device. For USB interfaces it basically is the physical USB
 *  device UDI appended with the interface number
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* usbif_compute_udi(const char* udi, int append_num)
{
    char* format;
    char* pd;
    char* name;
    int i, len;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/usbif_%s_%d";
    else
        format = "/org/freedesktop/Hal/devices/usbif_%s_%d-%d";

    pd = hal_device_get_property_string(udi, "Parent");
    len = strlen(pd);
    for(i=len-1; pd[i]!='/' && i>=0; i--)
        ;
    name = pd+i+1;

    snprintf(buf, 256, format, 
             name,
             hal_device_get_property_int(udi, "usbif.number"),
             append_num);

    free(pd);
    
    return buf;
}


/** This function will compute the device uid based on other properties
 *  of the device. Specifically, the following properties are required:
 *
 *   - usb.idVendor, usb.idProduct, usb.bcdDevice. 
 *
 *  Other properties may also be used, specifically the usb.SerialNumber 
 *  is used if available.
 *
 *  Requirements for uid:
 *   - do not rely on bus, port etc.; we want this id to be as unique for
 *     the device as we can
 *   - make sure it doesn't rely on properties that cannot be obtained
 *     from the minimal information we can obtain on an unplug event
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* usb_compute_udi(const char* udi, int append_num)
{
    char* serial;
    char* format;
    static char buf[256];

    if( append_num==-1 )
        format = "/org/freedesktop/Hal/devices/usb_%x_%x_%x_%d_%s";
    else
        format = "/org/freedesktop/Hal/devices/usb_%x_%x_%x_%d_%s-%d";

    if( hal_device_property_exists(udi, "usb.serial") )
        serial = hal_device_get_property_string(udi, "usb.serial");
    else
        serial = "noserial";

    snprintf(buf, 256, format, 
             hal_device_get_property_int(udi, "usb.idVendor"),
             hal_device_get_property_int(udi, "usb.idProduct"),
             hal_device_get_property_int(udi, "usb.bcdDevice"),
             hal_device_get_property_int(udi, "usb.configurationValue"),
             serial, append_num);
    
    return buf;
}

/** Set capabilities from interface class. This is a function from hell,
 *  maybe some searchable data-structure would be better...
 *
 *  @param  udi                 UDI of HAL device to set caps on
 *  @param  if_class            Interface class
 *  @param  if_sub_class        Interface sub class
 *  @param  if_proto            Interface protocol
 */
static void usb_add_caps_from_class(const char* udi,
                                    int if_class, 
                                    int if_sub_class, 
                                    int if_proto)
{
    char* cat = "unknown";

    switch( if_class )
    {
    case 0x01:
        cat = "multimedia.audio";
        hal_device_add_capability(udi, "multimedia.audio");
        break;
    case 0x02:
        if( if_sub_class==0x06 )
        {
            cat = "net";
            hal_device_add_capability(udi, "net");
            hal_device_add_capability(udi, "net.ethernet");
        }
        else if( if_sub_class==0x02 && if_proto==0x01 )
        {
            cat = "modem";
            hal_device_add_capability(udi, "modem");
        }
        break;
    case 0x03:
        cat = "input";
        hal_device_add_capability(udi, "input");
        if( if_sub_class==0x00 || if_sub_class==0x01 )
        {
            if( if_proto==0x01 )
            {
                cat = "input.keyboard";
                hal_device_add_capability(udi, "input.keyboard");
            }
            else if( if_proto==0x02 )
            {
                cat = "input.mouse";
                hal_device_add_capability(udi, "input.mouse");
            }
        }
        break;
    case 0x04:
        break;
    case 0x05:
        break;
    case 0x06:
        break;
    case 0x07:
        cat = "printer";
        hal_device_add_capability(udi, "printer");
        break;
    case 0x08:
        cat = "storage";
        hal_device_add_capability(udi, "storage");
        break;
    case 0x09:
        cat = "hub";
        hal_device_add_capability(udi, "hub");
        break;
    case 0x0a:
        break;
    }

    hal_device_set_property_string(udi, "Category", cat);
}

/** Visitor function for interfaces on a USB device.
 *
 *  @param  path                 Sysfs-path for USB interface
 *  @param  device               libsysfs object for USB interface
 */
static void visit_device_usb_interface(const char* path,
                                       struct sysfs_device *device)
{
    int i;
    int len;
    struct sysfs_attribute* cur;
    char* d;
    char* pd;
    const char* driver;
    char attr_name[SYSFS_NAME_LEN];
    int if_class = -1;
    int if_sub_class = -1;
    int if_proto = -1;

    /*printf("usb_interface: path=%s\n", path);*/

    /* Find parent USB device - this may block..  */
    pd = find_parent_udi_from_sysfs_path(path, HAL_LINUX_HOTPLUG_TIMEOUT);


    /* Create HAL device representing the interface */
    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "usbif");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", path);
    /** @note We also set the path here, because otherwise we can't handle two
     *  identical devices per the algorithm used in a #rename_and_maybe_add()
     *  The point is that we need something unique in the Bus namespace
     */
    hal_device_set_property_string(d, "usbif.linux.sysfs_path", path);
    hal_device_set_property_string(d, "PhysicalDevice", pd);
    hal_device_set_property_bool(d, "isVirtual", TRUE);
    hal_device_set_property_string(d, "Parent", pd);

    /* set driver */
    driver = drivers_lookup(path);
    if( driver!=NULL )
        hal_device_set_property_string(d, "linux.driver", driver);


    hal_device_set_property_int(d, "usbif.deviceIdVendor",
        hal_device_get_property_int(pd, "usb.idVendor"));
    hal_device_set_property_int(d, "usbif.deviceIdProduct",
        hal_device_get_property_int(pd, "usb.idProduct"));

    if( device->directory==NULL || device->directory->attributes==NULL )
        return;

    dlist_for_each_data(sysfs_get_device_attributes(device), cur,
                        struct sysfs_attribute)
    {
        
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        /* strip whitespace */
        len = strlen(cur->value);
        for(i=len-1; i>0 && isspace(cur->value[i]); --i)
            cur->value[i] = '\0';
        
        /*printf("attr_name=%s -> '%s'\n", attr_name, cur->value);*/

        if( strcmp(attr_name, "bInterfaceClass")==0 )
            if_class = parse_dec(cur->value);
        else if( strcmp(attr_name, "bInterfaceSubClass")==0 )
            if_sub_class = parse_dec(cur->value);
        else if( strcmp(attr_name, "bInterfaceProtocol")==0 )
            if_proto = parse_dec(cur->value);
        else if( strcmp(attr_name, "bInterfaceNumber")==0 )
            hal_device_set_property_int(d, "usbif.number", 
                                        parse_dec(cur->value));
    }

    hal_device_set_property_int(d, "usbif.bInterfaceClass", if_class);
    hal_device_set_property_int(d, "usbif.bInterfaceSubClass", if_sub_class);
    hal_device_set_property_int(d, "usbif.bInterfaceProtocol", if_proto);

    /* We set the caps on the parent device */
    usb_add_caps_from_class(pd, if_class, if_sub_class, if_proto);
    

    d = rename_and_maybe_add(d, usbif_compute_udi, "usbif");
}

/** Visitor function for USB device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_device_usb(const char* path, struct sysfs_device *device)
{
    int i;
    int len;
    dbus_bool_t is_interface;
    struct sysfs_attribute* cur;
    const char* d;
    const char* parent_udi;
    char attr_name[SYSFS_NAME_LEN];
    int vendor_id=0;
    int product_id=0;
    char* vendor_name;
    char* product_name;
    char* vendor_name_kernel = NULL;
    char* product_name_kernel = NULL;
    const char* driver;
    int bus_number;
    usb_proc_info* proc_info;

    /*printf("usb: %s, bus_id=%s\n", path, device->bus_id);*/

    if( device->directory==NULL || device->directory->attributes==NULL )
        return;

    /* Check if this is an USB interface */
    is_interface = FALSE;
    dlist_for_each_data(sysfs_get_device_attributes(device), cur,
                        struct sysfs_attribute)
    {
        if( is_interface )
            break;

        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;
        
        if( strcmp(attr_name, "iInterface")==0 )
            is_interface = TRUE;
    }
    
    /* USB interfaces are handled by a separate function */
    if( is_interface )
    {
        visit_device_usb_interface(path, device);
        return;
    }
    
    /* Must be a new USB device */
    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "usb");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", path);
    /** @note We also set the path here, because otherwise we can't handle two
     *  identical devices per the algorithm used in a #rename_and_maybe_add()
     *  The point is that we need something unique in the Bus namespace
     */
    hal_device_set_property_string(d, "usb.linux.sysfs_path", path);
    /*printf("*** created udi=%s for path=%s\n", d, path);*/

    /* set driver */
    driver = drivers_lookup(path);
    if( driver!=NULL )
        hal_device_set_property_string(d, "linux.driver", driver);
    
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
        
        if( strcmp(attr_name, "idProduct")==0 )
            product_id = parse_hex(cur->value);
        else if( strcmp(attr_name, "idVendor")==0 )
            vendor_id = parse_hex(cur->value);
        else if( strcmp(attr_name, "bcdDevice")==0 )
            hal_device_set_property_int(d, "usb.bcdDevice", 
                                        parse_hex(cur->value));
        else if( strcmp(attr_name, "bMaxPower")==0 )
            hal_device_set_property_int(d, "usb.bMaxPower", 
                                        parse_dec(cur->value));
        else if( strcmp(attr_name, "serial")==0 && strlen(cur->value)>0 )
            hal_device_set_property_string(d, "usb.serial", cur->value);
        else if( strcmp(attr_name, "bmAttributes")==0 )
        {
            int bmAttributes = parse_hex(cur->value);

            /* USB_CONFIG_ATT_SELFPOWER */
            hal_device_set_property_bool(d, "usb.selfPowered",
                                         (bmAttributes&0x40)!=0 );
            hal_device_set_property_bool(d, "usb.canWakeUp",
                                         (bmAttributes&0x20)!=0 );
        }
/*
        else if( strcmp(attr_name, "speed")==0 )
            hal_device_set_property_double(d, "usb.speed", 
                                           parse_double(cur->value));
*/
        
        else if( strcmp(attr_name, "manufacturer")==0 )
            vendor_name_kernel = cur->value;
        else if( strcmp(attr_name, "product")==0 )
            product_name_kernel = cur->value;
        else if( strcmp(attr_name, "bDeviceClass")==0 )
            hal_device_set_property_int(d, "usb.bDeviceClass", 
                                        parse_hex(cur->value));
        else if( strcmp(attr_name, "bDeviceSubClass")==0 )
            hal_device_set_property_int(d, "usb.bDeviceSubClass", 
                                        parse_hex(cur->value));
        else if( strcmp(attr_name, "bDeviceProtocol")==0 )
            hal_device_set_property_int(d, "usb.bDeviceProtocol", 
                                        parse_hex(cur->value));
        
        else if( strcmp(attr_name, "bNumConfigurations")==0 )
            hal_device_set_property_int(d, "usb.numConfigurations", 
                                        parse_dec(cur->value));
        else if( strcmp(attr_name, "bConfigurationValue")==0 )
            hal_device_set_property_int(d, "usb.configurationValue", 
                                        parse_dec(cur->value));
        
        else if( strcmp(attr_name, "bNumInterfaces")==0 )
            hal_device_set_property_int(d, "usb.numInterfaces", 
                                        parse_dec(cur->value));
        
    } /* for all attributes */

    hal_device_set_property_int(d, "usb.idProduct", product_id);
    hal_device_set_property_int(d, "usb.idVendor", vendor_id);

    /* Lookup names in usb.ids; these may override what the kernel told
     * us, but, hey, it's only a name; it's not something we are going
     * to match a device on... We prefer names from usb.ids as the kernel
     * name sometimes is just a hexnumber :-/
     *
     * Also provide best guess on name, Product and Vendor properties;
     * these can both be overridden in .fdi files.
     */
    usb_ids_find(vendor_id, product_id, &vendor_name, &product_name);
    if( vendor_name!=NULL )
    {
        hal_device_set_property_string(d, "usb.Vendor", vendor_name);
        hal_device_set_property_string(d, "Vendor", vendor_name);
    }
    else
    {
        /* fallback on name supplied from kernel */
        hal_device_set_property_string(d, "usb.Vendor", vendor_name_kernel);
        hal_device_set_property_string(d, "Vendor", vendor_name_kernel);
    }

    if( product_name!=NULL )
    {
        hal_device_set_property_string(d, "usb.Product", product_name);
        hal_device_set_property_string(d, "Product", product_name);
    }
    else
    {
        /* fallback on name supplied from kernel */
        hal_device_set_property_string(d, "usb.Product", product_name_kernel);
        hal_device_set_property_string(d, "Product", product_name_kernel);
    }

    /* Compute parent */
    parent_udi = find_parent_udi_from_sysfs_path(path, HAL_LINUX_HOTPLUG_TIMEOUT);
    if( parent_udi!=NULL )
        hal_device_set_property_string(d, "Parent", parent_udi);

    /* Merge information from /proc/bus/usb/devices */
    proc_info = NULL;

    if( sscanf(device->bus_id, "usb%d", &bus_number)==1 )
    {
        /* Is of form "usb%d" which means that this is a USB virtual root
         * hub, cf. drivers/usb/hcd.c in kernel 2.6
         */
        hal_device_set_property_int(d, "usb.busNumber", bus_number);
        proc_info = usb_proc_find_virtual_hub(bus_number);
    }
    else
    {
        int i;
        int len;
        int digit;
        int port_number;
        /* Not a root hub; According to the Linux kernel sources,
         * the name is of the form
         *
         *  "%d-%s[.%d]"
         *
         * where the first number is the bus-number, the middle string is
         * the parent device and the last, optional, number is the port
         * number in the event that the USB device is a hub.
         */

        len = strlen(device->bus_id);

        /* the first part is easy */
        bus_number = atoi(device->bus_id);

        hal_device_set_property_int(d, "usb.busNumber", bus_number);

        /* The naming convention also guarantees that
         *
         *   device is on a (non-virtual) hub    
         *
         *            IF AND ONLY IF  
         *
         *   the bus_id contains a "."
         */
        for(i=0; i<len; i++)
        {
            if( device->bus_id[i]=='.' )
                break;
        }

        if( i==len )
        {
            /* Not on a hub; this means we must be a child of the root
             * hub... Thus the name must is of the form "%d-%d"
             */
            if( sscanf(device->bus_id, "%d-%d", 
                       &bus_number, &port_number) == 2 )
            {

                proc_info = usb_proc_find_virtual_hub_child(bus_number, 
                                                            port_number);
                hal_device_set_property_int(d, "usb.portNumber", port_number);
            }
        }
        else
        {
            int parent_device_number;

            /* On a hub */

            /* This is quite a hack */
            port_number = 0;
            for(i=len-1; i>0 && isdigit(device->bus_id[i]); --i)
            {
                digit = (int)(device->bus_id[i] - '0');
                port_number *= 10;
                port_number += digit;
            }

            hal_device_set_property_int(d, "usb.portNumber", port_number);

            /* Ok, got the port number and bus number; this is not quite
             * enough though.. We take the usb.linux.device_number from
             * our parent and then we are set.. */
            if( parent_udi==NULL )
            {
                fprintf(stderr, "Device %s is on a hub but no parent??\n", d);
                /* have to give up then */
                proc_info = NULL;
            }
            else
            {
                parent_device_number = 
                    hal_device_get_property_int(parent_udi,
                                                "usb.linux.device_number");
                //printf("parent_device_number = %d\n", parent_device_number);
                proc_info = usb_proc_find_on_hub(bus_number, port_number,
                                                 parent_device_number);
            }

        }
    }


    if( proc_info!=NULL )
    {
        char kernel_path[32+1];

        hal_device_set_property_int(d, "usb.levelNumber", proc_info->t_level);
        hal_device_set_property_int(d, "usb.linux.device_number",
                                    proc_info->t_device);
        hal_device_set_property_int(d, "usb.linux.parent_number",
                                    proc_info->t_device);
        hal_device_set_property_int(d, "usb.numPorts", 
                                    proc_info->t_max_children);
        hal_device_set_property_int(d, "usb.bcdSpeed", proc_info->t_speed_bcd);
        hal_device_set_property_int(d, "usb.bcdVersion", 
                                    proc_info->d_version_bcd);

        /* Ok, now compute the unique name that the kernel sometimes use
         * to refer to the device; it's #usb_make_path() as defined in
         * include/linux/usb.h
         */
        if( proc_info->t_level==0 )
        {
            snprintf(kernel_path, 32, "usb-%s", 
                     hal_device_get_property_string(d, "usb.serial"));
            hal_device_set_property_string(d, "linux.kernel_devname",
                                           kernel_path);
        }
        else
        {
            if( parent_udi!=NULL )
            {
                if( proc_info->t_level==1 )
                {
                    snprintf(kernel_path, 32, "%s-%d", 
                             hal_device_get_property_string(parent_udi, 
                                                     "linux.kernel_devname"),
                             hal_device_get_property_int(d, "usb.portNumber"));
                }
                else
                {
                    snprintf(kernel_path, 32, "%s.%d", 
                             hal_device_get_property_string(parent_udi, 
                                                     "linux.kernel_devname"),
                             hal_device_get_property_int(d, "usb.portNumber"));
                }
                hal_device_set_property_string(d, "linux.kernel_devname",
                                               kernel_path);
            }
        }

    }

    /* Uncomment this line to test that sleeping works when handling USB
     * interfaces on not-yet added USB devices
     *
     * (you might need to tweak number of seconds to fit with your system)
     */
    /*sleep(5);*/

    /* Finally, Compute a proper UDI (unique device id), try to locate
     * a persistent unplugged device or add it
     */
    d = rename_and_maybe_add(d, usb_compute_udi, "usb");    

}


/** Init function for USB handling
 *
 */
void hal_usb_init()
{

    /* get all drivers under /sys/bus/usb/drivers */
    drivers_collect("usb");

    /* Load /usr/share/hwdata/usb.ids */
    /** @todo Hardcoding path to usb.ids is a hack */
    usb_ids_load("/usr/share/hwdata/usb.ids");

    /* Parse /proc/bus/usb/devices */
    usb_proc_parse();
}

/** Shutdown function for USB handling
 *
 */
void hal_usb_shutdown()
{
    usb_ids_free();
}

