/***************************************************************************
 * CVSID: $Id$
 *
 * probe-smbios.c : Probe system BIOS according to the SMBIOS/DMI standard
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "libhal/libhal.h"
#include "shared.h"

#define DMIPARSER_STATE_IGNORE 0
#define DMIPARSER_STATE_BIOS 1
#define DMIPARSER_STATE_SYSTEM 2
#define DMIPARSER_STATE_CHASSIS 3

#define strbegin(buf, str) (strncmp (buf, str, sizeof (str) - 1) == 0)

#define setstr(buf, str, prop)				                                                    \
	do {								                                    \
		if (strbegin (buf, str)) {				                                    \
			dbus_error_init (&error);                                                           \
			libhal_device_set_property_string (ctx, udi, prop, buf + sizeof(str), &error);      \
			dbg ("Setting %s='%s'", prop, buf + sizeof(str));                                   \
		}							                                    \
	} while (FALSE);


int 
main (int argc, char *argv[])
{
	int ret;
	char *udi;
	LibHalContext *ctx = NULL;
	DBusError error;
	char buf[512];
	int dmipipe[2];
	int nullfd;
	FILE *f;
	int dmiparser_state = DMIPARSER_STATE_IGNORE;

	/* on some system chassis pops up several times; so only take the first entry for each */
	int dmiparser_done_bios = FALSE;
	int dmiparser_done_system = FALSE;
	int dmiparser_done_chassis = FALSE;

	/* assume failure */
	ret = 1;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	pipe (dmipipe);	
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
		exit (1);
		break;
	case -1:
		dbg ("Cannot fork!");
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

		/* trim whitespace */
		len = strlen (buf);
		if (len >= sizeof (buf))
			continue;

		for (i = len - 1; isspace (buf[i]) && i >= 0; --i)
			buf[i] = '\0';

		if (!strbegin (buf, "\t\t"))
			dmiparser_state = DMIPARSER_STATE_IGNORE;

		switch (dmiparser_state)
		{
		case DMIPARSER_STATE_IGNORE:
			if (strbegin (buf, "\tBIOS Information")) {
				if (!dmiparser_done_bios)
					dmiparser_state = DMIPARSER_STATE_BIOS;
			} else if (strbegin (buf, "\tSystem Information")) {
				if (!dmiparser_done_system)
					dmiparser_state = DMIPARSER_STATE_SYSTEM;
			} else if (strbegin (buf, "\tChassis Information")) {
				if (!dmiparser_done_chassis)
					dmiparser_state = DMIPARSER_STATE_CHASSIS;
			}
			break;
			
		case DMIPARSER_STATE_BIOS:
			setstr (buf, "\t\tVendor:", "smbios.bios.vendor");
			setstr (buf, "\t\tVersion:", "smbios.bios.version");
			setstr (buf, "\t\tRelease Date:", "smbios.bios.release_date");
			dmiparser_done_bios = TRUE;
			break;

		case DMIPARSER_STATE_SYSTEM:
			setstr (buf, "\t\tManufacturer:", "smbios.system.manufacturer");
			setstr (buf, "\t\tProduct Name:", "smbios.system.product");
			setstr (buf, "\t\tVersion:", "smbios.system.version");
			setstr (buf, "\t\tSerial Number:", "smbios.system.serial");
			setstr (buf, "\t\tUUID:", "smbios.system.uuid");
			dmiparser_done_system = TRUE;
			break;

		case DMIPARSER_STATE_CHASSIS:
			setstr (buf, "\t\tManufacturer:", "smbios.chassis.manufacturer");
			setstr (buf, "\t\tType:", "smbios.chassis.type");
			dmiparser_done_chassis = TRUE;
			break;
		}
	}
	
	fclose (f);


out:
	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;
}
