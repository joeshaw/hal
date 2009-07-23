/***************************************************************************
 * CVSID: $Id$
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.1
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib.h>

#include "libhal/libhal.h"
#include "../../logger.h"

/* Stolen from kernel 2.6.4, drivers/usb/class/usblp.c */
#define IOCNR_GET_DEVICE_ID 1
#define LPIOC_GET_DEVICE_ID(len) _IOC(_IOC_READ, 'P', IOCNR_GET_DEVICE_ID, len)

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	char device_id[1024];
	char **props;
	char **iter;
	char *mfg;
	char *model;
	char *serial;
	char *desc;
	char *cmd;

	fd = -1;

	/* assume failure */
	ret = 1;

	setup_logger ();
	
	udi = getenv ("UDI");
	if (udi == NULL) {
		HAL_ERROR (("UDI not set"));	
		goto out;
	}

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		HAL_ERROR (("ctx init failed"));
		goto out;
	}

	device_file = getenv ("HAL_PROP_PRINTER_DEVICE");
	if (device_file == NULL) {
		HAL_ERROR (("device_file == NULL"));
		goto out;
	}

	fd = open (device_file, O_RDONLY);
	if (fd < 0) {
		HAL_ERROR (("Cannot open %s", device_file));
		goto out;
	}

	if (ioctl (fd, LPIOC_GET_DEVICE_ID (sizeof (device_id)), device_id) < 0) {
		HAL_ERROR (("Cannot do LPIOC_GET_DEVICE_ID on %s", device_file));
		goto out;
	} else

	mfg = NULL;
	model = NULL;
	serial = NULL;
	desc = NULL;
	cmd = NULL;

	HAL_DEBUG (("device_id = %s", device_id + 2));

	props = g_strsplit (device_id+2, ";", 0);
	for (iter = props; *iter != NULL; iter++) {
		if (strncmp (*iter, "MANUFACTURER:", 13) == 0)
			mfg = *iter + 13;
		else if (strncmp (*iter, "MFG:", 4) == 0)
			mfg = *iter + 4;
		else if (strncmp (*iter, "MODEL:", 6) == 0)
			model = *iter + 6;
		else if (strncmp (*iter, "MDL:", 4) == 0)
			model = *iter + 4;
		else if (strncmp (*iter, "SN:", 3) == 0)
			serial = *iter + 3;
		else if (strncmp (*iter, "SERN:", 5) == 0)
			serial = *iter + 5;
		else if (strncmp (*iter, "SERIALNUMBER:", 13) == 0)
			serial = *iter + 13;
		else if (strncmp (*iter, "DES:", 4) == 0)
			desc = *iter + 4;
		else if (strncmp (*iter, "DESCRIPTION:", 12) == 0)
			desc = *iter + 12;
		else if (strncmp (*iter, "COMMAND SET:", 12) == 0)
			cmd = *iter + 12;
		else if (strncmp (*iter, "COMMANDSET:", 11) == 0)
			cmd = *iter + 11;
		else if (strncmp (*iter, "CMD:", 4) == 0)
			cmd = *iter + 4;
	}

	if (mfg != NULL) {
		libhal_device_set_property_string (ctx, udi, "info.vendor", mfg, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_device_set_property_string (ctx, udi, "printer.vendor", mfg, &error);
	}		

	if (model != NULL) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_device_set_property_string (ctx, udi, "info.product", model, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_device_set_property_string (ctx, udi, "printer.product", model, &error);
	}

	if (serial != NULL) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_device_set_property_string (ctx, udi, "printer.serial", serial, &error);
	}

	if (desc != NULL) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_device_set_property_string (ctx, udi, "printer.description", desc, &error);
	}

	if (cmd != NULL) {
		char **cmdset = g_strsplit (cmd, ",", 0);
		for (iter = cmdset; *iter != NULL; iter++) {
			LIBHAL_FREE_DBUS_ERROR (&error);
			libhal_device_property_strlist_append (ctx, udi, "printer.commandset", *iter, &error);
		}
		g_strfreev (cmdset);
	}

	g_strfreev (props);

	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	LIBHAL_FREE_DBUS_ERROR (&error);

	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
