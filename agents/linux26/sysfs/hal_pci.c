/***************************************************************************
 * CVSID: $Id$
 *
 * hal_pci.c : PCI functions for sysfs-agent on Linux 2.6
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
#include "hal_pci.h"


/** Pointer to where the pci.ids file is loaded */
static char* pci_ids = NULL;

/** Length of data store at at pci_ids */
static unsigned int pci_ids_len;

/** Iterator position into pci_ids */
static unsigned int pci_ids_iter_pos;

/** Initialize the pci.ids line iterator to the beginning of the file */
static void pci_ids_line_iter_init()
{
    pci_ids_iter_pos = 0;
}

/** Maximum length of lines in pci.ids */
#define PCI_IDS_MAX_LINE_LEN 512

/** Get the next line from pci.ids
 *
 *  @param  line_len            Pointer to where number of bytes in line will
 *                              be stored
 *  @return                     Pointer to the line; only valid until the
 *                              next invocation of this function
 */
static char* pci_ids_line_iter_get_line(unsigned int* line_len)
{
    unsigned int i;
    static char line[PCI_IDS_MAX_LINE_LEN];

    for(i=0; 
        pci_ids_iter_pos<pci_ids_len && 
            i<PCI_IDS_MAX_LINE_LEN-1 && 
            pci_ids[pci_ids_iter_pos]!='\n';
        i++, pci_ids_iter_pos++)
    {
        line[i] = pci_ids[pci_ids_iter_pos];
    }

    line[i] = '\0';
    if( line_len!=NULL )
        *line_len = i;

    pci_ids_iter_pos++;
            
    return line;
}

/** See if there are more lines to process in pci.ids
 *
 *  @return                     #TRUE iff there are more lines to process
 */
static dbus_bool_t pci_ids_line_iter_has_more()
{
    return pci_ids_iter_pos<pci_ids_len;
}

/** Find the names for a PCI device.
 *
 *  The pointers returned are only valid until the next invocation of this
 *  function.
 *
 *  @param  vendor_id           PCI vendor id or 0 if unknown
 *  @param  product_id          PCI product id or 0 if unknown
 *  @param  subsys_vendor_id    PCI subsystem vendor id or 0 if unknown
 *  @param  subsys_product_id   PCI subsystem product id or 0 if unknown
 *  @param  vendor_name         Set to pointer of result or #NULL
 *  @param  product_name        Set to pointer of result or #NULL
 *  @param  subsys_vendor_name  Set to pointer of result or #NULL
 *  @param  subsys_product_name Set to pointer of result or #NULL
 */
static void pci_ids_find(int vendor_id, int product_id,
                         int subsys_vendor_id, int subsys_product_id,
                         char** vendor_name, char** product_name,
                         char** subsys_vendor_name,char** subsys_product_name)
{
    char* line;
    unsigned int i;
    unsigned int line_len;
    unsigned int num_tabs;
    char rep_vi[8];
    char rep_pi[8];
    char rep_svi[8];
    char rep_spi[8];
    static char store_vn[PCI_IDS_MAX_LINE_LEN];
    static char store_pn[PCI_IDS_MAX_LINE_LEN];
    static char store_svn[PCI_IDS_MAX_LINE_LEN];
    static char store_spn[PCI_IDS_MAX_LINE_LEN];
    dbus_bool_t vendor_matched=FALSE;
    dbus_bool_t product_matched=FALSE;

    snprintf(rep_vi, 8, "%04x", vendor_id);
    snprintf(rep_pi, 8, "%04x", product_id);
    snprintf(rep_svi, 8, "%04x", subsys_vendor_id);
    snprintf(rep_spi, 8, "%04x", subsys_product_id);

    *vendor_name = NULL;
    *product_name = NULL;
    *subsys_vendor_name = NULL;
    *subsys_product_name = NULL;

    for(pci_ids_line_iter_init(); pci_ids_line_iter_has_more(); )
    {
        line = pci_ids_line_iter_get_line(&line_len);

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

            /* first check subsys_vendor_id, if haven't done already */
            if( *subsys_vendor_name==NULL && subsys_vendor_id!=0 )
            {
                if( (*((dbus_uint32_t*)line))==(*((dbus_uint32_t*)rep_svi)) )
                {
                    /* found it */
                    for(i=4; i<line_len; i++)
                    {
                        if( !isspace(line[i]) )
                            break;
                    }
                    strncpy(store_svn, line+i, PCI_IDS_MAX_LINE_LEN);
                    *subsys_vendor_name = store_svn;
                }
            }
            
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
                    strncpy(store_vn, line+i, PCI_IDS_MAX_LINE_LEN);
                    *vendor_name = store_vn;
                }
            }
            
            break;

        case 1:
            product_matched = FALSE;

            /* product names */
            if( !vendor_matched )
                continue;

            /* check product_id */
            if( product_id!=0 )
            {
                if( memcmp(line+1, rep_pi, 4)==0 )
                {
                    /* found it */

                    product_matched = TRUE;

                    for(i=5; i<line_len; i++)
                    {
                        if( !isspace(line[i]) )
                            break;
                    }
                    strncpy(store_pn, line+i, PCI_IDS_MAX_LINE_LEN);
                    *product_name = store_pn;
                }
            }
            break;

        case 2:
            /* subsystem_vendor subsystem_product */
            if( !vendor_matched || !product_matched )
                continue;

            /* check product_id */
            if( subsys_vendor_id!=0 && subsys_product_id!=0 )
            {
                if( memcmp(line+2, rep_svi, 4)==0 &&
                    memcmp(line+7, rep_spi, 4)==0 )
                {
                    /* found it */
                    for(i=11; i<line_len; i++)
                    {
                        if( !isspace(line[i]) )
                            break;
                    }
                    strncpy(store_spn, line+i, PCI_IDS_MAX_LINE_LEN);
                    *subsys_product_name = store_spn;
                }
            }

            break;

        default:
            break;
        }
        
    }
}

/** Load the PCI database used for mapping vendor, product, subsys_vendor
 *  and subsys_product numbers into names.
 *
 *  @param  path                Path of the pci.ids file, e.g. 
 *                              /usr/share/hwdata/pci.ids
 *  @return                     #TRUE if the file was succesfully loaded
 */
static dbus_bool_t pci_ids_load(const char* path)
{
    FILE* fp;
    unsigned int num_read;

    fp = fopen(path, "r");
    if( fp==NULL )
    {
        printf("couldn't open PCI database at %s,", path);
        return FALSE;
    }

    fseek(fp, 0, SEEK_END);
    pci_ids_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    pci_ids = malloc(pci_ids_len);
    if( pci_ids==NULL )
    {
        printf("Couldn't allocate %d bytes for PCI database file\n",
               pci_ids_len);
        return FALSE;
    }
    
    num_read = fread(pci_ids, sizeof(char), pci_ids_len, fp);
    if( pci_ids_len!=num_read )
    {
        printf("Error loading PCI database file\n");
        free(pci_ids);
        pci_ids=NULL;
        return FALSE;
    }    

    return TRUE;
}

/** Free resources used by to store the PCI database
 *
 *  @param                      #FALSE if the PCI database wasn't loaded
 */
static dbus_bool_t pci_ids_free()
{
    if( pci_ids!=NULL )
    {
        free(pci_ids);
        pci_ids=NULL;
        return TRUE;
    }
    return FALSE;
}

/** This function will compute the device uid based on other properties
 *  of the device. Specifically, the following properties are required:
 *
 *   - pci.idVendor, pci.idProduct
 *
 *  Other properties may also be used.
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
static char* pci_compute_udi(const char* udi, int append_num)
{
    static char buf[256];

    /** @todo Rework this function to use e.g. serial number */

    if( append_num==-1 )
        sprintf(buf, "/org/freedesktop/Hal/devices/pci_%x_%x",
                hal_device_get_property_int(udi, "pci.idVendor"),
                hal_device_get_property_int(udi, "pci.idProduct"));
    else
        sprintf(buf, "/org/freedesktop/Hal/devices/pci_%x_%x/%d", 
                hal_device_get_property_int(udi, "pci.idVendor"),
                hal_device_get_property_int(udi, "pci.idProduct"),
                append_num);
    
    return buf;
}


/** Visitor function for PCI device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void visit_device_pci(const char* path, struct sysfs_device *device)
{
    int i;
    int len;
    char* parent_udi;
    const char* d;
    char attr_name[SYSFS_NAME_LEN];
    struct sysfs_attribute* cur;
    int vendor_id=0;
    int product_id=0;
    int subsys_vendor_id=0;
    int subsys_product_id=0;
    char* vendor_name;
    char* product_name;
    char* subsys_vendor_name;
    char* subsys_product_name;
    char namebuf[512];

    /*printf("pci: %s\n", path);*/

    /* Must be a new PCI device */
    d = hal_agent_new_device();
    assert( d!=NULL );
    hal_device_set_property_string(d, "Bus", "pci");
    hal_device_set_property_string(d, "Linux.sysfs_path", path);
    hal_device_set_property_string(d, "Linux.sysfs_path_device", path);
    /** @note We also set the path here, because otherwise we can't handle two
     *  identical devices per the algorithm used in a #rename_and_maybe_add()
     *  The point is that we need something unique in the Bus namespace
     */
    hal_device_set_property_string(d, "pci.linux.sysfs_path", path);
    /*printf("*** created udi=%s for path=%s\n", d, path);*/

    dlist_for_each_data(sysfs_get_device_attributes(device), cur,
                        struct sysfs_attribute)
    {
        
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;
        
        /* strip whitespace */
        len = strlen(cur->value);
        for(i=len-1; isspace(cur->value[i]); --i)
            cur->value[i] = '\0';
        
        /*printf("attr_name=%s -> '%s'\n", attr_name, cur->value);*/
        
        if( strcmp(attr_name, "device")==0 )
            product_id = parse_hex(cur->value);
        else if( strcmp(attr_name, "vendor")==0 )
            vendor_id = parse_hex(cur->value);
        else if( strcmp(attr_name, "subsystem_device")==0 )
            subsys_product_id = parse_hex(cur->value);
        else if( strcmp(attr_name, "subsystem_vendor")==0 )
            subsys_vendor_id = parse_hex(cur->value);
        else if( strcmp(attr_name, "class")==0 )
        {
            dbus_int32_t cls;
            cls = parse_hex(cur->value);
            hal_device_set_property_int(d, "pci.deviceClass", (cls>>16)&0xff);
            hal_device_set_property_int(d, "pci.deviceSubClass",(cls>>8)&0xff);
            hal_device_set_property_int(d, "pci.deviceProtocol", cls&0xff);
        }
    }

    hal_device_set_property_int(d, "pci.idVendor", vendor_id);
    hal_device_set_property_int(d, "pci.idProduct", product_id);
    hal_device_set_property_int(d, "pci.idVendorSubSystem", subsys_vendor_id);
    hal_device_set_property_int(d, "pci.idProductSubSystem",subsys_product_id);

    /* Lookup names in pci.ids */
    pci_ids_find(vendor_id, product_id, subsys_vendor_id, subsys_product_id,
                 &vendor_name, &product_name, 
                 &subsys_vendor_name, &subsys_product_name);
    if( vendor_name!=NULL )
        hal_device_set_property_string(d, "pci.Vendor", vendor_name);
    if( product_name!=NULL )
        hal_device_set_property_string(d, "pci.Product", product_name);
    if( subsys_vendor_name!=NULL )
        hal_device_set_property_string(d, "pci.VendorSubSystem",
                                       subsys_vendor_name);
    if( subsys_product_name!=NULL )
        hal_device_set_property_string(d, "pci.ProductSubSystem",
                                       subsys_product_name);

    /* Provide best-guess of name, goes in Product property; 
     * .fdi files can override this */
    if( product_name!=NULL )
    {
        hal_device_set_property_string(d, "Product", product_name);
    }
    else
    {
        snprintf(namebuf, 512, "Unknown (0x%04x)", product_id);
        hal_device_set_property_string(d, "Product", namebuf);
    }

    /* Provide best-guess of vendor, goes in Vendor property; 
     * .fdi files can override this */
    if( vendor_name!=NULL )
    {
        hal_device_set_property_string(d, "Vendor", vendor_name);
    }
    else
    {
        snprintf(namebuf, 512, "Unknown (0x%04x)", vendor_id);
        hal_device_set_property_string(d, "Vendor", namebuf);
    }

    /* Compute parent */
    parent_udi = find_parent_udi_from_sysfs_path(path, HAL_LINUX_HOTPLUG_TIMEOUT);
    if( parent_udi!=NULL )
        hal_device_set_property_string(d, "Parent", parent_udi);

    /* Compute a proper UDI (unique device id), try to locate a persistent
     * unplugged device or add it
     */
    rename_and_maybe_add(d, pci_compute_udi, "pci");    

}


/** Init function for PCI handling
 *
 */
void hal_pci_init()
{
    /** @todo Hardcoding path to pci.ids is a hack */

    /* Load /usr/share/hwdata/pci.ids */
    pci_ids_load("/usr/share/hwdata/pci.ids");
}

/** Shutdown function for PCI handling
 *
 */
void hal_pci_shutdown()
{
    pci_ids_free();
}

