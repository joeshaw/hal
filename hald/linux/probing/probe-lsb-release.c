/***************************************************************************
 * CVSID: $Id$
 *
 * probe-lsb-release.c : Probe LSB information
 *
 * Copyright (C) 2010 Danny Kukawka, <danny.kukawka@web.de>
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

	dbus_error_init (&error);

	if (buf[strlen(buf)-1] == '\n')
		buf[strlen(buf)-1] = '\0';	

	value = buf + strlen (str) + 1;
	if (strcmp (value, "n/a") == 0)
		goto out;

	if (!libhal_device_set_property_string (ctx, udi, prop, value, &error))
		dbus_error_init (&error);

	HAL_DEBUG (("Setting %s='%s'", prop, value));
	return TRUE;

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
	int lsbpipe[2];
	int nullfd;
	FILE *f;

	uint i;
	struct stat s;
	const char *path = NULL;
	const char *possible_paths[] = {
		"/usr/bin/lsb_release",
		"/bin/lsb_release",
		"/sbin/lsb_release",
		"/usr/local/bin/lsb_release",
        };

	/* assume failure */
	ret = 1;

	setup_logger ();

	dbus_error_init (&error);

	udi = getenv ("UDI");
	if (udi == NULL) {
		HAL_ERROR (("UDI not set"));
		goto out;
	}

	if ((ctx = libhal_ctx_init_direct (&error)) == NULL) {
		HAL_ERROR (("ctx init failed"));
		goto out;
	}


	/* find the path to lsb_release */
        for (i = 0; i < sizeof (possible_paths) / sizeof (char *); i++) {
                if (stat (possible_paths[i], &s) == 0 && S_ISREG (s.st_mode)) {
                        path = possible_paths[i];
                        break;
                }
        }

        if (path == NULL) {
                HAL_ERROR(("Could not find lsb_release, exit!"));
		exit(1);
	}

	if(pipe (lsbpipe) == -1) {
		HAL_ERROR(("Could not create pipe (error: '%s'), exit!", strerror(errno)));
		exit(1);
	}	

	if ((f = fdopen (lsbpipe[0], "r")) == NULL) {
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
		dup2 (lsbpipe[1], STDOUT_FILENO);
		close (lsbpipe[0]);
		close (lsbpipe[1]);
		
		/* execute the child */
		execl (path, path, "-a", NULL);
		
		/* throw an error if we ever reach this point */
		HAL_ERROR (("Failed to execute lsb_release!"));
		exit (1);
		break;
	case -1:
		HAL_ERROR (("Cannot fork!"));
		goto out;
	}
	
	/* parent continues from here */
	
	/* close unused descriptor */
	close (lsbpipe[1]);
	
	/* read the output of the child */
	while(fgets (buf, sizeof(buf), f) != NULL)
	{
		HAL_DEBUG (("probe-lsb-release, got line: '%s'", buf));

		if (strbegin (buf, "LSB Version:")) {
			setstr (buf, "LSB Version:", "system.lsb.version");
		} else if (strbegin (buf, "Distributor ID:")) {
			setstr (buf, "Distributor ID:", "system.lsb.distributor_id");
		} else if (strbegin (buf, "Description:")) {
			setstr (buf, "Description:", "system.lsb.description");
		} else if (strbegin (buf, "Release:")) {
			setstr (buf, "Release:", "system.lsb.release");
		} else if (strbegin (buf, "Codename:")) {
			setstr (buf, "Codename:", "system.lsb.codename");
		}

		/* return success only if there was something usefull to parse */
		ret = 0;
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
