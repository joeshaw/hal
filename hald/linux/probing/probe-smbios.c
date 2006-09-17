/***************************************************************************
 * CVSID: $Id$
 *
 * probe-smbios.c : Probe system BIOS according to the SMBIOS/DMI standard
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Richard Hughes, <richard@hughsie.com>
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

#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhal/libhal.h"
#include "../../logger.h"

#define DMIPARSER_STATE_IGNORE		0
#define DMIPARSER_STATE_BIOS		1
#define DMIPARSER_STATE_SYSTEM		2
#define DMIPARSER_STATE_CHASSIS		3

#define strbegin(buf, str) (strncmp (buf, str, strlen (str)) == 0)

/* global */
char *udi = NULL;
LibHalContext *ctx = NULL;

/** Finds the start of a null terminated string and sets HAL
 *  property if valid.
 *
 *  @param	buf		The non tabbed prefixed, null terminated string
 *  @param	str		The strings to compare with e.g. "Vendor:"
 *  @param	prop		The HAL property to set
 *  @return			TRUE is found, FALSE otherwise.
 */
static int
setstr (char *buf, char *str, char *prop)
{
	DBusError error;
	char *value;

	if (strbegin (buf, str)) {
		dbus_error_init (&error);
		value = buf + strlen (str) + 1;
		libhal_device_set_property_string (ctx, udi, prop, value, &error);
		HAL_DEBUG (("Setting %s='%s'", prop, value));
		return TRUE;
	}
	return FALSE;
}

/** Main entry point
 *
 *  @param	argc		Number of arguments given to program
 *  @param	argv		Arguments given to program
 *  @return			Return code
 */
int 
main (int argc, char *argv[])
{
	int ret;
	DBusError error;
	char buf[512];
	char *nbuf;
	int dmipipe[2];
	int nullfd;
	int tmp_ret;
	FILE *f;
	int dmiparser_state = DMIPARSER_STATE_IGNORE;

	/* on some system chassis pops up several times,
	 * so only take the first entry for each
	 */
	int dmiparser_done_bios = FALSE;
	int dmiparser_done_system = FALSE;
	int dmiparser_done_chassis = FALSE;

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

	tmp_ret = pipe (dmipipe);	
	f = fdopen (dmipipe[0], "r");
	nullfd = open ("/dev/null", O_RDONLY);
	
	/* fork the child process */
	switch (fork ()) {
	case 0:
		/* child */
		
		dup2 (nullfd, STDIN_FILENO);
		dup2 (dmipipe[1], STDOUT_FILENO);
		close (dmipipe[0]);
		close (dmipipe[1]);
		
		/* execute the child */
		execl ("/usr/sbin/dmidecode", "/usr/sbin/dmidecode", NULL);
		
		/* throw an error if we ever reach this point */
		HAL_ERROR (("Failed to execute dmidecode!"));
		exit (1);
		break;
	case -1:
		HAL_ERROR (("Cannot fork!"));
		goto out;
	}
	
	/* parent continues from here */
	
	/* close unused descriptor */
	close (dmipipe[1]);
	
	/* read the output of the child */
	while(fgets (buf, sizeof(buf), f) != NULL)
	{
		unsigned int i;
		unsigned int len;
		unsigned int tabs = 0;

		/* trim whitespace */
		len = strlen (buf);

		/* check that will fit in buffer */
		if (len >= sizeof (buf))
			continue;

		/* not big enough for data, and protects us from underflow */
		if (len < 3) {
			dmiparser_state = DMIPARSER_STATE_IGNORE;
			continue;
		}

		/* find out number of leading tabs */
		if (buf[0] == '\t' && buf[1] == '\t')
			tabs = 2; /* this is list data */
		else if (buf[0] == '\t')
			tabs = 1; /* this is data, 0 is section type */

		if (tabs == 2)
			/* we do not proccess data at depth 2 */
			continue;
			
		/* set the section type */
		if (tabs == 0) {
			if (!dmiparser_done_bios && strbegin (buf, "BIOS Information"))
				dmiparser_state = DMIPARSER_STATE_BIOS;
			else if (!dmiparser_done_system && strbegin (buf, "System Information"))
				dmiparser_state = DMIPARSER_STATE_SYSTEM;
			else if (!dmiparser_done_chassis && strbegin (buf, "Chassis Information"))
				dmiparser_state = DMIPARSER_STATE_CHASSIS;
			else
				/*
				 * We do not match the other sections,
				 * or sections we have processed before
				 */
				dmiparser_state = DMIPARSER_STATE_IGNORE;
			continue; /* next line */
		}

		/* we are not in a section we know, no point continueing */
		if (dmiparser_state == DMIPARSER_STATE_IGNORE)
			continue;

		/* return success only if there was something usefull to parse */
		ret = 0;


		/* removes the leading tab */
		nbuf = &buf[1];

		/* removes the trailing spaces */
		for (i = len - 2; isspace (nbuf[i]) && i >= 0; --i)
			nbuf[i] = '\0';

		if (dmiparser_state == DMIPARSER_STATE_BIOS) {
			setstr (nbuf, "Vendor:", "smbios.bios.vendor");
			setstr (nbuf, "Version:", "smbios.bios.version");
			setstr (nbuf, "Release Date:", "smbios.bios.release_date");
			dmiparser_done_bios = TRUE;
		} else if (dmiparser_state == DMIPARSER_STATE_SYSTEM) {
			setstr (nbuf, "Manufacturer:", "smbios.system.manufacturer");
			setstr (nbuf, "Product Name:", "smbios.system.product");
			setstr (nbuf, "Version:", "smbios.system.version");
			setstr (nbuf, "Serial Number:", "smbios.system.serial");
			setstr (nbuf, "UUID:", "smbios.system.uuid");
			dmiparser_done_system = TRUE;
		} else if (dmiparser_state == DMIPARSER_STATE_CHASSIS) {
			setstr (nbuf, "Manufacturer:", "smbios.chassis.manufacturer");
			setstr (nbuf, "Type:", "smbios.chassis.type");
			dmiparser_done_chassis = TRUE;
		}
	}

	/* as read to EOF, close */
	fclose (f);

out:
	/* free ctx */
	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
