/*! @file	addon-acpi-buttons-toshiba.c
 *  @brief	Toshiba SMM Button Addon
 *  @author	Richard Hughes <richard@hughsie.com>
 *  @date	2005-10-15
 *
 *  @note	Low level routines from IAL, Copyright (C) 2004, 2005
 *		Timo Hoenig <thoenig@nouse.net>
 */
/*
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#ifdef HAL_LINUX_INPUT_HEADER_H
 #include HAL_LINUX_INPUT_HEADER_H
else
 #include <linux/input.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib/gmain.h>

#include "libhal/libhal.h"
#include "../../logger.h"

/** Toshiba ACPI key interface */
#define TOSHIBA_ACPI_KEYS		"/proc/acpi/toshiba/keys"

/** Polling frequency in ms */
#define TOSHIBA_POLL_FREQ       	250

static LibHalContext *ctx = NULL;
static char* udi;

/** 
 * toshiba_key_flush:
 * 
 * Flush keys from the Toshiba hotkey register 
 */
static void
toshiba_key_flush (void)
{
	int hotkey_ready = 1;
	int value;
	FILE *fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
	if (!fp) {
		HAL_DEBUG (("Could not open %s!", TOSHIBA_ACPI_KEYS));
		return;
	}
	while (hotkey_ready) {
		fprintf (fp, "hotkey_ready:0\n");
		fclose (fp);
		fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
		if (fscanf (fp, "hotkey_ready: %d\nhotkey: 0x%4x", &hotkey_ready, &value) < 2)
			HAL_WARNING(("Warning: failure while parse %s", TOSHIBA_ACPI_KEYS));
	}
	if (fp)
		fclose (fp);
}

/** 
 *  toshiba_key_ready:
 *  @value:	The key id pressed, passed by reference
 * 
 *  Returns:	TRUE if there is an event pending, FALSE if no event pending.
 *
 *  Check whether there is a new event in the hotkey register
 */
static gboolean
toshiba_key_ready (int *value)
{
	FILE *fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
	int hotkey_ready = -1;

	if (!fp)
		return FALSE;

	if (fscanf (fp, "hotkey_ready: %1d\nhotkey: 0x%4x", &hotkey_ready, value) < 2)
		HAL_WARNING (("Warning: failure while parse %s", TOSHIBA_ACPI_KEYS)); 

	if (hotkey_ready) {
		fprintf (fp, "hotkey_ready:0\n");
		fclose (fp);
		return TRUE;
	}
	fclose (fp);
	return FALSE;
}

/** 
 *  toshiba_key_poll:
 *
 *  Returns:		TRUE on success, else FALSE.
 * 
 *  Callback to poll hotkey register and report occuring events. 
 */
static gboolean
toshiba_key_poll (void)
{
	char *result;
	int value;
	DBusError error;
	dbus_error_init (&error);

	/* for each key */
	while (toshiba_key_ready (&value) == TRUE) {
		result = NULL;
		if (value == 0x101) /* FnESC */
			result = "mute";
		else if (value == 0x13b) /* FnF1 */
			result = "lock";
		else if (value == 0x13c) /* FnF2 */
			result = "search";
		else if (value == 0x13d) /* FnF3 */
			result = "suspend";
		else if (value == 0x13e) /* FnF4 */
			result = "hibernate";
		else if (value == 0x140) /* FnF6 */
			result = "brightness-down";
		else if (value == 0x141) /* FnF7 */
			result = "brightness-up";
		else if (value == 0x142) /* FnF8 */
			result = "wifi-power";

		if (result) {
			HAL_DEBUG (("Sending condition '%s'", result));
			libhal_device_emit_condition (ctx, udi, "ButtonPressed", result, &error);
			if (dbus_error_is_set (&error)) {
				HAL_ERROR (("Failed to send condition: %s", error.message));
				dbus_error_free (&error);
				return FALSE;
			}
		}
	}
	return TRUE;
}

/* Main program */
int
main (int argc, char **argv)
{
	GMainLoop *loop = NULL;
	DBusError error;
	FILE *fp;

	setup_logger ();

	udi = getenv ("UDI");
	if (udi == NULL) {
		HAL_ERROR (("Failed to get UDI"));
		return 1;
	}
	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		HAL_ERROR (("Unable to initialise libhal context: %s", error.message));
		LIBHAL_FREE_DBUS_ERROR (&error);
		return 1;
	}

	if (!libhal_device_addon_is_ready (ctx, udi, &error)) {
		goto out;	
	}

	/* Check for Toshiba ACPI interface /proc/acpi/toshiba/keys */
	fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
	if (!fp) {
		HAL_ERROR (("Could not open %s! Aborting.", TOSHIBA_ACPI_KEYS));
		goto out;
	}
	fclose (fp);

	/* Flush keys as we may have some already in buffer */
	toshiba_key_flush ();

	/* Get the new input */
	g_timeout_add (TOSHIBA_POLL_FREQ, (GSourceFunc) toshiba_key_poll, NULL);

	loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (loop);
	return 0;

out:
        HAL_DEBUG (("An error occured, exiting cleanly"));

	LIBHAL_FREE_DBUS_ERROR (&error);

        if (ctx != NULL) {
                libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
                libhal_ctx_free (ctx);
        }

	return 0;
}
