/***************************************************************************
 * CVSID: $Id$
 *
 * hal_hotplug.c : Tiny program to send the hotplug event to the HAL daemon
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
#include <time.h>

#include "../../hald/linux2/hotplug_helper.h"

static char sysfs_mnt_path[PATH_MAX];

/** Get the mount path for sysfs. A side-effect is that sysfs_mnt_path
 *  is set on success.
 *
 *  @return                     0 on success, negative on error
 */
static int
get_sysfs_mnt_path (void)
{
	FILE *mnt;
	struct mntent *mntent;
	int ret = 0;
	size_t dirlen = 0;

	if ((mnt = setmntent ("/proc/mounts", "r")) == NULL) {
		return -1;
	}

	while (ret == 0 && dirlen == 0
	       && (mntent = getmntent (mnt)) != NULL) {
		if (strcmp (mntent->mnt_type, "sysfs") == 0) {
			dirlen = strlen (mntent->mnt_dir);
			if (dirlen <= (PATH_MAX - 1)) {
				strcpy (sysfs_mnt_path, mntent->mnt_dir);
			} else {
				ret = -1;
			}
		}
	}
	endmntent (mnt);

	if (dirlen == 0 && ret == 0) {
		ret = -1;
	}
	return ret;
}

/* safely strcat() at most the remaining space in 'dst' */
#define strcat_len(dst, src) do { \
	dst[sizeof (dst) - 1] = '\0'; \
	strncat (dst, src, sizeof (dst) - strlen (dst) - 1); \
} while(0)


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
	char *action;
	char *seqnum_str;
	char *devname;
	int net_ifindex = -1;
	unsigned long long seqnum;

	if (argc != 2)
		return 1;

	openlog ("hal.hotplug2", LOG_PID, LOG_USER);

	syslog (LOG_INFO, "ACTION=%s SUBSYS=%s SEQNUM=%s DEVPATH=%s PHYSDEVPATH=%s", 
		getenv ("ACTION"), argv[1], getenv ("SEQNUM"), getenv ("DEVPATH"), getenv ("PHYSDEVPATH"));

	if (get_sysfs_mnt_path() != 0) {
		syslog (LOG_ERR, "could not get mountpoint for sysfs");
		goto out;
	}

	fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (fd == -1) {
		syslog (LOG_ERR, "error opening socket");
		goto out;
	}

	subsystem = argv[1];
	if (subsystem == NULL) {
		syslog (LOG_ERR, "subsystem is not set");
		goto out;
	}

	devpath = getenv ("DEVPATH");
	if (devpath == NULL) {
		syslog (LOG_ERR, "DEVPATH is not set (subsystem %s)", subsystem);
		goto out;
	}

	action = getenv ("ACTION");
	if (action == NULL) {
		syslog (LOG_ERR, "ACTION is not set");
		goto out;
	}

	seqnum_str = getenv ("SEQNUM");
	if (seqnum_str == NULL) {
		syslog (LOG_ERR, "SEQNUM is not set");
		goto out;
	}
	seqnum = strtoull (seqnum_str, NULL, 10);

	devname = getenv ("DEVNAME");


	/* pickup ifindex for net as nameif'ing the interface a
	 * hotplug event handler will screw us otherwise */
	if (strcmp (subsystem, "net") == 0) {
		FILE *f;
		char buf[80];
		char path[PATH_MAX];

		strncpy (path, sysfs_mnt_path, PATH_MAX);
		strcat_len (path, devpath);
		strcat_len (path, "/ifindex");
		if ((f = fopen (path, "r")) != NULL) {
			memset (buf, 0, sizeof (buf));
			fread (buf, sizeof (buf) - 1, 1, f);
			fclose (f);

			net_ifindex = atoi(buf);
		}
	}

	memset (&saddr, 0x00, sizeof(struct sockaddr_un));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy (&saddr.sun_path[1], HALD_HELPER_SOCKET_PATH);
	addrlen = offsetof (struct sockaddr_un, sun_path) + strlen (saddr.sun_path+1) + 1;

	memset (&msg, 0x00, sizeof (msg));
	msg.magic = HALD_HELPER_MAGIC; 
	msg.seqnum = seqnum;
	strncpy (msg.action, action, HALD_HELPER_STRLEN-1);
	strncpy (msg.subsystem, subsystem, HALD_HELPER_STRLEN-1);
	strncpy (msg.sysfs_path, devpath, HALD_HELPER_STRLEN-1);
	if (devname != NULL)
		strncpy (msg.device_name, devname, HALD_HELPER_STRLEN-1);
	else
		msg.device_name[0] = '\0';
	msg.net_ifindex = net_ifindex;
	msg.time_stamp = time (NULL);

	if (sendto (fd, &msg, sizeof(struct hald_helper_msg), 0,
		    (struct sockaddr *)&saddr, addrlen) == -1) {
		/*syslog (LOG_INFO, "error sending message to hald");*/
		goto out;
	}

out:
	return 0;
}
