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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "../libprobe/hfp.h"

#define DMIDECODE			"/usr/local/sbin/dmidecode"

#define DMIPARSER_STATE_IGNORE		0
#define DMIPARSER_STATE_BIOS		1
#define DMIPARSER_STATE_SYSTEM		2
#define DMIPARSER_STATE_CHASSIS		3

#define strbegin(buf, str) (strncmp (buf, str, strlen (str)) == 0)

/**  
 *  setstr:
 *  @buf:		The non tabbed prefixed, null terminated string
 *  @str:		The strings to compare with e.g. "Vendor:"
 *  @prop:		The HAL property to set
 *
 *  Return		TRUE is found, FALSE otherwise.
 *
 *  Finds the start of a null terminated string and sets HAL property if valid.
 */
static int
setstr (char *buf, char *str, char *prop)
{
	DBusError error;
	char *value;

	if (strbegin (buf, str)) {
		dbus_error_init (&error);
		value = buf + strlen (str) + 1;
		if (strcmp (value, "Not Specified") == 0)
			goto out;

		libhal_device_set_property_string (hfp_ctx, hfp_udi, prop, value, &hfp_error);
		LIBHAL_FREE_DBUS_ERROR (&hfp_error);
		hfp_info ("Setting %s='%s'", prop, value);
		return TRUE;
	}
out:
	return FALSE;
}

static void
copykeyval (char *key, char *compat_key)
{
	char *value;

	value = libhal_device_get_property_string (hfp_ctx, hfp_udi, key, NULL);
	if (value != NULL) {
		hfp_info ("Copying %s -> %s", key, compat_key);
		libhal_device_set_property_string (hfp_ctx, hfp_udi, compat_key, value, NULL);
	}
}

/** 
 *  main:
 *  @argc:	Number of arguments given to program
 *  @argv:	Arguments given to program
 *  Returns:	Return code
 *
 *  Main entry point
 */
int
main (int argc, char *argv[])
{
	int ret;
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

	if (! hfp_init (argc, argv))
	  goto out;

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
		execl (DMIDECODE, DMIDECODE, NULL);

		/* throw an error if we ever reach this point */
		hfp_warning("failed to execute " DMIDECODE);
		exit (1);
		break;
	case -1:
		hfp_warning("cannot fork");
		break;
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

		/* removes the leading tab */
		nbuf = &buf[1];

		/* removes the trailing spaces */
		for (i = len - 2; isspace (nbuf[i]) && i >= 0; --i)
			nbuf[i] = '\0';

		if (dmiparser_state == DMIPARSER_STATE_BIOS) {
			setstr (nbuf, "Vendor:", "system.firmware.vendor");
			setstr (nbuf, "Version:", "system.firmware.version");
			setstr (nbuf, "Release Date:", "system.firmware.release_date");
			dmiparser_done_bios = TRUE;
		} else if (dmiparser_state == DMIPARSER_STATE_SYSTEM) {
			setstr (nbuf, "Manufacturer:", "system.hardware.vendor");
			setstr (nbuf, "Product Name:", "system.hardware.product");
			setstr (nbuf, "Version:", "system.hardware.version");
			setstr (nbuf, "Serial Number:", "system.hardware.serial");
			setstr (nbuf, "UUID:", "system.hardware.uuid");
			dmiparser_done_system = TRUE;
		} else if (dmiparser_state == DMIPARSER_STATE_CHASSIS) {
			setstr (nbuf, "Manufacturer:", "system.chassis.manufacturer");
			setstr (nbuf, "Type:", "system.chassis.type");
			dmiparser_done_chassis = TRUE;
		}
	}

	/* as read to EOF, close */
	fclose (f);

	/* return success */
	ret = 0;

out:
	return ret;
}
