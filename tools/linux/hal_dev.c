/***************************************************************************
 * CVSID: $Id$
 *
 * hal_dev.c : Tiny program to send the udev device event to
 *             the hal daemon
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <mntent.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "../../hald/linux/hald_helper.h"

/*#define HAL_DEVD_EXTRA_DEBUG 1*/

/** Entry point
 *
 *  @param  argc                Number of arguments
 *  @param  argv                Array of arguments
 *  @param  envp                Environment
 *  @return                     Exit code
 */
int
main (int argc, char *argv[], char *envp[])
{
	int fd;
	struct hald_helper_msg msg;
	struct sockaddr_un saddr;
	socklen_t addrlen;
	char *subsystem;
	char *devpath;
	char *devname;
	char *action;
	char *seqnum_str;
	int seqnum;

	openlog ("hal.dev", LOG_PID, LOG_USER);

#ifdef HAL_DEVD_EXTRA_DEBUG
	syslog (LOG_NOTICE, "entering for %s", getenv ("DEVPATH"));
#endif

	if (argc != 2)
		return 1;

	subsystem = argv[1];
	if (subsystem == NULL) {
		syslog (LOG_ERR, "subsystem is not set");
		goto out;
	}

	devpath = getenv ("DEVPATH");
	if (devpath == NULL) {
		syslog (LOG_ERR, "DEVPATH is not set");
		goto out;
	}

	devname = getenv ("DEVNAME");
	if (devname == NULL) {
		syslog (LOG_ERR, "DEVNAME is not set");
		goto out;
	}

	action = getenv ("ACTION");
	if (action == NULL) {
		syslog (LOG_ERR, "ACTION is not set");
		goto out;
	}

	seqnum_str = getenv ("SEQNUM");
	if (seqnum_str == NULL) {
		seqnum = 0;
	} else {
		seqnum = strtoull (seqnum_str, NULL, 10);
	}


	fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (fd == -1) {
		syslog (LOG_ERR, "error opening socket");
		goto out;
	}

	memset (&saddr, 0x00, sizeof(struct sockaddr_un));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy (&saddr.sun_path[1], HALD_HELPER_SOCKET_PATH);
	addrlen = offsetof (struct sockaddr_un, sun_path) + strlen (saddr.sun_path+1) + 1;

	memset (&msg, 0x00, sizeof (msg));
	msg.magic = HALD_HELPER_MAGIC; 
	msg.type = HALD_DEVD;
	msg.seqnum = seqnum;
	strncpy (msg.action, action, HALD_HELPER_STRLEN-1);
	strncpy (msg.subsystem, subsystem, HALD_HELPER_STRLEN-1);
	strncpy (msg.sysfs_path, devpath, HALD_HELPER_STRLEN-1);
	strncpy (msg.device_name, devname, HALD_HELPER_STRLEN-1);

	if (sendto (fd, &msg, sizeof(struct hald_helper_msg), 0,
		    (struct sockaddr *)&saddr, addrlen) == -1) {
#ifdef HAL_DEVD_EXTRA_DEBUG
		syslog (LOG_ERR, "error sending message to hald for %s", devpath);
#endif
	} else {
#ifdef HAL_DEVD_EXTRA_DEBUG
		syslog (LOG_NOTICE, "sent message to hald for %s", devpath);
#endif
	}
out:
#ifdef HAL_DEVD_EXTRA_DEBUG
	syslog (LOG_NOTICE, "exiting");
#endif
	return 0;
}
