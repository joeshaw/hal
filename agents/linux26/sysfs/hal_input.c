/***************************************************************************
 * CVSID: $Id$
 *
 * hal_input.c : Input device functions for sysfs-agent on Linux 2.6
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

#include <linux/input.h> /* for EV_* etc. */

#include "main.h"
#include "hal_input.h"


/** Key information about input devices from /proc that is not available 
 *  in sysfs
 */
typedef struct input_proc_info_s
{
    int bus;
    int vendor;
    int product;
    int version;

    dbus_uint32_t evbit;
    dbus_uint32_t ledbit;
    dbus_uint32_t relbit;
    dbus_uint32_t absbit;

    char name[128];
    char phys_name[128];
    char handlers[128];
    char keybit[128];

    struct input_proc_info_s* next; /**< next element or #NULL if last */
} input_proc_info;

/** Allocate and initialize an input_proc_info object
 *
 *  @return                     Pointer input_proc_info object
 */
static input_proc_info* get_input_proc_cur_info_obj()
{
    input_proc_info* i;

    i = malloc(sizeof(input_proc_info));
    if( i==NULL )
        DIE(("Cannot allocated memory"));

    (i->name)[0] = '\0';
    (i->phys_name)[0] = '\0';
    (i->handlers)[0] = '\0';
    (i->keybit)[0] = '\0';
    i->evbit = 0;
    i->ledbit = 0;
    i->relbit = 0;
    i->absbit = 0;
    i->bus = 0;
    i->vendor = 0;
    i->product = 0;
    i->version = 0;

    return i;
}


/** First element in input proc linked list */
static input_proc_info* input_proc_head = NULL;

/** Unique device id of the device we are working on */
static input_proc_info* input_proc_cur_info = NULL;


/** Parse the interface field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/input/devices starting
 *                              with "I:"
 */
static void input_proc_handle_interface(input_proc_info* info, char* s)
{
    info->bus = find_num("Bus=", s, 16);
    info->vendor = find_num("Vendor=", s, 16);
    info->product = find_num("Product=", s, 16);
    info->version = find_num("Version=", s, 16);
}

/** Parse the name field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/input/devices starting
 *                              with "N:"
 */
static void input_proc_handle_name(input_proc_info* info, char* s)
{
    char* str;
    /* grr, the kernel puts quotes in the name */
    str = find_string("Name=\"", s);

    if( str!=NULL )
    {
        strncpy(info->name, str, 128);
        /* remove the trailing quote */
        (info->name)[strlen(info->name)-1] = '\0';
    }
    else
        (info->name)[0]='\0';

    
}

/** Parse the physical information field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/input/devices starting
 *                              with "P:"
 */
static void input_proc_handle_phys(input_proc_info* info, char* s)
{
    char* str;
    str = find_string("Phys=", s);
    if( str!=NULL )
        strncpy(info->phys_name, str, 128);
    else
        (info->phys_name)[0]='\0';
}

/** Parse the handlers field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/input/devices starting
 *                              with "H:"
 */
static void input_proc_handle_handlers(input_proc_info* info, char* s)
{
    char* str;
    str = find_string("Handlers=", s);
    if( str!=NULL )
        strncpy(info->handlers, str, 128);
    else
        (info->handlers)[0]='\0';
}

/** Parse a bits field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/input/devices starting
 *                              with "N:"
 */
static void input_proc_handle_bits(input_proc_info* info, char* s)
{
    int num;

    if( (num=find_num("EV=", s, 16))!=LONG_MAX )
    {
        info->evbit = num;
        return;
    }

    if( (num=find_num("LED=", s, 16))!=LONG_MAX )
    {
        info->ledbit = num;
        return;
    }

    if( (num=find_num("REL=", s, 16))!=LONG_MAX )
    {
        info->relbit = num;
        return;
    }

    if( (num=find_num("ABS=", s, 16))!=LONG_MAX )
    {
        info->absbit = num;
        return;
    }

    if( strncmp("B: KEY=", s, 7)==0 )
    {
        strncpy(info->keybit, s+7, 128);
        /* remove trailing newline */
        info->keybit[strlen(info->keybit)-1] = '\0';
    }
}



/** Called when an entry from /proc/bus/input/devices have been parsed.
 *
 *  @param  info                Structure representing the entry
 */
static void input_proc_device_done(input_proc_info* info)
{
    info->next = input_proc_head;
    input_proc_head = info;
}


/** Parse a line from /proc/bus/input/devices
 *
 *  @param  s                   Line from /proc/bus/input/devices
 */
static void input_proc_parse_line(char* s)
{
    /* See drivers/input/input.c, function input_devices_read() in 
     * the Linux kernel for details.
     */

    switch( s[0] )
    {
    case 'I': /* interface; always present, indicates a new device */
        if( input_proc_cur_info!=NULL )
        {
            // beginning of a new device, done with current
            input_proc_device_done(input_proc_cur_info);
        }

        input_proc_cur_info = get_input_proc_cur_info_obj();

        input_proc_handle_interface(input_proc_cur_info, s);
        break;

    case 'N': /* Name */
        input_proc_handle_name(input_proc_cur_info, s);
        break;

    case 'P': /* Phyiscal information */
        input_proc_handle_phys(input_proc_cur_info, s);
        break;

    case 'H': /* Handlers */
        input_proc_handle_handlers(input_proc_cur_info, s);
        break;

    case 'B': /* Bits */
        input_proc_handle_bits(input_proc_cur_info, s);
        break;


    default:
        break;
    }
}

/** Parse /proc/bus/input/devices
 */
static void input_proc_parse()
{
    FILE* f;
    char buf[256];

    f = fopen("/proc/bus/input/devices", "r");
    if( f==NULL )
    {
        DIE(("Couldn't open /proc/bus/input/devices"));
    }

    while( !feof(f) )
    {
        fgets(buf, 256, f);
        input_proc_parse_line(buf);
    }
    input_proc_device_done(input_proc_cur_info);

    {
        input_proc_info* i;
        for(i=input_proc_head; i!=NULL; i=i->next)
        {
            printf("/p/b/i/d entry\n");
            printf("  bus               %d\n", i->bus);
            printf("  vendor            %d\n", i->vendor);
            printf("  product           %d\n", i->product);
            printf("  version           %d\n", i->version);
            printf("  evbit             0x%08x\n", i->evbit);
            printf("  name              '%s'\n", i->name);
            printf("  phys_name         '%s'\n", i->phys_name);
            printf("  handlers          '%s'\n", i->handlers);
            printf("\n");
        }
    }
}


    /*

This is an example input event

----------------------------------------
Sat Nov 29 14:32:55 CET 2003
----------------------------------------
input
----------------------------------------
EV=7
NAME=Logitech USB-PS/2 Optical Mouse
PATH=/sbin:/bin:/usr/sbin:/usr/bin
ACTION=add
PWD=/
KEY=f0000 0 0 0 0 0 0 0 0
REL=103
HOME=/
SHLVL=2
PHYS=usb-0000:00:07.2-1.1.2/input0
PRODUCT=3/46d/c012/1320
_=/bin/env
----------------------------------------
----------------------------------------

This is /proc/bus/input/devices                                                                                
I: Bus=0011 Vendor=0002 Product=0001 Version=0000
N: Name="PS/2 Generic Mouse"
P: Phys=isa0060/serio1/input0
H: Handlers=mouse0
B: EV=7
B: KEY=70000 0 0 0 0 0 0 0 0
B: REL=3
 
I: Bus=0011 Vendor=0001 Product=0002 Version=ab02
N: Name="AT Translated Set 2 keyboard"
P: Phys=isa0060/serio0/input0
H: Handlers=kbd
B: EV=120003
B: KEY=4 2200000 c061f9 fbc9d621 efdfffdf ffefffff ffffffff fffffffe
B: LED=7
 
I: Bus=0003 Vendor=04b3 Product=3005 Version=0100
N: Name="Silitek IBM USB HUB KEYBOARD"
P: Phys=usb-0000:00:07.2-1.2.1/input0
H: Handlers=kbd
B: EV=120003
B: KEY=10000 7f ffe7207a c14057ff ffbeffdf ffffffff ffffffff fffffffe
B: LED=1f
 
I: Bus=0003 Vendor=046d Product=c012 Version=1320
N: Name="Logitech USB-PS/2 Optical Mouse"
P: Phys=usb-0000:00:07.2-1.2.2/input0
H: Handlers=mouse1
B: EV=7
B: KEY=f0000 0 0 0 0 0 0 0 0
B: REL=103

     */


/** Process an input device field.
 *
 *  @param  i                   input device field
 */
static void process_input_proc_info(input_proc_info* i)
{
    int n;
    char* udi;
    char phys[128];

    /* First extract physical name */
    for(n=0; n<128; n++)
    {
        if( i->phys_name[n]=='/' )
            break;
        phys[n] = i->phys_name[n];
    }
    phys[n] = '\0';
    
/*
  if( sscanf(i->phys_name+n+1, "input%d", &in_num)!=1 )
  {
      printf("Error parsing %s\n", i->phys_name+n+1);
      continue;
  }
*/

    udi = find_udi_by_key_value("linux.kernel_devname", phys, 
                                HAL_LINUX_HOTPLUG_TIMEOUT);
    if( udi==NULL )
    {
        syslog(LOG_WARNING, "No HAL device for input %s\n", i->phys_name);
        return;
    }
    
    syslog(LOG_INFO, "Found HAL device %s for input %s\n", 
           udi, i->phys_name);
    
    hal_device_set_property_string(udi, "input.linux.phys", 
                                   i->phys_name);
    hal_device_set_property_string(udi, "input.linux.handlers", 
                                   i->handlers);
    hal_device_set_property_int(udi, "input.linux.evbit", 
                                i->evbit);
    hal_device_set_property_int(udi, "input.linux.ledbit", 
                                i->ledbit);
    hal_device_set_property_int(udi, "input.linux.relbit", 
                                i->relbit);
    hal_device_set_property_int(udi, "input.linux.absbit", 
                                i->absbit);
    hal_device_set_property_string(udi, "input.linux.keybit", 
                                   i->keybit);
        
    /* The "USB HID for Linux USB" document provides good information
     * http://www.frogmouth.net/hid-doco/linux-hid.html
     *
     */
    hal_device_set_property_bool(udi, "input.key", 
                                 i->evbit&(1<<EV_KEY));
    hal_device_set_property_bool(udi, "input.relative", 
                                 i->evbit&(1<<EV_REL));
    hal_device_set_property_bool(udi, "input.absolute", 
                                 i->evbit&(1<<EV_ABS));
    hal_device_set_property_bool(udi, "input.led", 
                                 i->evbit&(1<<EV_LED));
    hal_device_set_property_bool(udi, "input.sound", 
                                 i->evbit&(1<<EV_SND));
    hal_device_set_property_bool(udi, "input.repeat", 
                                 i->evbit&(1<<EV_REP));
    hal_device_set_property_bool(udi, "input.forceFeedback", 
                                 i->evbit&(1<<EV_FF));
    hal_device_set_property_bool(udi, "input.misc", 
                                 i->evbit&(1<<EV_MSC));
    hal_device_set_property_bool(udi, "input.rst", 
                                 i->evbit&(1<<EV_RST));
    
    if( i->evbit&(1<<EV_LED) )
    {
        hal_device_set_property_bool(udi, "input.led.numlock", 
                                     i->ledbit&(1<<LED_NUML));
        hal_device_set_property_bool(udi, "input.led.capslock", 
                                     i->ledbit&(1<<LED_CAPSL));
        hal_device_set_property_bool(udi, "input.led.scrolllock", 
                                     i->ledbit&(1<<LED_SCROLLL));
        hal_device_set_property_bool(udi, "input.led.compose", 
                                     i->ledbit&(1<<LED_COMPOSE));
        hal_device_set_property_bool(udi, "input.led.kana", 
                                     i->ledbit&(1<<LED_KANA));
        hal_device_set_property_bool(udi, "input.led.sleep", 
                                     i->ledbit&(1<<LED_SLEEP));
        hal_device_set_property_bool(udi, "input.led.suspend", 
                                     i->ledbit&(1<<LED_SUSPEND));
        hal_device_set_property_bool(udi, "input.led.mute", 
                                     i->ledbit&(1<<LED_MUTE));
        hal_device_set_property_bool(udi, "input.led.misc", 
                                     i->ledbit&(1<<LED_MISC));
        hal_device_set_property_bool(udi, "input.led.max", 
                                     i->ledbit&(1<<LED_MAX));
    }
    
    if( i->evbit&(1<<EV_REL) )
    {
        hal_device_set_property_bool(udi, "input.relative.x", 
                                     i->relbit&(1<<REL_X));
        hal_device_set_property_bool(udi, "input.relative.y", 
                                     i->relbit&(1<<REL_Y));
        hal_device_set_property_bool(udi, "input.relative.z", 
                                     i->relbit&(1<<REL_Z));
        hal_device_set_property_bool(udi, "input.relative.hwheel", 
                                     i->relbit&(1<<REL_HWHEEL));
        hal_device_set_property_bool(udi, "input.relative.dial", 
                                     i->relbit&(1<<REL_DIAL));
        hal_device_set_property_bool(udi, "input.relative.wheel", 
                                     i->relbit&(1<<REL_WHEEL));
        hal_device_set_property_bool(udi, "input.relative.misc", 
                                     i->relbit&(1<<REL_MISC));
    }
    
    if( i->evbit&(1<<EV_ABS) )
    {
        hal_device_set_property_bool(udi, "input.absolute.x", 
                                     i->absbit&(1<<ABS_X));
        hal_device_set_property_bool(udi, "input.absolute.y", 
                                     i->absbit&(1<<ABS_Y));
        hal_device_set_property_bool(udi, "input.absolute.z", 
                                     i->absbit&(1<<ABS_Z));
        hal_device_set_property_bool(udi, "input.absolute.rx", 
                                     i->absbit&(1<<ABS_RX));
        hal_device_set_property_bool(udi, "input.absolute.ry", 
                                     i->absbit&(1<<ABS_RY));
        hal_device_set_property_bool(udi, "input.absolute.rz", 
                                     i->absbit&(1<<ABS_RZ));
        hal_device_set_property_bool(udi, "input.absolute.throttle", 
                                     i->absbit&(1<<ABS_THROTTLE));
        hal_device_set_property_bool(udi, "input.absolute.rudder", 
                                     i->absbit&(1<<ABS_RUDDER));
        hal_device_set_property_bool(udi, "input.absolute.wheel", 
                                     i->absbit&(1<<ABS_WHEEL));
        hal_device_set_property_bool(udi, "input.absolute.gas", 
                                     i->absbit&(1<<ABS_GAS));
        hal_device_set_property_bool(udi, "input.absolute.brake", 
                                     i->absbit&(1<<ABS_BRAKE));
        hal_device_set_property_bool(udi, "input.absolute.hat0x", 
                                     i->absbit&(1<<ABS_HAT0X));
        hal_device_set_property_bool(udi, "input.absolute.hat0y", 
                                     i->absbit&(1<<ABS_HAT0Y));
        hal_device_set_property_bool(udi, "input.absolute.hat1x", 
                                     i->absbit&(1<<ABS_HAT1X));
        hal_device_set_property_bool(udi, "input.absolute.hat1y", 
                                     i->absbit&(1<<ABS_HAT1Y));
        hal_device_set_property_bool(udi, "input.absolute.hat2x", 
                                     i->absbit&(1<<ABS_HAT2X));
        hal_device_set_property_bool(udi, "input.absolute.hat2y", 
                                     i->absbit&(1<<ABS_HAT2Y));
        hal_device_set_property_bool(udi, "input.absolute.pressure", 
                                     i->absbit&(1<<ABS_PRESSURE));
        hal_device_set_property_bool(udi, "input.absolute.distance", 
                                     i->absbit&(1<<ABS_DISTANCE));
        hal_device_set_property_bool(udi, "input.absolute.tilt_x", 
                                     i->absbit&(1<<ABS_TILT_X));
        hal_device_set_property_bool(udi, "input.absolute.tilt_Y", 
                                     i->absbit&(1<<ABS_TILT_Y));
        hal_device_set_property_bool(udi, "input.absolute.misc", 
                                     i->absbit&(1<<ABS_MISC));
    }

    /* Either way, we're an input device */
    hal_device_add_capability(udi, "input");

    /** @todo Figure out if this input device is a mouse, a keyboard or.. 
     *        bzzzttt.. an UPS.. We can wawe our hands a bit because this
     *        can always be overridden by a .fdi file..
     */
    if( i->relbit!=0 )
    {
        hal_device_add_capability(udi, "input.mouse");
        hal_device_set_property_string(udi, "Category", "input.mouse");
    }
    else
    {
        hal_device_add_capability(udi, "input.keyboard");
        hal_device_set_property_string(udi, "Category", "input.keyboard");
    }
    
    /** @todo FIXME, is there any other key information we want to
     *        give here? Like major:minor number of device?
     */
}

/** Probe for input devices by looking at /proc/bus/input/devices. Do
 *  not call this before all physical devices are probed as no new HAL
 *  devices will be created..
 */
void hal_input_probe()
{
    input_proc_info* i;
    
    input_proc_parse();    

    for(i=input_proc_head; i!=NULL; i=i->next)
        process_input_proc_info(i);
}

/** Handle input hotplug event add
 *
 */
void hal_input_handle_hotplug_add()
{
    char* s;
    input_proc_info* i;

    i = get_input_proc_cur_info_obj();

    /** @todo FIXME; parse product (we don't use it yet) */

    if( (s = getenv("NAME")) == NULL )
    {
        syslog(LOG_ERR, "$NAME not in hotplug environment");
        return;
    }
    strncpy(i->name, s, 128);

    if( (s = getenv("PHYS")) == NULL )
    {
        syslog(LOG_ERR, "$PHYS not in hotplug environment");
        return;
    }
    strncpy(i->phys_name, s, 128);


    if( (s = getenv("EV")) == NULL )
    {
        syslog(LOG_ERR, "$EV not in hotplug environment");
        return;
    }
    i->evbit = parse_dec(s);

    if( (s = getenv("KEY")) == NULL )
    {
        syslog(LOG_ERR, "$KEY not in hotplug environment");
        return;
    }
    strncpy(i->keybit, s, 128);

    if( (s = getenv("REL")) != NULL )
    {
        i->relbit = parse_hex(s);
    }

    if( (s = getenv("ABS")) != NULL )
    {
        i->absbit = parse_hex(s);
    }

    if( (s = getenv("LED")) != NULL )
    {
        i->ledbit = parse_hex(s);
    }

    process_input_proc_info(i);
}

/** Handle input hotplug event remove
 *
 */
void hal_input_handle_hotplug_remove()
{
    /* this function is left intentionally blank */
}

/** Init function for block device handling
 *
 */
void hal_input_init()
{
}

/** Shutdown function for block device handling
 *
 */
void hal_input_shutdown()
{
}

