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
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libhal/libhal.h"
#include "../../logger.h"

#define DMIPARSER_STATE_IGNORE		0
#define DMIPARSER_STATE_BIOS		1
#define DMIPARSER_STATE_SYSTEM		2
#define DMIPARSER_STATE_CHASSIS		3
#define DMIPARSER_STATE_BOARD		4

#define strbegin(buf, str) (strncmp (buf, str, strlen (str)) == 0)

/* global */
static char *udi = NULL;
static LibHalContext *ctx = NULL;

/** 
 *  setstr:
 *  @buf:		The non tabbed prefixed, null terminated string
 *  @str:		The strings to compare with e.g. "Vendor:"
 *  @prop:		The HAL property to set
 *
 *  Returns:		TRUE is found, FALSE otherwise.
 *
 *  Finds the start of a null terminated string and sets HAL
 *  property if valid.
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

		if (!libhal_device_set_property_string (ctx, udi, prop, value, &error))
			dbus_error_init (&error);

		HAL_DEBUG (("Setting %s='%s'", prop, value));
		return TRUE;
	}
out:
	return FALSE;
}


/** 
 *  main:
 *  @argc:	Number of arguments given to program
 *  @argv:	Arguments given to program
 *
 *  Returns: 	Return code
 * 
 *  Main entry point
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
	FILE *f;
	int dmiparser_state = DMIPARSER_STATE_IGNORE;

	/* on some system chassis pops up several times,
	 * so only take the first entry for each
	 */
	int dmiparser_done_bios = FALSE;
	int dmiparser_done_system = FALSE;
	int dmiparser_done_chassis = FALSE;

	uint i;
	struct stat s;
	const char *path = NULL;
	const char *possible_paths[] = {
		"/usr/sbin/dmidecode",
		"/bin/dmidecode",
		"/sbin/dmidecode",
		"/usr/local/sbin/dmidecode",
        };

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


	/* find the path to dmidecode */
        for (i = 0; i < sizeof (possible_paths) / sizeof (char *); i++) {
                if (stat (possible_paths[i], &s) == 0 && S_ISREG (s.st_mode)) {
                        path = possible_paths[i];
                        break;
                }
        }

        if (path == NULL) {
                HAL_ERROR(("Could not find dmidecode, exit!"));
		exit(1);
	}

	if(pipe (dmipipe) == -1) {
		HAL_ERROR(("Could not create pipe (error: '%s'), exit!", strerror(errno)));
		exit(1);
	}	

	if ((f = fdopen (dmipipe[0], "r")) == NULL) {
		HAL_ERROR(("Could not open file (error: '%s'), exit!", strerror(errno)));
		exit(1);
	}

	if ((nullfd = open ("/dev/null", O_RDONLY)) == -1){
		HAL_ERROR(("Could not open /dev/null (error: '%s'), exit!", strerror(errno)));
		exit(1);
	}
	
	/* fork the child process */
	switch (fork ()) {
	case 0:
		/* child */
		
		dup2 (nullfd, STDIN_FILENO);
		dup2 (dmipipe[1], STDOUT_FILENO);
		close (dmipipe[0]);
		close (dmipipe[1]);
		
		/* execute the child */
		execl (path, path, NULL);
		
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
		int j;
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
			else if (!dmiparser_done_chassis && strbegin (buf, "Base Board Information"))
				dmiparser_state = DMIPARSER_STATE_BOARD;
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
		for (j = len - 2; isspace (nbuf[j]) && j >= 0; --j)
			nbuf[j] = '\0';

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
		} else if (dmiparser_state == DMIPARSER_STATE_BOARD) {
			setstr (nbuf, "Manufacturer:", "system.board.vendor");
			setstr (nbuf, "Product Name:", "system.board.product");
			setstr (nbuf, "Version:", "system.board.version");
			setstr (nbuf, "Serial Number:", "system.board.serial");
			dmiparser_done_system = TRUE;
		}
	}

	/* as read to EOF, close */
	fclose (f);

out:
	LIBHAL_FREE_DBUS_ERROR (&error);

	/* free ctx */
	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
