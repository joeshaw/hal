/***************************************************************************
 * CVSID: $Id$
 *
 * linux_common.c : Common functionality used by Linux specific parts
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
#include <limits.h>

#include <libhal/libhal.h> /* For HAL_STATE_* */

#include "../logger.h"
#include "../device_store.h"
#include "linux_common.h"

/**
 * @defgroup HalDaemonLinuxCommon Utility functions
 * @ingroup HalDaemonLinux
 * @brief Utility functions
 * @{
 */

/** Parse a double represented as a decimal number (base 10) in a string. 
 *
 *  @param  str                 String to parse
 *  @return                     Double; If there was an error parsing the
 *                              result is undefined.
 */
double parse_double(const char* str)
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
dbus_int32_t parse_dec(const char* str)
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
dbus_int32_t parse_hex(const char* str)
{
    dbus_int32_t value;
    value = strtol(str, NULL, 16);
    /** @todo Check error condition */
    return value;
}

/** Find an integer appearing right after a substring in a string.
 *
 *  The result is LONG_MAX if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @param  base                Base, e.g. decimal or hexadecimal, that
 *                              number appears in
 *  @return                     Number
 */
long int find_num(char* pre, char* s, int base)
{
    char* where;
    int result;

    where = strstr(s, pre);
    if( where==NULL )
    {
        /*DIE(("Didn't find '%s' in '%s'", pre, s));*/
        return LONG_MAX;
    }
    where += strlen(pre);

    result = strtol(where, NULL, base);
    /** @todo Handle errors gracefully */
/*
    if( result==LONG_MIN || result==LONG_MAX )
        DIE(("Error parsing value for '%s' in '%s'", pre, s));
*/

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
double find_double(char* pre, char* s)
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
int find_bcd2(char* pre, char* s)
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
char* find_string(char* pre, char* s)
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

/** Read the first line of a file and return it.
 *
 *  @param  filename_format     Name of file, printf-style formatted
 *  @return                     Pointer to string or #NULL if the file could
 *                              not be opened. The result is only valid until
 *                              the next invocation of this function.
 */
char* read_single_line(char* filename_format,...)
{
    FILE* f;
    int i;
    int len;
    char filename[512];
    static char buf[512];
    va_list args;

    va_start(args, filename_format);
    vsnprintf(filename, 512, filename_format, args);
    va_end(args);

    f = fopen(filename, "rb");
    if( f==NULL )
        return NULL;

    if( fgets(buf, 512, f)==NULL )
    {
        fclose(f);
        return NULL;
    }

    len = strlen(buf);
    for(i=len-1; i>0; --i )
    {
        if( buf[i]=='\n' || buf[i]=='\r' )
            buf[i]='\0';
        else
            break;
    }

    fclose(f);
    return buf;
}

/** Given a path, /foo/bar/bat/foobar, return the last element, e.g.
 *  foobar.
 *
 *  @param  path                Path
 *  @return                     Pointer into given string
 */
const char* get_last_element(const char* s)
{
    int len;
    const char* p;
    
    len = strlen(s);
    for(p=s+len-1; p>s; --p)
    {
        if( (*p)=='/' )
            return p+1;
    }

    return s;
}


/** This function takes a temporary device and renames it to a proper
 *  UDI using the supplied bus-specific #naming_func. After renaming
 *  the HAL daemon will locate a .fdi file and possibly boot the
 *  device (pending RequireEnable property).
 *
 *  This function handles the fact that identical devices (for
 *  instance two completely identical USB mice) gets their own unique
 *  device id by appending a trailing number after it.
 *
 *  @param  d                   HalDevice object
 *  @param  naming_func         Function to compute bus-specific UDI
 *  @param  namespace           Namespace of properties that must match,
 *                              e.g. "usb", "pci", in order to have matched
 *                              a device
 *  @return                     New non-temporary UDI for the device
 *                              or #NULL if the device already existed.
 *                              In the event that the device already existed
 *                              the given HalDevice object is destroyed
 */
char* rename_and_maybe_add(HalDevice* d, 
                           ComputeFDI naming_func,
                           const char* namespace)
{
    int append_num;
    char* computed_udi;
    HalDevice* computed_d;

    /* udi is a temporary udi */

    append_num = -1;
tryagain:
    /* compute the udi for the device */
    computed_udi = (*naming_func)(d, append_num);

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
    computed_d = ds_device_find(computed_udi);
    if( computed_d!=NULL )
    {
        
        if( ds_property_get_int(computed_d, "State")!=
            42 /** @todo: FIXME HAL_STATE_UNPLUGGED*/ )
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
            if( ds_device_matches(computed_d, d, namespace) )
            {
                fprintf(stderr, "Found device already present as '%s'!\n",
                        computed_udi);
                ds_print(d);
                ds_print(computed_d);
                /* indeed a match, must be b) ; ignore device */
                ds_device_destroy(d);
                /* and return */
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
        ds_device_merge(computed_d, d);
        
        /* Remove temporary device */
        ds_device_destroy(d);
        
        /* Set that the device is in the disabled state... */
        /*ds_property_set_int(computed_d, "State", HAL_STATE_DISABLED);*/
        
        /* Now try to enable the device.. */
        if( ds_property_exists(computed_d, "RequireEnable") &&
            !ds_property_get_bool(computed_d, "RequireEnable") )
        {
            /*HAL_INFO(("TODO: enable %s!", computed_udi));*/
            /*hal_device_enable(computed_udi);*/
        }
    }
    else
    {
        /* Device is not in list... */
        
        /* Set required parameters */
        ds_property_set_bool(d, "GotDeviceInfo", FALSE);
        /*ds_property_set_int(d, "State", HAL_STATE_NEED_DEVICE_INFO);*/
        
        /* commit the device to the Global Device List - give the
         * computed device name */
        ds_device_set_udi(d, computed_udi);
        ds_gdl_add(d);

        /* Now try to enable the device - if a .fdi is found 
         * and merged, the HAL daemon will know to respect the
         * RequireEnable property */
        /*HAL_INFO(("TODO: enable %s!", computed_udi));*/
        /*hal_device_enable(computed_udi);*/

    }

    return computed_udi;
}

/** Given a sysfs-path for a device, this functions finds the sysfs
 *  path representing the parent of the given device by truncation.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @return                     Path for parent; must be freed by caller
 */
char* get_parent_sysfs_path(const char* path)
{
    int i;
    int len;
    char* parent_path;

    /* Find parent device by truncating our own path */
    parent_path = strndup(path, SYSFS_PATH_MAX);
    if( parent_path==NULL )
        DIE(("No memory"));
    len = strlen(parent_path);
    for(i=len-1; parent_path[i]!='/'; --i)
    {
        parent_path[i]='\0';
    }
    parent_path[i]='\0';

    return parent_path;
}

/** Set the physical device for a device.
 *
 *  This function visits all parent devices and sets the property
 *  PhysicalDevice to the first parent device that doesn't have the
 *  PhysicalDevice property set.
 *
 *  @param  device              HalDevice to process
 */
void find_and_set_physical_device(HalDevice* device)
{
    HalDevice* d;
    HalDevice* parent;
    const char* parent_udi;

    d = device;
    do
    {
        parent_udi = ds_property_get_string(d, "Parent");
        if( parent_udi==NULL )
        {
            HAL_ERROR(("Error finding parent for %s\n", d->udi));
            return;
        }

        parent = ds_device_find(parent_udi);
        if( parent==NULL )
        {
            HAL_ERROR(("Error resolving UDI %s\n", parent_udi));
            return;
        }

        if( !ds_property_exists(parent, "PhysicalDevice") )
        {
            ds_property_set_string(device, "PhysicalDevice", parent_udi);
            return;
        }

        d = parent;
    } 
    while(TRUE);
}


/* Entry in bandaid driver database */
struct driver_entry_s
{
    char driver_name[SYSFS_NAME_LEN];  /**< Name of driver, e.g. 8139too */
    char device_path[SYSFS_PATH_MAX];  /**< Sysfs path */
    struct driver_entry_s* next;       /**< Pointer to next element or #NULL
                                        *   if the last element */
};

/** Head of linked list of #driver_entry_s structs */
static struct driver_entry_s* drivers_table_head = NULL;

/** Add an entry to the bandaid driver database.
 *
 *  @param  driver_name         Name of the driver
 *  @param  device_path         Path to device, must start with /sys/devices
 */
static void drivers_add_entry(const char* driver_name, const char* device_path)
{
    struct driver_entry_s* entry;

    entry = malloc(sizeof(struct driver_entry_s));
    if( entry==NULL )
        DIE(("Out of memory"));
    strncpy(entry->driver_name, driver_name, SYSFS_NAME_LEN);
    strncpy(entry->device_path, device_path, SYSFS_PATH_MAX);
    entry->next = drivers_table_head;
    drivers_table_head = entry;
}

/** Given a device path under /sys/devices, lookup the driver. You need
 *  to have called #drivers_collect() on the bus-type before hand.
 *
 *  @param  device_path         sysfs path to device
 *  @return                     Driver name or #NULL if no driver is bound
 *                              to that sysfs device
 */
const char* drivers_lookup(const char* device_path)
{
    struct driver_entry_s* i;

    for(i=drivers_table_head; i!=NULL; i=i->next)
    {
        if( strcmp(device_path, i->device_path)==0 )
            return i->driver_name;
    }
    return NULL;
}

/** Collect all drivers being used on a bus. This is only bandaid until
 *  sysutils fill in the driver_name in sysfs_device.
 *
 *  @param  bus_name            Name of bus, e.g. pci, usb
 */
void drivers_collect(const char* bus_name)
{
    char path[SYSFS_PATH_MAX];
    struct sysfs_directory* current;
    struct sysfs_link* current2;
    struct sysfs_directory* dir;
    struct sysfs_directory* dir2;

    /* traverse /sys/bus/<bus>/drivers */
    snprintf(path, SYSFS_PATH_MAX, "%s/bus/%s/drivers", sysfs_mount_path, bus_name);
    dir = sysfs_open_directory(path);
    if( dir==NULL )
        DIE(("Error opening sysfs directory at %s\n", path));
    if( sysfs_read_directory(dir)!=0 )
        DIE(("Error reading sysfs directory at %s\n", path));
    if( dir->subdirs!=NULL )
    {
        dlist_for_each_data(dir->subdirs, current, struct sysfs_directory)
        {
            /*printf("name=%s\n", current->name);*/

            dir2 = sysfs_open_directory(current->path);
            if( dir2==NULL )
                DIE(("Error opening sysfs directory at %s\n", current->path));
            if( sysfs_read_directory(dir2)!=0 )
                DIE(("Error reading sysfs directory at %s\n", current->path));

            if( dir2->links!=NULL )
            {
                dlist_for_each_data(dir2->links, current2, 
                                    struct sysfs_link)
                {
                    /*printf("  link=%s\n", current2->target);*/
                    drivers_add_entry(current->name, current2->target);
                }
                sysfs_close_directory(dir2);
            }
        }
    }
    sysfs_close_directory(dir);
}

/** @} */
