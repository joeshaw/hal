/***************************************************************************
 * CVSID: $Id$
 *
 * main.c : main() and common functions for sysfs-agent on Linux 2.6
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

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <libsysfs.h>

#include <libhal/libhal.h>

#include "main.h"
#include "hal_usb.h"
#include "hal_pci.h"
#include "hal_ide.h"
#include "hal_scsi.h"
#include "hal_block.h"
#include "hal_net.h"
#include "hal_input.h"

#include "hal_monitor.h"

/** @defgroup HalAgentsLinux26 Linux 2.6 sysfs
 *  @ingroup  HalAgentsLinux
 *  @brief    HAL agent using sysfs on Linux 2.6 kernels
 *
 *  @{
 */

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
dbus_bool_t is_probing = FALSE;

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
char* rename_and_maybe_add(const char* udi, 
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
 *  representing the parent of the given device by truncating the sysfs
 *  path. There may not be a parent device, in which case this function
 *  returns #NULL.
 *
 *  Optionally, the caller may specify for many how seconds to try. This is
 *  useful for hotplug situations where the many hotplug events for a
 *  usb-storage device are not in order. Remember, in this situation a
 *  number of copies of this program is running.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @param  max_time_to_try     Number of seconds to try before giving up.
 *  @return                     UDI (unique device id) of parent, or #NULL
 *                              if no parent device was found.
 */
char* find_udi_from_sysfs_path(const char* path, 
                               int max_time_to_try)
{
    char** udis;
    int num_udi;
    int time_slept = 0;
    int time_to_sleep = 500*1000; // every 250 ms 
    
    /*printf("*** found parent=%s\n", parent_path);*/

    while( time_slept<=max_time_to_try*1000*1000 )
    {    
        /* Now find corresponding HAL device */
        udis = hal_manager_find_device_string_match(
            "Linux.sysfs_path_device",
            path,
            &num_udi);
    
        /** @todo fix memory leak */

        if( num_udi!=1 || udis==NULL )
        {
            /* Don't do this if we are probing devices; it'd be a waste of 
             * time*/

            if( is_probing )
                return NULL;

            syslog(LOG_INFO, "Finding UDI for %s; sleeping %d us",
                   path, time_to_sleep);

            if( max_time_to_try>0 )
                usleep(time_to_sleep);
            time_slept += time_to_sleep;
        }
        else
        {
            return udis[0];
        }
    }

    syslog(LOG_INFO, "Giving up finding UDI for %s", path);

    return NULL;
}

/** Given a sysfs-path for a device, this functions finds the HAL device
 *  representing the parent of the given device by truncating the sysfs
 *  path. There may not be a parent device, in which case this function
 *  returns #NULL.
 *
 *  Optionally, the caller may specify for many how seconds to try. This is
 *  useful for hotplug situations where the many hotplug events for a
 *  usb-storage device are not in order. Remember, in this situation a
 *  number of copies of this program is running.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @param  max_time_to_try     Number of seconds to try before giving up.
 *  @return                     UDI (unique device id) of parent, or #NULL
 *                              if no parent device was found.
 */
char* find_parent_udi_from_sysfs_path(const char* path, 
                                      int max_time_to_try)
{
    int i;
    int len;
    char parent_path[SYSFS_PATH_MAX];

    /* Find parent device by truncating our own path */
    strncpy(parent_path, path, SYSFS_PATH_MAX);
    len = strlen(parent_path);
    for(i=len-1; parent_path[i]!='/'; --i)
    {
        parent_path[i]='\0';
    }
    parent_path[i]='\0';

    return find_udi_from_sysfs_path(parent_path, max_time_to_try);
}

/** This function finds a device where a given key mathces a given value.
 *
 *  If several devices match it is undefined which of the devices are
 *  returned.
 *
 *  Optionally, the caller may specify for many how seconds to try. This is
 *  useful for hotplug situations where the many hotplug events for a
 *  input device are not in order. Remember, in this situation a
 *  number of copies of this program is running.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @param
 *  @param  max_time_to_try     Number of seconds to try before giving up.
 *  @return                     UDI (unique device id) of parent, or #NULL
 *                              if no parent device was found.
 */
char* find_udi_by_key_value(const char* key,
                            const char* value,
                            int max_time_to_try)
{
    char** udis;
    int num_udi;
    int time_slept = 0;
    int time_to_sleep = 500*1000; // every 250 ms 
    
    while( time_slept<=max_time_to_try*1000*1000 )
    {    
        /* Now find corresponding HAL device */
        udis = hal_manager_find_device_string_match(
            key,
            value,
            &num_udi);
    
        /** @todo fix memory leak */

        if( num_udi!=1 || udis==NULL )
        {
            /* Don't do this if we are probing devices; it'd be a waste of 
             * time*/

            if( is_probing )
                return NULL;

            syslog(LOG_INFO, "Finding UDI for key,val=%s,%s; sleeping %d us",
                   key, value, time_to_sleep);

            if( max_time_to_try>0 )
                usleep(time_to_sleep);
            time_slept += time_to_sleep;
        }
        else
        {
            return udis[0];
        }
    }

    syslog(LOG_INFO, "Giving up finding UDI for key,val=%s,%s", 
           key, value);

    return NULL;
}

/** Set the physical device for a device.
 *
 *  This function visits all parent devices and sets the property
 *  PhysicalDevice to the first parent device that doesn't have the
 *  PhysicalDevice property set.
 *
 *  @param  udi                 Unique Device Id
 */
void find_and_set_physical_device(char* udi)
{
    char* d;
    char* parent_udi;

    d = udi;

    do
    {
        parent_udi = hal_device_get_property_string(d, "Parent");
        if( parent_udi==NULL )
        {
            printf("Error finding physicl device for %s\n", udi);
            return;
        }

        if( !hal_device_property_exists(parent_udi, "PhysicalDevice") )
        {
            hal_device_set_property_string(udi, "PhysicalDevice", parent_udi);
            return;
        }

        d = parent_udi;
    } 
    while(TRUE);
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

    /*printf("    %s  busid=%s\n", device->bus, device->bus_id);*/

    if( device->bus!=NULL )
    {
        if( strcmp(device->bus, "usb")==0 )
            visit_device_usb(path, device);
        else if( strcmp(device->bus, "pci")==0 )
            visit_device_pci(path, device);
        else if( strcmp(device->bus, "ide")==0 )
            visit_device_ide(path, device);
        /** @todo This is a hack; is there such a thing as an ide_host? */
        else if ( strncmp(device->bus_id, "ide", 3)==0 )
            visit_device_ide_host(path, device);
        else 
        {
            /*printf("bus=%s path=%s\n", device->bus, path);*/
        }
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


/** Visitor function for any class device.
 *
 *  This function determines the class of the device and call the
 *  appropriate visit_class_device_<classtype> function if matched.
 *
 *  @param  path                Sysfs-path for class device, e.g.
 *                              /sys/class/scsi_host/host7
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void visit_class_device(const char* path, dbus_bool_t visit_children)
{
    struct sysfs_class_device* class_device;
    struct sysfs_directory* subdir;

    class_device = sysfs_open_class_device(path);
    if( class_device==NULL )
        DIE(("Coulnd't get sysfs class device object for path %s", path));

/*
    printf("    visit_class_device classname=%s name=%s path=%s\n",
           class_device->classname,
           class_device->name,
           class_device->path);
*/

    if( strcmp(class_device->classname, "scsi_host")==0 )
        visit_class_device_scsi_host(path, class_device);
    else if( strcmp(class_device->classname, "scsi_device")==0 )
        visit_class_device_scsi_device(path, class_device);
    else if( strcmp(class_device->classname, "block")==0 )
        visit_class_device_block(path, class_device);
    else if( strcmp(class_device->classname, "net")==0 )
        visit_class_device_net(path, class_device);

    /* Visit children */
    if( visit_children && class_device->directory!=NULL &&
        class_device->directory->subdirs!=NULL )
    {
        dlist_for_each_data(class_device->directory->subdirs, subdir, 
                            struct sysfs_directory)
        {
            char newpath[SYSFS_PATH_MAX];
            snprintf(newpath, SYSFS_PATH_MAX, "%s/%s", path, subdir->name);
            visit_class_device(newpath, TRUE);
        }
    }

    sysfs_close_class_device(class_device);
}

/** Visit all devices of a given class
 *
 *  @param  class_name          Name of class, e.g. scsi_host or block
 *  @param  visit_children      If children of this device should be visited
 *                              set this to #TRUE. For device-probing, this
 *                              should set be set to true so as to visit
 *                              all devices. For hotplug events, it should
 *                              be set to #FALSE as each sysfs object will
 *                              generate a separate event.
 */
static void visit_class(const char* class_name, dbus_bool_t visit_children)
{
    struct sysfs_class* cls = NULL;
    struct sysfs_class_device* cur = NULL;

    cls = sysfs_open_class(class_name);
    if( cls==NULL )
    {
        fprintf(stderr, "Error opening class %s\n", class_name);
        return;
    }

    if( cls->devices!=NULL )
    {
        dlist_for_each_data(cls->devices, cur, struct sysfs_class_device)
        {
            visit_class_device(cur->path, visit_children);
        }
    }
    
    sysfs_close_class(cls);
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

    /* traverse /sys/devices */
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

    /* visit class devices in /sys/class/scsi_host */
    visit_class("scsi_host", FALSE);

    /* visit class devices in /sys/class/scsi_host */
    visit_class("scsi_device", FALSE);

    /* visit all block devices */
    visit_class("block", TRUE);

    /* visit all net devices */
    visit_class("net", TRUE);

    /* Find the input devices (no yet in sysfs) */
    hal_input_probe();

    /* Process /etc/mtab and modify block devices we indeed have mounted 
     * (dont set up the watcher)
     */
    etc_mtab_process_all_block_devices(FALSE);
}

/** This function is invoked on hotplug add events */
static void device_hotplug_add(char* bus)
{
    int rc;
    const char* devpath;
    char path[SYSFS_PATH_MAX];
    char sysfs_path[SYSFS_PATH_MAX];

    if( strcmp(bus, "input")==0 )
    {
        syslog(LOG_INFO, "adding input device %s", getenv("PHYS"));
        hal_input_handle_hotplug_add();
        return;
    }

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
        visit_device(path, FALSE);
    }
    else if( strcmp(bus, "pci")==0 )
    {
        syslog(LOG_INFO, "adding pci device at %s", path);
        visit_device(path, FALSE);
    }
    else if( strcmp(bus, "scsi_host")==0 )
    {
        syslog(LOG_INFO, "adding scsi_host class device at %s", path);
        visit_class_device(path, FALSE);
    }
    else if( strcmp(bus, "scsi_device")==0 )
    {
        syslog(LOG_INFO, "adding scsi_device class device at %s", path);
        visit_class_device(path, FALSE);
    }
    else if( strcmp(bus, "block")==0 )
    {
        syslog(LOG_INFO, "adding block class device at %s", path);
        visit_class_device(path, FALSE);
    }
    else if( strcmp(bus, "net")==0 )
    {
        syslog(LOG_INFO, "adding net class device at %s", path);
        visit_class_device(path, FALSE);
    }
    else
    {
        syslog(LOG_INFO, "ignoring device add of bus=%s at %s", bus, path);
        return;
    }
}

/** This function is invoked on hotplug remove events */
static void device_hotplug_remove(char* bus)
{
    int rc;
    const char* devpath;
    const char* device_udi = NULL;
    char path[SYSFS_PATH_MAX];
    char sysfs_path[SYSFS_PATH_MAX];
    char** device_udis;
    int num_device_udis;

    if( strcmp(bus, "input")==0 )
    {
        syslog(LOG_INFO, "removing input device %s", getenv("PHYS"));
        hal_input_handle_hotplug_remove();
        return;
    }


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
            syslog(LOG_INFO, "removing usb at %s", path);
    }
    else if( strcmp(bus, "pci")==0 )
    {
        syslog(LOG_INFO, "removing pci device at %s", path);
    }
    else if( strcmp(bus, "scsi_host")==0 )
    {
        syslog(LOG_INFO, "removing scsi_host class device at %s", path);
    }
    else if( strcmp(bus, "scsi_device")==0 )
    {
        syslog(LOG_INFO, "removing scsi_device class device at %s", path);
    }
    else if( strcmp(bus, "block")==0 )
    {
        syslog(LOG_INFO, "removing block class device at %s", path);
    }
    else if( strcmp(bus, "net")==0 )
    {
        syslog(LOG_INFO, "removing net class device at %s", path);
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
    int rc;
    char path[SYSFS_PATH_MAX];
    char sysfs_path[SYSFS_PATH_MAX];
    struct sysfs_directory* current;
    struct sysfs_link* current2;
    struct sysfs_directory* dir;
    struct sysfs_directory* dir2;

    /* get mount path for sysfs */
    rc = sysfs_get_mnt_path(sysfs_path, SYSFS_PATH_MAX);
    if( rc!=0 )
    {
        DIE(("Couldn't get mount path for sysfs"));
    }

    /* traverse /sys/bus/<bus>/drivers */
    snprintf(path, SYSFS_PATH_MAX, "%s/bus/%s/drivers", sysfs_path, bus_name);
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
"        --monitor        Monitor devices (link detection, mount points)\n"
"        --help           Show this information and exit\n"
"\n"
"This program is supposed to only be invoked from the linux-hotplug package\n"
"or from the HAL daemon hald.\n"
"\n");
}

LibHalFunctions hal_functions = {mainloop_integration,
                                 NULL /*property_changed*/,
                                 NULL /*device_added*/, 
                                 NULL /*device_removed*/, 
                                 NULL /*device_new_capability*/, 
                                 NULL /*device_booting*/,
                                 NULL /*device_shutting_down*/,
                                 NULL /*device_disabled*/,
                                 NULL /*device_need_device_info*/,
                                 NULL /*device_boot_error*/,
                                 NULL /*device_enabled*/,
                                 NULL /*device_req_user*/ };

/** Entry point for sysfs agent for HAL on GNU/Linux
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @return                     Exit code
 */
int main(int argc, char* argv[])
{
    GMainLoop* loop;

    fprintf(stderr, "hal-sysfs-agent " PACKAGE_VERSION "\r\n");

    loop = g_main_loop_new(NULL, FALSE);

    if( hal_initialize(&hal_functions)  )
    {
        fprintf(stderr, "error: hal_initialize failed\r\n");
        exit(1);
    }

    openlog("hal-sysfs-agent", LOG_CONS|LOG_PID|LOG_PERROR, LOG_DAEMON);

    hal_pci_init();
    hal_usb_init();
    hal_ide_init();
    hal_scsi_init();
    hal_block_init();
    hal_net_init();
    hal_input_init();

/*
    {
        int i;
        int num_devices;
        char** devices;

        devices = hal_find_device_by_capability("input", &num_devices);
        printf("rc: devices=%x, num_devices=%d\n", devices, num_devices);
        for(i=0; i<num_devices; i++)
        {
            printf("devices[%d] = %s\n", i, devices[i]);
        }
        return 0;
    }
*/

    while(TRUE)
    {
        int c;
        int option_index = 0;
        const char* opt;
        static struct option long_options[] = 
        {
            {"help", 0, NULL, 0},
            {"probe", 0, NULL, 0},
            {"monitor", 0, NULL, 0},
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

/*
    {
        int i;
        int num_devices;
        char** devices;

        devices = hal_get_all_devices(&num_devices);
        printf("rc: devices=%x, num_devices=%d\n", devices, num_devices);
        for(i=0; devices[i]!=NULL; i++)
        {
            hal_device_print(devices[i]);
        }
        return 0;
    }
*/

                return 0;
            }
            else if( strcmp(opt, "monitor")==0 )
            {
                /* Go into monitor mode to monitor /etc/mtab and do network
                 * link detection
                 */
                hal_monitor_enter(loop);
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
