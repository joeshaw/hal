/***************************************************************************
 * 
 *
 * addon-dell-backlight.cpp : Sets the backlight for Dell laptops using the libsmbios interface
 * 
 * Copyright (C) 2007 Erik Andrén <erik.andren@gmail.com>
 * Heavily based on the macbook addon and the dellLcdBrightness code in libsmbios. 
 * This program needs the dcdbas module to be loaded and libsmbios >= 0.12.1 installed
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
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

#include <string.h>

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../../logger.h"

#include "smbios/ISmi.h"
#include "smbios/IToken.h"

static LibHalContext *halctx = NULL;
static GMainLoop *main_loop;
static char *udi;
static DBusConnection *conn;
static const int DELL_LCD_BRIGHTNESS_TOKEN = 0x007d;

static u32 minValue = 0;
static u32 maxValue = 0;

#ifdef HAVE_SMBIOS_2_0_3
static bool hasPass = false;
#endif

using namespace std;
using namespace smi;

typedef u32 (*readfn)(u32 location, u32 *minValue, u32 *maxValue);
typedef u32 (*writefn)(const string &password, u32 location, u32 value, u32 *minValue, u32 *maxValue);

static u32 
read_backlight (dbus_bool_t onAC)
{
	u8  location = 0;
	u32 curValue;
	readfn readFunction;
	
	if (onAC) 
                readFunction = &smi::readACModeSetting;
    	else 
                readFunction = &smi::readBatteryModeSetting;
	
	smbios::TokenTableFactory *ttFactory = smbios::TokenTableFactory::getFactory () ;
        smbios::ITokenTable *tokenTable = ttFactory->getSingleton ();
        smbios::IToken *token = &(*((*tokenTable)[ DELL_LCD_BRIGHTNESS_TOKEN ]));
        dynamic_cast< smbios::ISmiToken * >(token)->getSmiDetails (static_cast<u16*>(0), static_cast<u8*>(0), &location);

	try 
	{ 
        	curValue = readFunction (location, &minValue, &maxValue);
	}
	catch (const exception &e) 
	{
        	HAL_ERROR (("Could not access the dcdbas kernel module. Please make sure it is loaded"));
		return maxValue;
    	}

	if (onAC) 
		HAL_DEBUG (("Reading %d from the AC backlight register", curValue));	
	else 
		HAL_DEBUG (("Reading %d from the BAT backlight register", curValue));

	return curValue;
}

#ifdef HAVE_SMBIOS_2_0_3
static void
check_bios_password()
{
	if (smi::getPasswordStatus((u16)9) || smi::getPasswordStatus((u16)10))
		hasPass=true;
}
#endif

static void 
write_backlight (u32 newBacklightValue, dbus_bool_t onAC) 
{	
	u8  location = 0;
	u32 curValue;
	writefn writeFunction;
	string password(""); /* FIXME: Implement password support */

#ifdef HAVE_SMBIOS_2_0_3
	if (hasPass) {
		HAL_WARNING(("Setting brightness via SMI is not supported with a BIOS password"));
		return;
	}
#endif
	
	if (onAC) 
                writeFunction = &smi::writeACModeSetting;
    	else 
                writeFunction = &smi::writeBatteryModeSetting;

	smbios::TokenTableFactory *ttFactory = smbios::TokenTableFactory::getFactory ();
        smbios::ITokenTable *tokenTable = ttFactory->getSingleton ();
        smbios::IToken *token = &(*((*tokenTable)[ DELL_LCD_BRIGHTNESS_TOKEN ]));
        dynamic_cast< smbios::ISmiToken * >(token)->getSmiDetails ( static_cast<u16*>(0), static_cast<u8*>(0), &location );

	try 
	{
		curValue = writeFunction(password, location, newBacklightValue, &minValue, &maxValue); 
	}
	catch (const exception &e)
    	{
        	HAL_ERROR (("Could not access the dcdbas kernel module. Please make sure it is loaded"));
		return;
    	}

	if (onAC)
		HAL_DEBUG (("Wrote %d to the AC backlight", curValue));
	else
		HAL_DEBUG (("Wrote %d to the BAT backlight", curValue));
}

static gboolean
check_priv (DBusConnection *connection, DBusMessage *message, const char *udi, const char *action)
#ifdef HAVE_POLKIT
{
        gboolean ret;
        char *polkit_result;
        const char *invoked_by_syscon_name;
        DBusMessage *reply;
        DBusError error;

        ret = FALSE;
        polkit_result = NULL;

        invoked_by_syscon_name = dbus_message_get_sender (message);
        
        dbus_error_init (&error);
        polkit_result = libhal_device_is_caller_privileged (halctx,
                                                            udi,
                                                            action,
                                                            invoked_by_syscon_name,
                                                            &error);
        if (polkit_result == NULL) {
                reply = dbus_message_new_error_printf (message,
                                                       "org.freedesktop.Hal.Device.Error",
                                                       "Cannot determine if caller is privileged",
                                                       action, polkit_result);
                dbus_connection_send (connection, reply, NULL);
                goto out;
        }
        if (strcmp (polkit_result, "yes") != 0) {

                reply = dbus_message_new_error_printf (message,
                                                       "org.freedesktop.Hal.Device.PermissionDeniedByPolicy",
                                                       "%s %s <-- (action, result)",
                                                       action, polkit_result);
                dbus_connection_send (connection, reply, NULL);
                goto out;
        }

        ret = TRUE;

out:
	LIBHAL_FREE_DBUS_ERROR (&error);
        if (polkit_result != NULL)
                libhal_free_string (polkit_result);
        return ret;
}
#else
{
        return TRUE;
}
#endif

static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply = NULL;
	dbus_bool_t AC;
	char **udis;
	int i, num_devices;

	dbus_error_init (&err);

        if (!check_priv (connection, message, dbus_message_get_path (message), "org.freedesktop.hal.power-management.lcd-panel")) {
                return DBUS_HANDLER_RESULT_HANDLED;
        }
	
	/* set a default */
	AC = TRUE; 
	/* Mechanism to ensure that we always set the AC brightness when we are on AC-power etc. */
	if ((udis = libhal_find_device_by_capability(halctx, "ac_adapter", &num_devices, &err)) != NULL) {
		for (i = 0; udis[i] != NULL; i++) {
			LIBHAL_FREE_DBUS_ERROR (&err);

			if (libhal_device_property_exists (halctx, udis[i], "ac_adapter.present", &err)) {
				AC = libhal_device_get_property_bool (halctx, udis[i], "ac_adapter.present", &err);
				break; /* we found one AC device, leave the for-loop */
			}
		}
		libhal_free_string_array(udis);
	}

	if (dbus_message_is_method_call (message, 
					 "org.freedesktop.Hal.Device.LaptopPanel", 
					 "SetBrightness")) {
		int brightness;
		
		HAL_DEBUG (("Received SetBrightness DBus call"));

		LIBHAL_FREE_DBUS_ERROR (&err);

		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			if (brightness < (int) minValue || brightness > (int) maxValue) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.LaptopPanel.Invalid",
								"Brightness level is invalid");

			} else {
				int return_code;
				
				write_backlight (brightness, AC);

				reply = dbus_message_new_method_return (message);
				if (reply == NULL)
					goto error;

				return_code = 0;

				dbus_message_append_args (reply,
							  DBUS_TYPE_INT32, &return_code,
							  DBUS_TYPE_INVALID);
			}

			dbus_connection_send (connection, reply, NULL);
		}
		
	} else if (dbus_message_is_method_call (message, 
						"org.freedesktop.Hal.Device.LaptopPanel", 
						"GetBrightness")) {
		HAL_DEBUG (("Received GetBrightness DBUS call"));
		
		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INVALID)) {
			int brightness = read_backlight (AC);

			if (brightness < (int) minValue)
				brightness = minValue;
			else if (brightness > (int) maxValue)
				brightness = maxValue;

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
	}	
error:
	LIBHAL_FREE_DBUS_ERROR (&err);

	if (reply != NULL)
		dbus_message_unref (reply);
	
	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
 	DBusError err;
	int retval = 0;

	setup_logger ();

	udi = getenv ("UDI");

	HAL_DEBUG (("udi=%s", udi));
	if (udi == NULL) {
		HAL_ERROR (("No device specified"));
		retval = -2;
		goto out;
	}

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		HAL_ERROR (("Cannot connect to hald"));
		retval = -3;
		goto out;
	}

	if (!libhal_device_addon_is_ready(halctx, udi, &err)) {
		retval = -4;
		goto out;
	}

	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

	read_backlight (TRUE); /* Fill min- & maxValue with the correct values */

#ifdef HAVE_SMBIOS_2_0_3
	check_bios_password(); /* Find out about our BIOS pass capabilities */
#endif

	if (maxValue == 0) {
		HAL_ERROR (("This machine don't support set brightness."));
		retval = -5;
		goto out;
	}
	
	if (!libhal_device_set_property_int (halctx, 
					    "/org/freedesktop/Hal/devices/dell_lcd_panel",
					    "laptop_panel.num_levels",
					    maxValue + 1,
					    &err)) {
		HAL_ERROR (("Could not set 'laptop_panel.numlevels' to %d", maxValue));
		retval = -4;
		goto out;
	}
	HAL_DEBUG (("laptop_panel.numlevels is set to %d", maxValue + 1));

	/* this works because we hardcoded the udi's in the <spawn> in the fdi files */
	if (!libhal_device_claim_interface (halctx, 
					    "/org/freedesktop/Hal/devices/dell_lcd_panel", 
					    "org.freedesktop.Hal.Device.LaptopPanel", 
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.LaptopPanel'"));
		retval = -4;
		goto out;
	}
	
	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	return 0;

out: 

	LIBHAL_FREE_DBUS_ERROR (&err);

	if (halctx != NULL) {
		libhal_ctx_shutdown (halctx, &err);
		LIBHAL_FREE_DBUS_ERROR (&err);
		libhal_ctx_free (halctx);
	}

	return retval;
}


