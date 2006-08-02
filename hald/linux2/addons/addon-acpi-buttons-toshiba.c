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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <glib/gmain.h>

#include "libhal/libhal.h"
#include "../probing/shared.h"

/** Toshiba ACPI key interface */
#define TOSHIBA_ACPI_KEYS		"/proc/acpi/toshiba/keys"

/** Polling frequency in ms */
#define TOSHIBA_POLL_FREQ       	250

static LibHalContext *ctx = NULL;
static char* udi;

/** Flush keys from the Toshiba hotkey register */
static void
toshiba_key_flush (void)
{
	int hotkey_ready = 1;
	int value;
	FILE *fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
	if (!fp) {
		dbg ("Could not open %s!", TOSHIBA_ACPI_KEYS);
		return;
	}
	while (hotkey_ready) {
		fprintf (fp, "hotkey_ready:0\n");
		fclose (fp);
		fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
		fscanf (fp, "hotkey_ready: %d\nhotkey: 0x%4x",
			&hotkey_ready, &value);
	}
	if (fp)
		fclose (fp);
}

/** Check whether there is a new event in the hotkey register
 *
 *  @param	value	The key id pressed, passed by reference
 *  @returns		TRUE if there is an event pending, FALSE if no event pending.
 */
static gboolean
toshiba_key_ready (int *value)
{
	FILE *fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
	int hotkey_ready = -1;

	if (!fp)
		return FALSE;

	fscanf (fp, "hotkey_ready: %1d\nhotkey: 0x%4x",
		&hotkey_ready, value);

	if (hotkey_ready) {
		fprintf (fp, "hotkey_ready:0\n");
		fclose (fp);
		return TRUE;
	}
	fclose (fp);
	return FALSE;
}

/** Callback to poll hotkey register and report occuring events.
 *
 *  @returns		TRUE on success, else FALSE.
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
			dbg ("Sending condition '%s'", result);
			libhal_device_emit_condition (ctx, udi, "ButtonPressed", result, &error);
			if (dbus_error_is_set (&error)) {
				dbg ("Failed to send condition: %s", error.message);
				dbus_error_free (&error);
			}
		}
	}
	return TRUE;
}

/** Main program
 */
int
main (int argc, char **argv)
{
	GMainLoop *loop = NULL;
	DBusError error;
	FILE *fp;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;
	udi = getenv ("UDI");
	if (udi == NULL) {
		dbg ("Failed to get UDI");
		return 1;
	}
	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		dbg ("Unable to initialise libhal context: %s", error.message);
		return 1;
	}

	/* Check for Toshiba ACPI interface /proc/acpi/toshiba/keys */
	fp = fopen (TOSHIBA_ACPI_KEYS, "r+");
	if (!fp) {
		dbg ("Could not open %s! Aborting.", TOSHIBA_ACPI_KEYS);
		return 0;
	}
	fclose (fp);

	/* Flush keys as we may have some already in buffer */
	toshiba_key_flush ();

	/* Get the new input */
	g_timeout_add (TOSHIBA_POLL_FREQ, (GSourceFunc) toshiba_key_poll, NULL);

	loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (loop);
	return 0;
}
