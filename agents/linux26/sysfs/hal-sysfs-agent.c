/***************************************************************************
 * CVSID: $Id$
 *
 * hal-sysfs-agent.c : Agent scanning sysfs for HAL on Linux 2.6
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
#include <syslog.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <libsysfs.h>

#include <libhal/libhal.h>


/** @defgroup HalAgentsLinux26 Linux 2.6 sysfs
 *  @ingroup  HalAgentsLinux
 *  @brief    HAL agent using sysfs on Linux 2.6 kernels
 *
 *  @{
 */


/** Macro to abort the program.
 *
 *  @param  expr                Format line and arguments
 */
#define DIE(expr) do {printf("*** [DIE] %s:%s():%d : ", __FILE__, __FUNCTION__, __LINE__); printf expr; printf("\n"); exit(1); } while(0)



/** Memory allocation; aborts if no memory.
 *
 *  @param  how_much            Number of bytes to allocated
 *  @return                     Pointer to allocated storage
 */
/*
static void* xmalloc(unsigned int how_much)
{
    void* p = malloc(how_much);
    if( !p )
        DIE(("Unable to allocate %d bytes of memory", how_much));
    return p;
}
*/


/** Global variable that is #TRUE if, and only if, we are invoked to probe
 *  all the devices. Hence it is #FALSE if, and only if, we are invoked
 *  by linux-hotplug
 */
static dbus_bool_t is_probing = FALSE;

/** Parse a double represented as a decimal number (base 10) in a string. 
 *
 *  @param  str                 String to parse
 *  @return                     Double; If there was an error parsing the
 *                              result is undefined.
 */
static double parse_double(const char* str)
{
    /** @todo Check error condition */
    return atof(str);
}

/** Parse an integer represented as a decimal number (base 10) in a string. 
 *
 *  @param  str                 String to parse
 *  @return                     Integer; If there was an error parsing the
 *                              result is undefined.
 */
static dbus_int32_t parse_dec(const char* str)
{
    dbus_int32_t value;
    value = strtol(str, NULL, 10);
    /** @todo Check error condition */
    return value;
}

/** Parse an integer represented as a hexa-decimal number (base 16) in
 *  a string.
 *
 *  @param  str                 String to parse
 *  @return                     Integer; If there was an error parsing the
 *                              result is undefined.
 */
static dbus_int32_t parse_hex(const char* str)
{
    dbus_int32_t value;
    value = strtol(str, NULL, 16);
    /** @todo Check error condition */
    return value;
}

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

/** Find the name corresponding to a PCI vendor id.
 *
 *  @param  vendor_id           PCI vendor id
 *  @return                     The name as a string or #NULL if the name
 *                              couldn't be found
 */
static char* pci_ids_find_vendor(int vendor_id)
{
    char* line;
    unsigned int i;
    unsigned int line_len;
    char rep[8];

    snprintf(rep, 8, "%04x", vendor_id);
    printf(" vendor_id=%x => '%s'\n", vendor_id, rep);

    for(pci_ids_line_iter_init(); pci_ids_line_iter_has_more(); )
    {
        line = pci_ids_line_iter_get_line(&line_len);

        if( line_len>=4 )
        {
            /* fast way to compare four bytes */
            if( (*((dbus_uint32_t*)line))==(*((dbus_uint32_t*)rep)) )
            {
                /* found it */
                for(i=4; i<line_len; i++)
                {
                    if( !isspace(line[i]) )
                        break;
                }
                return line+i;
            }

        }
    }

    return NULL;
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

/** Find the name corresponding to a USB vendor id.
 *
 *  @param  vendor_id           USB vendor id
 *  @return                     The name as a string or #NULL if the name
 *                              couldn't be found
 */
static char* usb_ids_find_vendor(int vendor_id)
{
    char* line;
    unsigned int i;
    unsigned int line_len;
    char rep[8];

    snprintf(rep, 8, "%04x", vendor_id);
    printf(" vendor_id=%x => '%s'\n", vendor_id, rep);

    for(usb_ids_line_iter_init(); usb_ids_line_iter_has_more(); )
    {
        line = usb_ids_line_iter_get_line(&line_len);

        if( line_len>=4 )
        {
            /* fast way to compare four bytes */
            if( (*((dbus_uint32_t*)line))==(*((dbus_uint32_t*)rep)) )
            {
                /* found it */
                for(i=4; i<line_len; i++)
                {
                    if( !isspace(line[i]) )
                        break;
                }
                return line+i;
            }

        }
    }

    return NULL;
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



/** Find an integer appearing right after a substring in a string.
 *
 *  The result is undefined if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @param  base                Base, e.g. decimal or hexadecimal, that
 *                              number appears in
 *  @return                     Number
 */
static long int find_num(char* pre, char* s, int base)
{
    char* where;
    int result;

    where = strstr(s, pre);
    if( where==NULL )
        DIE(("Didn't find '%s' in '%s'", pre, s));
    where += strlen(pre);

    result = strtol(where, NULL, base);
    /** @todo Handle errors gracefully */
    if( result==LONG_MIN || result==LONG_MAX )
        DIE(("Error parsing value for '%s' in '%s'", pre, s));

    return result;
}

/** Find a floating point number appearing right after a substring in a string
 *  and return it as a double precision IEEE754 floating point number.
 *
 *  The result is undefined if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @return                     Number
 */
static double find_double(char* pre, char* s)
{
    char* where;
    double result;

    where = strstr(s, pre);
    /** @todo Handle errors gracefully */
    if( where==NULL )
        DIE(("Didn't find '%s' in '%s'", pre, s));
    where += strlen(pre);

    result = atof(where);

    return result;
}

/** Find a floating point number appearing right after a substring in a string
 *  and return it as a BCD encoded number with 2 digits of precision.
 *
 *  The result is undefined if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @return                     Number
 */
static int find_bcd2(char* pre, char* s)
{
    int i;
    char c;
    int digit;
    int left, right, result;
    int len;
    char* str;
    dbus_bool_t passed_white_space;
    int num_prec;

    str = find_string(pre, s);
    if( str==NULL || strlen(str)==0 )
        return 0xffff;


    left = 0;
    len = strlen(str);
    passed_white_space = FALSE;
    for(i=0; i<len && str[i]!='.'; i++)
    {
        if( isspace(str[i]) )
        {
            if( passed_white_space )
                break;
            else
                continue;
        }
        passed_white_space = TRUE;
        left *= 16;
        c = str[i];
        digit = (int) (c-'0');
        left += digit;
    }
    i++;
    right=0;
    num_prec=0;
    for( ; i<len; i++)
    {
        if( isspace(str[i]) )
            break;
        if( num_prec==2 )       /* Only care about 2 digits of precision */
            break;
        right *= 16;
        c = str[i];
        digit = (int) (c-'0');
        right += digit;
        num_prec++;
    }

    for( ; num_prec<2; num_prec++)
        right*=16;
    
    result = left*256 + (right&255);
    return result;
}

/** Find a string appearing right after a substring in a string
 *  and return it. The string return is statically allocated and is 
 *  only valid until the next invocation of this function.
 *
 *  The result is undefined if the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @return                     Number
 */
static char* find_string(char* pre, char* s)
{
    char* where;
    static char buf[256];
    char* p;

    where = strstr(s, pre);
    if( where==NULL )
        return NULL;
    where += strlen(pre);

    p=buf;
    while( *where!='\n' && *where!='\r')
    {
        char c = *where;

        // ignoring char 63 fixes a problem with info from the Lexar CF Reader
        if( (isalnum(c) || isspace(c) || ispunct(c)) && c!=63 )
        {
            *p = c;
            ++p;
        }

        ++where;
    }
    *p = '\0';

    // remove trailing white space
    --p;
    while( isspace(*p) )
    {
        *p='\0';
        --p;
    }

    return buf;
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
    unsigned int i;
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

    serial = hal_device_get_property_string(udi, "usb.serial");
    if( serial==NULL )
    serial = "noserial";

    snprintf(buf, 256, format, 
             hal_device_get_property_int(udi, "usb.idVendor"),
             hal_device_get_property_int(udi, "usb.idProduct"),
             hal_device_get_property_int(udi, "usb.bcdDevice"),
             hal_device_get_property_int(udi, "usb.configurationValue"),
             serial, append_num);
    
    return buf;
}

/** Type for function to compute the UDI (unique device id) for a given
 *  HAL device.
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
typedef char* (*ComputeFDI)(const char* udi, int append_num);


/** This function takes a temporary device and renames it to a proper
 *  UDI using the supplied bus-specific #naming_func. After renaming
 *  the HAL daemon will locate a .fdi file and possibly boot the
 *  device (pending RequireEnable property).
 *
 *  This function handles the fact that identical devices (for
 *  instance two completely identical USB mice) gets their own unique
 *  device id by appending a trailing number after it.
 *
 *  @param  udi                 UDI (unique device id) of temporary device
 *  @param  naming_func         Function to compute bus-specific UDI
 *  @param  namespace           Namespace of properties that must match,
 *                              e.g. "usb", "pci", in order to have matched
 *                              a device
 *  @return                     New non-temporary UDI for the device
 *                              or #NULL if the device already existed.
 */
static char* rename_and_maybe_add(const char* udi, 
                                  ComputeFDI naming_func,
                                  const char* namespace)
{
    int append_num;
    char* computed_udi;

    /* udi is a temporary udi */

    append_num = -1;
tryagain:
    /* compute the udi for the device */
    computed_udi = (*naming_func)(udi, append_num);

    /* See if a device with the computed udi already exist. It can exist
     * because the device-list is (can be) persistent across invocations 
     * of hald.
     *
     * If it does exist, note that it's udi is computed from only the same 
     * information as our just computed udi.. So if we match, and it's
     * unplugged, it's the same device!
     *
     * (of course assuming that our udi computing algorithm actually works!
     *  Which it might not, see discussions - but we can get close enough
     *  for it to be practical)
     */
    if( hal_device_exists(computed_udi) )
    {
        
        if( hal_device_get_property_int(computed_udi, "State")!=
            HAL_STATE_UNPLUGGED )
        {
            /* Danger, Will Robinson! Danger!
             *
             * Ok, this means that either
             *
             * a) The user plugged in two instances of the kind of
             *    of device; or
             *
             * b) The agent is invoked with --probe for the second
             *    time during the life of the HAL daemon
             *
             * We want to support b) otherwise we end up adding a lot
             * of devices which is a nuisance.. We also want to be able
             * to do b) when developing HAL agents.
             *
             * So, therefore we check if the non-unplugged device has 
             * the same bus information as our newly hotplugged one.
             */
            if( hal_agent_device_matches(computed_udi, udi, namespace) )
            {
                fprintf(stderr, "Found device already present as '%s'!\n",
                        computed_udi);
                hal_device_print(udi);
                hal_device_print(computed_udi);
                /* indeed a match, must be b) ; ignore device */
                hal_agent_remove_device(udi);
                /* and continue processing the next device */
                return NULL;
            }
            
            /** Not a match, must be case a). Choose next computed_udi
             *  and try again! */
            append_num++; 
            goto tryagain;
        }
        
        /* It did exist! Merge our properties from the probed device
         * since some of the bus-specific properties are likely to be
         * different 
         *
         * (user may have changed port, #Dev is different etc.)
         *
         * Note that the probed device only contain bus-specific
         * properties - the other properties will remain..
         */
        hal_agent_merge_properties(computed_udi, udi);
        
        /* Remove temporary device */
        hal_agent_remove_device(udi);
        
        /* Set that the device is in the disabled state... */
        hal_device_set_property_int(computed_udi, "State", 
                                    HAL_STATE_DISABLED);
        
        /* Now try to enable the device.. */
        if( hal_device_property_exists(computed_udi, "RequireEnable") &&
            !hal_device_get_property_bool(computed_udi, "RequireEnable") )
        {
            hal_device_enable(computed_udi);
        }
    }
    else
    {
        /* Device is not in list... */
        
        /* Set required parameters */
        hal_device_set_property_bool(udi, "GotDeviceInfo", FALSE);
        hal_device_set_property_int(udi, "State", 
                                    HAL_STATE_NEED_DEVICE_INFO);
        
        /* commit the device to the Global Device List - give the
         * computed device name */
        hal_agent_commit_to_gdl(udi, computed_udi);
        
        /* Now try to enable the device - if a .fdi is found 
         * and merged, the HAL daemon will know to respect the
         * RequireEnable property */
        hal_device_enable(computed_udi);

    }

    return computed_udi;
}

/** Given a sysfs-path for a device, this functions finds the HAL device
 *  representing the parent of the given device. There may not be a parent
 *  device, in which case this function returns #NULL.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @return                     UDI (unique device id) of parent, or #NULL
 *                              if no parent device was found.
 */
static char* find_parent_udi_from_sysfs_path(const char* path)
{
    int i;
    int len;
    char** parent_udis;
    int num_parent_udi;
    char parent_path[SYSFS_PATH_MAX];
    int time_to_sleep = 100*1000;

    /* Find parent device by truncating our own path */
    strncpy(parent_path, path, SYSFS_PATH_MAX);
    len = strlen(parent_path);
    for(i=len-1; parent_path[i]!='/'; --i)
    {
        parent_path[i]='\0';
    }
    parent_path[i]='\0';
    
    /*printf("*** found parent=%s\n", parent_path);*/

    while( time_to_sleep<2*1000*1000 ) // means we'll only sleep max 4 seconds
    {    
        /* Now find corresponding HAL device */
        parent_udis = hal_manager_find_device_string_match("Linux.sysfs_path",
                                                       parent_path,
                                                       &num_parent_udi);
    
        /** @todo fix memory leak */

        if( num_parent_udi!=1 || parent_udis==NULL )
        {
            /* no parent, or multiple parents; the latter doesn't make sense 
             *
             * Hmm.. one good example is plugging in the IBM USB Preferred
             * Keyboard which appear as two USB devices; one hub and one
             * HID device which is a child of the hub..
             *
             * Well, the Linux kernel invokes /sbin/hotplug for these two
             * devices at the same time so two instances of this program
             * is running which means that the invocation for the HID
             * device might get to *this* point before the invocation for
             * the hub have called #hal_manager_commit_to_gdl().. 
             *
             * Fix: sleep for a while then try again. Don't blame me, blame
             * the Linux kernel :-)
             */

            /* Don't do this if we are probing devices; it'd be a waste of 
             * time*/
            if( is_probing )
                return NULL;

            usleep(time_to_sleep); 
            time_to_sleep*=2;
        }
        else
        {
            return parent_udis[0];
        }
    }

    return NULL;
}



/** Given a sysfs-path for a device, this functions finds the HAL device
 *  representing the given device. 
 *
 *  @param  path                Sysfs-path of device to find UDI for
 *  @return                     UDI (unique device id) of device, or #NULL
 *                              if no device was found.
 */
static char* find_udi_from_sysfs_path(const char* path)
{
    int i;
    int len;
    char** udis;
    int num_udi;


    udis = hal_manager_find_device_string_match("Linux.sysfs_path",
                                                path,
                                                &num_udi);
    
    if( num_udi==1 && udis!=NULL )
    {
        return udis[0];
    }

    /** @todo Log the error */

    return NULL;
}

/** Given a sysfs-path for an USB interface, this function finds the
 *  HAL device representing the USB device. This function may sleep
 *  for a while because we may race against the /sbin/hotplug invoked
 *  event for the USB device.
 *
 *  @param  path                Sysfs-path of USB interface
 *  @return                     UDI (unique device id) of USB device
 */
static char* find_usb_device_from_interface_sysfs_path(const char* path)
{
    int i;
    int len;
    char* d;
    char parent_path[SYSFS_PATH_MAX+1];
    int time_to_sleep = 500*1000;
    dbus_bool_t have_slept;

    /* First find parent device; that's easy; just remove chars from end
     * until and including first '/' */
    strncpy(parent_path, path, SYSFS_PATH_MAX);
    len = strlen(parent_path);
    for(i=len-1; i>0; --i)
    {
        if( parent_path[i]=='/' )
            break;
        parent_path[i]='\0';
    }
    parent_path[i]='\0';
    //printf("parent_path = '%s'\n", parent_path);
    /* Lookup HAL device from parent_path */

    have_slept = FALSE;

    while( time_to_sleep<5*1000*1000 ) /* we'll sleep a max of 10 seconds */
    {
        d = find_udi_from_sysfs_path(parent_path);
        if( d!=NULL )
        {
            if( have_slept )
                syslog(LOG_NOTICE, "Found USB device for USB interface %s", 
                       path);
            return d;
        }

        syslog(LOG_NOTICE, "Cannot find USB device for USB interface %s! "
               "Sleeping %d ms...", path, time_to_sleep/1000);

        usleep(time_to_sleep); 
        have_slept = TRUE;
        time_to_sleep*=2;
    }

    syslog(LOG_CRIT, "Couldn't find USB device for USB interface %s! "
           "Giving up", path);
    DIE(("Giving up"));

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
    int in_num;
    int conf_num;
    struct sysfs_attribute* cur;
    char* d;
    char buf[256];
    char attr_name[SYSFS_NAME_LEN];

    //printf("usb_interface: path=%s\n", path);

    d = find_usb_device_from_interface_sysfs_path(path);

    if( device->directory==NULL || device->directory->attributes==NULL )
        return;

    /** @todo What about multiple configurations? Must acquire an USB
     *        device that does this :-) Right now assume configuration 1
     */
    conf_num = 1;

    /* first, find interface number */
    in_num = -1;
    dlist_for_each_data(sysfs_get_device_attributes(device), cur,
                        struct sysfs_attribute)
    {
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        if( strcmp(attr_name, "bInterfaceNumber")==0 )
        {
            in_num = parse_dec(cur->value);
        }
    }
    if( in_num==-1 )
        return;

    /*printf("conf_num=%d, in_num=%d\n", conf_num, in_num);*/

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
        {
            snprintf(buf, 256, "usb.%d.%d.bInterfaceClass", conf_num, in_num);
            hal_device_set_property_int(d, buf, parse_dec(cur->value));
            if( conf_num==1 && in_num==0 )
            {
                hal_device_set_property_int(d, "usb.bInterfaceClass", 
                                            parse_dec(cur->value));
            }
        }
        else if( strcmp(attr_name, "bInterfaceSubClass")==0 )
        {
            snprintf(buf, 256, "usb.%d.%d.bInterfaceSubClass",conf_num,in_num);
            hal_device_set_property_int(d, buf, parse_dec(cur->value));
            if( conf_num==1 && in_num==0 )
            {
                hal_device_set_property_int(d, "usb.bInterfaceSubClass",
                                            parse_dec(cur->value));
            }
        }
        else if( strcmp(attr_name, "bInterfaceProtocol")==0 )
        {
            snprintf(buf, 256, "usb.%d.%d.bInterfaceProtocol",conf_num,in_num);
            hal_device_set_property_int(d, buf, parse_dec(cur->value));
            if( conf_num==1 && in_num==0 )
            {
                hal_device_set_property_int(d, "usb.bInterfaceProtocol", 
                                            parse_dec(cur->value));
            }
        }
    }
}

/** Visitor function for USB device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
static void visit_device_usb(const char* path, struct sysfs_device *device)
{
    int i;
    int len;
    dbus_bool_t is_interface;
    struct sysfs_attribute* cur;
    struct sysfs_device* in;
    const char* d;
    const char* parent_udi;
    char attr_name[SYSFS_NAME_LEN];
    char in_path[SYSFS_PATH_MAX];
    int vendor_id=0;
    int product_id=0;
    char* vendor_name;
    char* product_name;
    char* vendor_name_kernel;
    char* product_name_kernel;
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
    /** @note We also set the path here, because otherwise we can't handle two
     *  identical devices per the algorithm used in a #rename_and_maybe_add()
     *  The point is that we need something unique in the Bus namespace
     */
    hal_device_set_property_string(d, "usb.linux.sysfs_path", path);
    /*printf("*** created udi=%s for path=%s\n", d, path);*/
    
    dlist_for_each_data(sysfs_get_device_attributes(device), cur,
                        struct sysfs_attribute)
    {
        
        if( sysfs_get_name_from_path(cur->path, 
                                     attr_name, SYSFS_NAME_LEN) != 0 )
            continue;

        /* strip whitespace */
        len = strlen(cur->value);
        for(i=len-1; isspace(cur->value[i]) && i>=0; --i)
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
    parent_udi = find_parent_udi_from_sysfs_path(path);
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
static void visit_device_pci(const char* path, struct sysfs_device *device)
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
    parent_udi = find_parent_udi_from_sysfs_path(path);
    if( parent_udi!=NULL )
        hal_device_set_property_string(d, "Parent", parent_udi);

    /* Compute a proper UDI (unique device id), try to locate a persistent
     * unplugged device or add it
     */
    rename_and_maybe_add(d, pci_compute_udi, "pci");    

}

/** Visitor function for IDE device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
static void visit_device_ide(const char* path, struct sysfs_device *device)
{
    printf("ide: %s\n", path);
}

/** Visitor function for SCSI device.
 *
 *  This function parses the attributes present and creates a new HAL
 *  device based on this information.
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
static void visit_device_scsi(const char* path, struct sysfs_device *device)
{
    printf("scsi: %s\n", path);
}

/** Visitor function for any device.
 *
 *  This function determines the bus-type of the device and call the
 *  appropriate visit_device_<bustype> function if matched.
 *
 *  @param  path                Sysfs-path for device
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void visit_device(const char* path, dbus_bool_t visit_children)
{
    struct sysfs_device* device;
    struct sysfs_directory* subdir;

    device = sysfs_open_device(path);
    if( device==NULL )
        DIE(("Coulnd't get sysfs device object for path %s", path));

    //printf("bus=%s driver=%s device=%s\n", device->bus, device->driver_name, path);

    if( device->bus!=NULL )
    {
        if( strcmp(device->bus, "usb")==0 )
            visit_device_usb(path, device);
        else if( strcmp(device->bus, "pci")==0 )
            visit_device_pci(path, device);
        else if( strcmp(device->bus, "ide")==0 )
            visit_device_ide(path, device);
        else if( strcmp(device->bus, "scsi")==0 )
            visit_device_scsi(path, device);
    }

    /* Visit children */
    if( visit_children && device->directory->subdirs!=NULL )
    {
        dlist_for_each_data(device->directory->subdirs, subdir, 
                            struct sysfs_directory)
        {
            char newpath[SYSFS_PATH_MAX];
            snprintf(newpath, SYSFS_PATH_MAX, "%s/%s", path, subdir->name);
            visit_device(newpath, TRUE);
        }
    }

    sysfs_close_device(device);
}

/** This function is called when the program is invoked to probe sysfs.
 */
static void hal_sysfs_probe()
{
    int rc;
    char path[SYSFS_PATH_MAX];
    char sysfs_path[SYSFS_PATH_MAX];
    struct sysfs_directory* current;
    struct sysfs_directory* dir;

    is_probing = TRUE;

    /* get mount path */
    rc = sysfs_get_mnt_path(sysfs_path, SYSFS_PATH_MAX);
    if( rc!=0 )
        DIE(("Couldn't get mount path for sysfs"));

    /* traverse each root device */
    snprintf(path, SYSFS_PATH_MAX, "%s%s", sysfs_path, SYSFS_DEVICES_DIR);
    dir = sysfs_open_directory(path);
    if( dir==NULL )
    {
        DIE(("Error opening sysfs directory at %s\n", path));
    }
    if( sysfs_read_directory(dir)!=0 )
    {
        DIE(("Error reading sysfs directory at %s\n", path));
    }


    if( dir->subdirs!=NULL )
    {
        dlist_for_each_data(dir->subdirs, current, struct sysfs_directory)
        {
            visit_device(current->path, TRUE);
        }
    }
    sysfs_close_directory(dir);
}

/** This function is invoked on hotplug add events */
static void device_hotplug_add(char* bus)
{
    int rc;
    const char* devpath;
    const char* interface;
    char path[SYSFS_PATH_MAX];
    char sysfs_path[SYSFS_PATH_MAX];
    struct sysfs_device* device;

    /* Must have $DEVPATH */
    devpath = getenv("DEVPATH");
    if( devpath==NULL )
        return;

    /* get mount path for sysfs */
    rc = sysfs_get_mnt_path(sysfs_path, SYSFS_PATH_MAX);
    if( rc!=0 )
        DIE(("Couldn't get mount path for sysfs"));

    snprintf(path, SYSFS_PATH_MAX, "%s%s", sysfs_path, devpath);

    if( strcmp(bus, "usb")==0 )
    {
        if( getenv("INTERFACE")!=NULL )
            syslog(LOG_INFO, "adding usb interface at %s", path);
        else
            syslog(LOG_INFO, "adding usb device at %s", path);
    }
    else if( strcmp(bus, "pci")==0 )
    {
        syslog(LOG_INFO, "adding pci device at %s", path);
    }
    else
    {
        syslog(LOG_INFO, "ignoring device add of bus=%s at %s", bus, path);
        return;
    }

    visit_device(path, FALSE);
}

/** This function is invoked on hotplug remove events */
static void device_hotplug_remove(char* bus)
{
    int rc;
    const char* devpath;
    const char* device_udi;
    char path[SYSFS_PATH_MAX];
    char sysfs_path[SYSFS_PATH_MAX];
    char** device_udis;
    int num_device_udis;

    /* Must have $DEVPATH */
    devpath = getenv("DEVPATH");
    if( devpath==NULL )
        return;

    /* get mount path for sysfs */
    rc = sysfs_get_mnt_path(sysfs_path, SYSFS_PATH_MAX);
    if( rc!=0 )
    {
        DIE(("Couldn't get mount path for sysfs"));
    }

    snprintf(path, SYSFS_PATH_MAX, "%s%s", sysfs_path, devpath);

    if( strcmp(bus, "usb")==0 )
    {
        if( getenv("INTERFACE")!=NULL )
            syslog(LOG_INFO, "removing usb interface at %s", path);
        else
            syslog(LOG_INFO, "removing usb device at %s", path);
    }
    else if( strcmp(bus, "pci")==0 )
    {
        syslog(LOG_INFO, "removing pci device at %s", path);
    }
    else
    {
        syslog(LOG_INFO, "ignoring device removal of bus=%s at %s", bus, path);
        return;
    }

    /* Now find corresponding HAL device */
    device_udis = hal_manager_find_device_string_match("Linux.sysfs_path",
                                                       path,
                                                       &num_device_udis);
    if( device_udis==NULL || num_device_udis!=1 )
    {
        //syslog(LOG_ERR, "device_hotplug_remove(), Could not find UDI!");
        syslog(LOG_ERR, "couldn't find device UDI %s", device_udi);
    }
    else
    {
        device_udi = device_udis[0];
        //syslog(LOG_INFO, "device_hotplug_remove(), device_udi=%s", device_udi);
        syslog(LOG_INFO, "device UDI %s", device_udi);
        hal_agent_remove_device(device_udi);
    }
}

/** This function is invoked on hotplug events */
static void device_hotplug(char* bus)
{
    const char* action;

    action = getenv("ACTION");
    if( action==NULL )
        return;

    if( strcmp(action, "add")==0 )
        device_hotplug_add(bus);
    else if( strcmp(action, "remove")==0 )
        device_hotplug_remove(bus);

}


/** D-BUS mainloop integration for libhal.
 *
 *  @param  dbus_connection     D-BUS connection to integrate
 */
static void mainloop_integration(DBusConnection* dbus_connection)
{
    dbus_connection_setup_with_g_main(dbus_connection, NULL);
}

/** Usage */
static void usage()
{
    fprintf(stderr, 
"\n"
"usage : hal-sysfs-agent [--help] [--probe]\n"
"\n"
"        --probe          Probe devices present in sysfs\n"
"        --help           Show this information and exit\n"
"\n"
"This program is supposed to only be invoked from the linux-hotplug package\n"
"or from the HAL daemon hald.\n"
"\n");
}

/** Entry point for USB agent for HAL on GNU/Linux
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @return                     Exit code
 */
int main(int argc, char* argv[])
{
    //GMainLoop* loop;
    LibHalFunctions hal_functions = {mainloop_integration,
                                     NULL /*property_changed*/,
                                     NULL /*device_added*/, 
                                     NULL /*device_remove*/, 
                                     NULL /*device_booting*/,
                                     NULL /*device_shutting_down*/,
                                     NULL /*device_disabled*/,
                                     NULL /*device_need_device_info*/,
                                     NULL /*device_boot_error*/,
                                     NULL /*device_enabled*/,
                                     NULL /*device_req_user*/ };

    fprintf(stderr, "hal-sysfs-agent " PACKAGE_VERSION "\r\n");

    if( hal_initialize(&hal_functions)  )
    {
        fprintf(stderr, "error: hal_initialize failed\r\n");
        exit(1);
    }

    openlog("hal-sysfs-agent", LOG_CONS|LOG_PID, LOG_DAEMON);

    pci_ids_load("/usr/share/hwdata/pci.ids");
    usb_ids_load("/usr/share/hwdata/usb.ids");

    /* Parse /proc/bus/usb/devices */
    usb_proc_parse();

    while(TRUE)
    {
        int c;
        int option_index = 0;
        const char* opt;
        static struct option long_options[] = 
        {
            {"help", 0, NULL, 0},
            {"probe", 0, NULL, 0},
            {NULL, 0, NULL, 0}
        };

        c = getopt_long(argc, argv, "",
                        long_options, &option_index);
        if (c == -1)
            break;
        
        switch(c)
        {
        case 0:
            opt = long_options[option_index].name;

            if( strcmp(opt, "help")==0 )
            {
                usage();
                exit(0);
            }
            else if( strcmp(opt, "probe")==0 )
            {
                hal_sysfs_probe();

                sleep(1);
                return 0;
            }

        default:
            usage();
            exit(1);
            break;
        }         
    }

    if( argc==2 )
    {
        device_hotplug(argv[1]);
        sleep(1);
        return 0;
    }

    usage();

    return 0;
}

/** @} */
