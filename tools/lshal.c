/***************************************************************************
 * CVSID: $Id$
 *
 * lshal.c : Show devices managed by HAL
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include <glib.h>
#include <dbus/dbus-glib.h>

#include <libhal/libhal.h>


static void die(const char *message)
{
    fprintf(stderr, "*** %s", message);
    exit (1);
}

static void dump_devices()
{
    int i;
    int num_devices;
    char** device_names;

    device_names = hal_get_all_devices(&num_devices);

    if( device_names==NULL )
        die("Couldn't obtain list of devices\n");

    printf("\n"
           "Dumping %d device(s) from the Global Device List:\n"
           "-------------------------------------------------\n", num_devices);

    for(i=0; i<num_devices; i++)
    {
        LibHalPropertySet* props;
        LibHalPropertySetIterator it;
        int type;

        props = hal_device_get_all_properties(device_names[i]);

        /* NOTE NOTE NOTE: This may be NULL if the device was removed
         *                 in the daemon; this is because hal_device_get_all_
         *                 properties() is a in essence an IPC call and
         *                 other stuff may be happening..
         */
        if( props==NULL )
            continue;

        printf("udi = '%s'\n", device_names[i]);
        
        for(hal_psi_init(&it, props); hal_psi_has_more(&it); hal_psi_next(&it))
        {
            type = hal_psi_get_type(&it);
            switch( type )
            {
            case DBUS_TYPE_STRING:
                printf("  %s = '%s'  (string)\n", 
                       hal_psi_get_key(&it), hal_psi_get_string(&it));
                break;

            case DBUS_TYPE_INT32:
                printf("  %s = %d  (0x%x)  (int)\n", 
                       hal_psi_get_key(&it), 
                       hal_psi_get_int(&it), hal_psi_get_int(&it));
                break;

            case DBUS_TYPE_DOUBLE:
                printf("  %s = %g  (double)\n", 
                       hal_psi_get_key(&it), hal_psi_get_double(&it));
                break;

            case DBUS_TYPE_BOOLEAN:
                printf("  %s = %s  (bool)\n", 
                       hal_psi_get_key(&it), 
                       hal_psi_get_bool(&it) ? "true" : "false");
                break;
            }
        }
        hal_free_property_set(props);
        printf("\n");
    }

    dbus_free_string_array(device_names);

    printf("\n"
           "Dumped %d device(s) from the Global Device List:\n"
           "------------------------------------------------\n", num_devices);

    printf("\n");
}

static void device_added(const char* udi)
{
    fprintf(stderr, "*** lshal: device_added, udi='%s'\n", udi);
    dump_devices();
}

static void device_removed(const char* udi)
{
    fprintf(stderr, "*** lshal: device_removed, udi='%s'\n", udi);
    dump_devices();
}

static void device_new_capability(const char* udi, const char* capability)
{
    fprintf(stderr, "*** lshal: new_capability, udi='%s'\n", udi);
    fprintf(stderr, "*** capability: %s\n", capability);
    /*dump_devices();*/
}


static void print_property(const char* udi, const char* key)
{
    int type;

    type = hal_device_get_property_type(udi, key);

    switch( type )
    {
    case DBUS_TYPE_STRING:
        fprintf(stderr, "*** new value: '%s'  (string)\n", 
                hal_device_get_property_string(udi, key));
        break;
    case DBUS_TYPE_INT32:
    {
        dbus_int32_t value = hal_device_get_property_int(udi, key);
        fprintf(stderr, "*** new value: %d (0x%x)  (int)\n", value, value);
    }
    break;
    case DBUS_TYPE_DOUBLE:
        fprintf(stderr, "*** new value: %g  (double)\n", 
                hal_device_get_property_double(udi, key));
        break;
    case DBUS_TYPE_BOOLEAN:
        fprintf(stderr, "*** new value: %s  (bool)\n", 
                hal_device_get_property_bool(udi, key) ? "true" : "false");
        break;
        
    default:
        fprintf(stderr, "Unknown type %d='%c'\n", type, type);
        break;
    }
}

static void property_changed(const char* udi, const char* key)
{
    fprintf(stderr, "*** lshal: property_changed, udi='%s', key=%s\n", 
            udi, key);
    print_property(udi, key);
    fprintf(stderr, "\n");
    /*dump_devices();*/
}

static void property_added(const char* udi, const char* key)
{
    fprintf(stderr, "*** lshal: property_added, udi='%s', key=%s\n", 
            udi, key);
    print_property(udi, key);
    fprintf(stderr, "\n");
    /*dump_devices();*/
}

static void property_removed(const char* udi, const char* key)
{
    fprintf(stderr, "*** lshal: property_removed, udi='%s', key=%s\n", 
            udi, key);
    fprintf(stderr, "\n");
    /*dump_devices();*/
}

static void mainloop_integration(DBusConnection* dbus_connection)
{
    dbus_connection_setup_with_g_main(dbus_connection, NULL);
}


static void usage(int argc, char* argv[])
{
    fprintf(stderr, 
"\n"
"usage : %s --monitor [--help]\n", argv[0]);
    fprintf(stderr, 
"\n"
"        --monitor        Monitor device list\n"
"        --help           Show this information and exit\n"
"\n"
"Shows all devices and their properties. If the --monitor option is given\n"
"then the device list is also monitored for changes.\n"
"\n");
}

int main(int argc, char* argv[])
{
    dbus_bool_t do_monitor = FALSE;
    GMainLoop* loop;
    LibHalFunctions hal_functions = { mainloop_integration,
                                      device_added, 
                                      device_removed, 
                                      device_new_capability,
                                      property_changed,
                                      property_added,
                                      property_removed };

    fprintf(stderr, "lshal version " PACKAGE_VERSION "\n");

    loop = g_main_loop_new (NULL, FALSE);

    while(1)
    {
        int c;
        int option_index = 0;
        const char* opt;
        static struct option long_options[] = 
        {
            {"monitor", 0, NULL, 0},
            {"help", 0, NULL, 0},
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
                usage(argc, argv);
                return 0;
            }
            else if( strcmp(opt, "monitor")==0 )
            {
                do_monitor = TRUE;
            }
            break;        

        default:
            usage(argc, argv);
            return 1;
            break;
        }         
    }


    if( hal_initialize(&hal_functions)  )
    {
        fprintf(stderr, "error: hal_initialize failed\n");
        exit(1);
    }

    dump_devices();    

    /* run the main loop only if we should monitor */
    if( do_monitor )
    {
        hal_device_property_watch_all();
        g_main_loop_run(loop);
    }

    hal_shutdown();
    return 0;
}
