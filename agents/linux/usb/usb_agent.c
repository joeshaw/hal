/***************************************************************************
 * CVSID: $Id$
 *
 * usb_agent.c : USB agent for HAL on GNU/Linux
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
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include <libhal/libhal.h>

/** @defgroup HalAgentsLinuxUsb USB
 *  @ingroup  HalAgentsLinux
 *  @brief    HAL agent for USB devices on GNU/Linux
 *
 *  @{
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

/** D-BUS mainloop integration for libhal.
 *
 *  @param  dbus_connection     D-BUS connection to integrate
 */
static void mainloop_integration(DBusConnection* dbus_connection)
{
    dbus_connection_setup_with_g_main(dbus_connection, NULL);
}

static void usage()
{
    fprintf(stderr, 
"\n"
"usage : hal_agent.usb [--help] [--probe]\n"
"\n"
"        --probe          Probe devices on the bus\n"
"        --help           Show this information and exit\n"
"\n"
"This program is supposed to only be invoked from the linux-hotplug package\n"
"or from the HAL daemon hald.\n"
"\n");
}

/** This function is called when the program is invoked by the
 *  linux-hotplug subsystem.
 */
static void usb_hotplug()
{
    printf("blah hotplug\n");
}





// fails if number not found
static long int find_num(char* pre, char* s, int base)
{
    char* where;
    int result;

    where = strstr(s, pre);
    if( where==NULL )
        DIE(("Didn't find '%s' in '%s'", pre, s));
    where += strlen(pre);

    result = strtol(where, NULL, base);
    if( result==LONG_MIN || result==LONG_MAX )
        DIE(("Error parsing value for '%s' in '%s'", pre, s));

    return result;
}

// fails if double not found
static double find_double(char* pre, char* s)
{
    char* where;
    double result;

    where = strstr(s, pre);
    if( where==NULL )
        DIE(("Didn't find '%s' in '%s'", pre, s));
    where += strlen(pre);

    result = atof(where);

    return result;
}

// return val only valid until next invocation. Return NULL if string not found
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


// T:  Bus=00 Lev=00 Prnt=00 Port=00 Cnt=00 Dev#=  1 Spd=12  MxCh= 2
static void handle_topology(const char* d, char* s)
{
    int busNumber, deviceNumber;

    busNumber = find_num("Bus=", s, 10);
    deviceNumber = find_num("Dev#=",s, 10);

/*
    if( !full_probe )
    {
        if(    busNumber==hal_device_get_property_int(d, "usb.busNumber") &&
            deviceNumber==hal_device_get_property_int(d, "usb.deviceNumber") )
        {
            merge_probe_matched = 1;
        }
    }

    if( !merge_probe_matched && !full_probe )
        return;
*/

    hal_device_set_property_int(d, "usb.busNumber", busNumber);
    hal_device_set_property_int(d, "usb.levelNumber", find_num("Lev=", s, 10));
    hal_device_set_property_int(d, "usb.parentNumber", find_num("Prnt=",s,10));
    hal_device_set_property_int(d, "usb.portNumber", find_num("Port=",s,10));
    hal_device_set_property_int(d, "usb.busCount", find_num("Cnt=", s, 10));
    hal_device_set_property_int(d, "usb.deviceNumber", deviceNumber);
    hal_device_set_property_double(d, "usb.speed", find_double("Spd=", s));
}

static void handle_device_info(const char* d, char* s)
{
/*    if( !merge_probe_matched && !full_probe )
        return;
*/

    hal_device_set_property_double(d, "usb.version", find_double("Ver=", s));
    hal_device_set_property_int(d, "usb.bDeviceClass", find_num("Cls=", s,16));
    hal_device_set_property_int(d,"usb.bDeviceSubClass",find_num("Sub=",s,16));
    hal_device_set_property_int(d,"usb.bDeviceProtocol",
                                find_num("Prot=", s, 16));
    hal_device_set_property_int(d,"usb.numConfigurations",
                                find_num("#Cfgs=",s,10));
}

static void handle_device_info2(const char* d, char* s)
{
/*
    if( !merge_probe_matched && !full_probe )
        return;
*/

    hal_device_set_property_int(d, "usb.idVendor", find_num("Vendor=", s, 16));
    hal_device_set_property_int(d, "usb.idProduct",find_num("ProdID=", s, 16));
    hal_device_set_property_double(d, "usb.revisionProduct", 
                                  find_double("Rev=", s));
}

static void handle_device_string(const char* d, char* s)
{
    char* str;

/*
    if( !merge_probe_matched && !full_probe )
        return;
*/

    str = find_string("Manufacturer=", s);
    if( str!=NULL && strlen(str)>0 )
    {
        hal_device_set_property_string(d, "usb.VendorString", str);
        return; // Only one per line
    }

    str = find_string("Product=", s);
    if( str!=NULL && strlen(str)>0 )
    {
        hal_device_set_property_string(d, "usb.ProductString", str);
        return; // Only one per line
    }

    str = find_string("SerialNumber=", s);
    if( str!=NULL && strlen(str)>0 )
    {
        hal_device_set_property_string(d, "usb.SerialNumber", str);
        return; // Only one per line
    }

    fprintf(stderr, "hal-agent.usb : Unknown or borken device string '%s'", s);
}

/** This function will compute the device uid based on other properties
 *  of the device. Specifically, the following properties are required:
 *
 *   - usb.idVendor, usb.idProduct, usb.revisionProduct. 
 *
 *  Other properties may also be used, specifically the usb.SerialNumber 
 *  is used if available.
 *
 *  In the event two similar devices is plugged in, this function will
 *  still compute a unique id by concatenating -1, -2, .. to the device name.
 *
 *  Requirements for uid:
 *   - do not rely on bus, port etc.; we want this id to be as unique for
 *     the device as we can
 *   - make sure it doesn't rely on properties that cannot be obtained
 *     from a) probing; b) hotplug events that have called 
 *     usb_hotplug_merge_more
 *
 *  @param  udi                 Unique device id of tempoary device object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char* usb_compute_udi(const char* udi, int append_num)
{
    static char buf[256];

    /** @todo Rework this function to use e.g. serial number */

    if( append_num==-1 )
        sprintf(buf, "/org/freedesktop/Hal/devices/usb_%d_%d_%f", 
                hal_device_get_property_int(udi, "usb.idVendor"),
                hal_device_get_property_int(udi, "usb.idProduct"),
                hal_device_get_property_double(udi, "usb.revisionProduct"));
    else
        sprintf(buf, "/org/freedesktop/Hal/devices/usb_%d_%d_%f/%d", 
                hal_device_get_property_int(udi, "usb.idVendor"),
                hal_device_get_property_int(udi, "usb.idProduct"),
                hal_device_get_property_double(udi, "usb.revisionProduct"),
                append_num);
    
    return buf;
}

/** Maximum number of USB devices supported */
#define USB_MAX_NUM_DEVICES 512

/** List of UDI's for probed devices */
const char* usb_probe_devices[USB_MAX_NUM_DEVICES];

/** Number of elements in #usb_probe_devices */
unsigned int usb_probe_num_devices = 0;

/** Add device to #usb_probe_devices
 *
 *  @param  udi                 Unique device id
 */
static void device_done(const char* udi)
{
/*
    const char* new_udi;
    // set unique id and commit
    new_udi = usb_compute_name_and_commit(d);
*/


    if( usb_probe_num_devices==USB_MAX_NUM_DEVICES )
    {
        fprintf(stderr, "hal_agent.usb : Max %d devices supported!",
                USB_MAX_NUM_DEVICES);
        return;
    }

    // save name
    usb_probe_devices[usb_probe_num_devices++] = udi;
}

static int curConfigNum;

static void handle_config_desc(const char* d, char* s)
{
    int configNum;
    char buf[256];

/*
    if( !merge_probe_matched && !full_probe )
        return;
*/

    configNum = find_num("Cfg#=", s, 10);

    // hack, but this is how /proc/bus/usb/devices is...
    //
    curConfigNum = configNum;

    sprintf(buf, "usb.config.%d.numInterfaces", configNum);
    hal_device_set_property_int(d, buf, find_num("#Ifs=", s, 10));

    sprintf(buf, "usb.config.%d.attributes", configNum);
    hal_device_set_property_int(d, buf, find_num("Atr=", s, 16));

    sprintf(buf, "usb.config.%d.maxPower", configNum);
    hal_device_set_property_int(d, buf, find_num("MxPwr=", s, 10));
}

static void handle_interface_desc(const char* d, char* s)
{
    int iNum;
    char* driver;
    char buf[256];
    int klass, sub_class, protocol;

/*
    if( !merge_probe_matched && !full_probe )
        return;
*/

    iNum = find_num("If#=", s, 10);

    sprintf(buf, "usb.config.%d.%d.alternateNumber", curConfigNum, iNum);
    hal_device_set_property_int(d, buf, find_num("Alt=", s, 10));

    sprintf(buf, "usb.config.%d.%d.numEndpoints", curConfigNum, iNum);
    hal_device_set_property_int(d, buf, find_num("#EPs=", s, 10));

    klass = find_num("Cls=", s, 16);
    sub_class = find_num("Sub=", s, 16);
    protocol = find_num("Prot=", s, 16);

    sprintf(buf, "usb.config.%d.%d.class", curConfigNum, iNum);
    hal_device_set_property_int(d, buf, klass);

    sprintf(buf, "usb.config.%d.%d.subClass", curConfigNum, iNum);
    hal_device_set_property_int(d, buf, sub_class);

    sprintf(buf, "usb.config.%d.%d.protocol", curConfigNum, iNum);
    hal_device_set_property_int(d, buf, protocol);

    sprintf(buf, "usb.config.%d.%d.linux_driver", curConfigNum, iNum);
    driver = find_string("Driver=", s);
    if( driver!=NULL && strcmp(driver, "(none)")!=0 )
        hal_device_set_property_string(d, buf, driver);

    // duplicate first interface for convenience
    if( iNum==0 )
    {
        hal_device_set_property_int(d, "usb.bInterfaceClass", klass);
        hal_device_set_property_int(d, "usb.bInterfaceSubClass", sub_class);
        hal_device_set_property_int(d, "usb.bInterfaceProtocol", protocol);
    }
}

/** Unique device id of the device we are working on */
static char* usb_cur_udi = NULL;

/** Parse a line from /proc/bus/usb/devices
 *
 *  @param  s                   Line from /proc/bus/usb/devices
 */
static void parse_line(char* s)
{
    switch( s[0] )
    {
    case 'T': /* topology; always present, indicates a new device */
/*
        if( full_probe )
        {
*/
            if( usb_cur_udi!=NULL )
            {
                // beginning of a new device, done with usb_cur_udi
                device_done(usb_cur_udi);
            }
            usb_cur_udi = hal_agent_new_device();
            assert(usb_cur_udi!=NULL);
            hal_device_set_property_string(usb_cur_udi, "Bus", "usb");
/*
        }
        else
        {
            // not matched anymore
            merge_probe_matched = 0;
        }
*/
        handle_topology(usb_cur_udi, s);
        break;

    case 'B': /* bandwidth */
        break;

    case 'D': /* device information */
        handle_device_info(usb_cur_udi, s);
        break;

    case 'P': /* more device information */
        handle_device_info2(usb_cur_udi, s);
        break;

    case 'S': /* device string information */
        handle_device_string(usb_cur_udi, s);
        break;

    case 'C': /* config descriptor info */
        handle_config_desc(usb_cur_udi, s);
        break;

    case 'I': /* interface descriptor info */
        handle_interface_desc(usb_cur_udi, s);
        break;

    case 'E': /* endpoint descriptor info */
        break;

    default:
        break;
    }
}

/** Find a parent for a device.
 *
 *  @param  d                   UDI of device to find parent for; must
 *                              represent an USB device
 *  @param  devices             Array of UDI's for devices that may be parent
 *                              the given device to find a parent for; if
 *                              an UDI is #NULL it will be skipped
 *  @param  num_devices         Number of elements in array
 */
static void usb_device_set_parent(char* d, char** devices, 
                                  int num_devices)
{
    int i;

    // Root hub, cannot have parent device
    if( hal_device_get_property_int(d, "usb.levelNumber")==0 )
    {
        /** @todo Should set parent of the USB device to the corresponding
         *        PCI bridge device (on a x86 legacy system at least)
         */
        return;
    }

    // Need to find parent of device; go search through all USB devices
    for(i=0; i<num_devices; i++)
    {
        const char* c = devices[i]; // for Candidate
       
        // means it is not an USB device; see usb_compute_parents
        if( c==NULL )
            continue;

        // only test against USB devices
        if( strcmp(hal_device_get_property_string(c, "Bus"), "usb")!=0 )
            continue;

        if( hal_device_get_property_int(d, "usb.parentNumber")==
            hal_device_get_property_int(c, "usb.deviceNumber")   &&
            hal_device_get_property_int(d, "usb.busNumber")==
            hal_device_get_property_int(c, "usb.busNumber")    )
        {
            hal_device_set_property_string(d, "Parent", c);
            break;
        }
    }
}

/** Set Parent property for all USB devices in the GDL
 */
static void usb_compute_parents()
{
    int i;
    int num_devices;
    char** devices;

    devices = hal_get_all_devices(&num_devices);

    /* minimize set to USB devices */
    for(i=0; i<num_devices; i++)
    {
        const char* bus;

        bus = hal_device_get_property_string(devices[i], "Bus");

        if( bus==NULL || strcmp(bus, "usb")!=0 )
        {
            /* not an USB device, remove from list.. */
            free(devices[i]);
            devices[i]=NULL;
        }   
    }

    /* compute parent for each USB device */
    for(i=0; i<num_devices; i++)
    {
        if( devices[i]==NULL )
            continue;
        usb_device_set_parent(devices[i], devices, num_devices);
    }

    hal_free_string_array(devices);
}

/** This function is called when the program is invoked to 
 *  probe the bus.
 */
static void usb_probe()
{
    unsigned int i;
    FILE* f;
    char buf[256];

    f = fopen("/proc/bus/usb/devices", "r");
    if( f==NULL )
        return;

    /* First probe all USB devices and put them in a list; usb_probe_devices..
     * they are all hidden and with temporary UDI's
     */
    usb_cur_udi = NULL;
    usb_probe_num_devices = 0;
    //full_probe = 1;

    while( !feof(f) )
    {
        fgets(buf, 256, f);
        parse_line(buf);
    }
    device_done(usb_cur_udi);         /* done with usb_cur_udi */

    /* Right, now go through all devices and (possibly) add each of them */
    for(i=0; i<usb_probe_num_devices; i++)
    {
        int append_num;
        const char* udi;
        char* computed_udi;

        /* this is a temporary udi */
        udi = usb_probe_devices[i];

        
        append_num = -1;

    tryagain:
        /* compute the udi for the device */
        computed_udi = usb_compute_udi(udi, append_num);

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
            printf("foobar1\n");

            if( hal_device_get_property_int(computed_udi, "State")!=
                HAL_STATE_UNPLUGGED )
            {
                printf("foobar2\n");

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
                if( hal_agent_device_matches(computed_udi, udi, "usb") )
                {
                    fprintf(stderr, "Found device already present as '%s'!\n",
                            computed_udi);
                    /* indeed a match, must be b) ; ignore device */
                    hal_agent_remove_device(udi);
                    /* and continue processing the next device */
                    continue;
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
        
    } /* for all probed devices */

    /* Now compute parents for all USB devices */
    usb_compute_parents();
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

    fprintf(stderr, "hal_agent.usb " PACKAGE_VERSION "\r\n");

    if( hal_initialize(&hal_functions)  )
    {
        fprintf(stderr, "error: hal_initialize failed\r\n");
        exit(1);
    }

    if( argc==2 && strcmp(argv[1], "usb")==0 )
        usb_hotplug();

    while(1)
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
                usb_probe();
                exit(0);
            }

        default:
            usage();
            exit(1);
            break;
        }         
    }

    usage();

    return 0;
}

/** @} */
