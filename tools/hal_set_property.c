/***************************************************************************
 * CVSID: $Id$
 *
 * hal_set_property.c : Set property for a device
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

#include <libhal/libhal.h>

/**
 * @defgroup HalSetProperty  Set HAL device property
 * @ingroup HalMisc
 *
 * @brief A commandline tool setting a property of a device. Uses libhal
 *
 * @{
 */

/** Print out program usage.
 *
 *  @param  argc                Number of arguments given to program
 *  @param  argv                Arguments given to program
 */
static void usage(int argc, char* argv[])
{
    fprintf(stderr, 
"\n"
"usage : %s --udi <udi> --key <key>\n"
"           (--int <value> | --string <value> | --bool <value> |"
"            --double <value> | --remove) [--help]\n", argv[0]);
    fprintf(stderr, 
"\n"
"        --udi            Unique Device Id\n"
"        --key            Key of the property to set\n"
"        --int            Set value to an integer. Accepts decimal and "
"                         hexadecimal prefixed with 0x or x\n"
"        --string         Set value to a string\n"
"        --double         Set value to a floating point number\n"
"        --bool           Set value to a boolean, ie. true or false\n"
"        --remove         Indicates that the property should be removed\n"
"        --help           Show this information and exit\n"
"\n"
"This program attempts to set property for a device. Note that, due to\n"
"security considerations, it may not be possible to set a property; on\n"
"success this program exits with exit code 0. On error, the program exits\n"
"with an exit code different from 0\n"
"\n");
}

/** Entry point
 *
 *  @param  argc                Number of arguments given to program
 *  @param  argv                Arguments given to program
 *  @return                     Return code
 */
int main(int argc, char* argv[])
{
    int rc = 0;
    char* udi = NULL;
    char* key = NULL;
    char* str_value = NULL;
    dbus_int32_t int_value = 0;
    double double_value = 0.0f;
    dbus_bool_t bool_value = TRUE;
    dbus_bool_t remove = FALSE;
    int type = DBUS_TYPE_NIL;

    fprintf(stderr, "%s " PACKAGE_VERSION "\n", argv[0]);

    if( argc<=1 )
    {
        usage(argc, argv);
        return 1;
    }

    while(1)
    {
        int c;
        int option_index = 0;
        const char* opt;
        static struct option long_options[] = 
        {
            {"udi", 1, NULL, 0},
            {"key", 1, NULL, 0},
            {"int", 1, NULL, 0},
            {"string", 1, NULL, 0},
            {"double", 1, NULL, 0},
            {"bool", 1, NULL, 0},
            {"remove", 0, NULL, 0},
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
            else if( strcmp(opt, "key")==0 )
            {
                key = strdup(optarg);
            }
            else if( strcmp(opt, "string")==0 )
            {
                str_value = strdup(optarg);
                type = DBUS_TYPE_STRING;
            }
            else if( strcmp(opt, "int")==0 )
            {
                int_value = strtol(optarg, NULL, 0);
                type = DBUS_TYPE_INT32;
            }
            else if( strcmp(opt, "double")==0 )
            {
                double_value = (double) atof(optarg);
                type = DBUS_TYPE_DOUBLE;
            }
            else if( strcmp(opt, "bool")==0 )
            {
                if( strcmp(optarg, "true")==0 )
                    bool_value = TRUE;
                else if( strcmp(optarg, "false")==0 )
                    bool_value = FALSE;
                else
                {
                    usage(argc, argv);
                    return 1;
                }
                type = DBUS_TYPE_BOOLEAN;
            }
            else if( strcmp(opt, "remove")==0 )
            {
                remove = TRUE;
            }
            else if( strcmp(opt, "udi")==0 )
            {
                udi = strdup(optarg);
            }
            break;        

        default:
            usage(argc, argv);
            return 1;
            break;
        }         
    }

    /* must have at least one, but not neither or both */
    if( (remove && type!=DBUS_TYPE_NIL) || 
        ((!remove) && type==DBUS_TYPE_NIL ) )
    {
        usage(argc, argv);
        return 1;
    }

    fprintf(stderr, "\n");

    if( hal_initialize(NULL, FALSE)  )
    {
        fprintf(stderr, "error: hal_initialize failed\n");
        return 1;
    }

    if( remove )
    {
        rc = hal_device_remove_property(udi, key);
        if( rc!=0 )
            return 1;
    }
    else
    {
        switch( type )
        {
        case DBUS_TYPE_STRING:
            rc = hal_device_set_property_string(udi, key, str_value);
            break;
        case DBUS_TYPE_INT32:
            rc = hal_device_set_property_int(udi, key, int_value);
            break;
        case DBUS_TYPE_DOUBLE:
            rc = hal_device_set_property_double(udi, key, double_value);
            break;
        case DBUS_TYPE_BOOLEAN:
            rc = hal_device_set_property_bool(udi, key, bool_value);
            break;
        }
        if( rc!=0 )
            return 1;
    }

    return 0;
}

/**
 * @}
 */
