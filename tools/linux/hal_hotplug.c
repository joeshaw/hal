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

#include "../../hald/linux/hald_helper.h"

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

static const char *file_list_class_device[] ={
	"dev",
	NULL
};

static const char *file_list_usb[] = {
	"idProduct",
	"idVendor",
	"bcdDevice",
	"bMaxPower",
	/*"serial", */
	"bmAttributes",
	/*"manufacturer",*/
	/*"product",*/
	"bDeviceClass",
	"bDeviceSubClass",
	"bDeviceProtocol",
	"bNumConfigurations",
	"bConfigurationValue",
	"bNumInterfaces",
	NULL

};

static const char *file_list_usbif[] = {
	"bInterfaceClass",
	"bInterfaceSubClass",
	"bInterfaceProtocol",
	"bInterfaceNumber",
	NULL
};

static const char *file_list_scsi[] = { "vendor",
					"model",
					"type",
					NULL };

static const char *file_list_scsi_generic[] = { "device", 
						NULL };

static const char *file_list_scsi_host[] = { "device",
					     NULL };

static const char *file_list_block[] = {
	"dev",
	"size",
	/*"removable",*/
	NULL
};

static const char *file_list_pci[] = {
	"device",
	"vendor",
	"subsystem_device",
	"subsystem_vendor",
	"class",
	NULL
};

/* safely strcat() at most the remaining space in 'dst' */
#define strcat_len(dst, src) do { \
	dst[sizeof (dst) - 1] = '\0'; \
	strncat (dst, src, sizeof (dst) - strlen (dst) - 1); \
} while(0)

static int
wait_for_sysfs_info (char *devpath, char *hotplug_type)
{
	size_t devpath_len;
	const char **file_list;
	int timeout;
	int rc;
	struct stat stat_buf;
	int i;
	char path[PATH_MAX];

	devpath_len = strlen (devpath);

	file_list = NULL;

	if (strcmp (hotplug_type, "pci") == 0) {
		file_list = file_list_pci;
	} else if (strcmp (hotplug_type, "usb") == 0) {
		int is_interface = 0;

		if (strstr (devpath, "class") != NULL) {
			file_list = file_list_class_device;
		} else {
			for (i = devpath_len - 1; devpath[i] != '/' && i > 0; --i) {
				if (devpath[i] == ':') {
					is_interface = 1;
					break;
				}
			}

			if (is_interface) {
				file_list = file_list_usbif;
			} else
				file_list = file_list_usb;
		}
	} else if (strcmp (hotplug_type, "scsi") == 0) {
		file_list = file_list_scsi;
	} else if (strcmp (hotplug_type, "scsi_generic") == 0) {
		file_list = file_list_scsi_generic;
	} else if (strcmp (hotplug_type, "scsi_host") == 0) {
		file_list = file_list_scsi_host;
	} else if (strcmp (hotplug_type, "block") == 0) {
		file_list = file_list_block;
	}

	if (file_list == NULL) {
		return -1;
	}

	timeout = 0;

try_again:
	if (timeout > 0) {
		usleep (100 * 1000); /* 100 ms */
	}
	timeout += 100 * 1000;

	if (timeout >= 10 * 1000*1000) { /* 10 secs */
		syslog (LOG_NOTICE, "timout(%d ms) waiting for %s ",
			timeout / 1000, devpath);
		return -1;
	}


	/* first, check directory */
	strncpy (path, sysfs_mnt_path, PATH_MAX);
	strcat_len (path, devpath);

	/*printf("path0 = %s\n", path); */

	rc = stat (path, &stat_buf);
	/*printf("rc0 = %d\n", rc); */
	if (rc != 0)
		goto try_again;

	/* second, check each requested file */
	for (i = 0; file_list[i] != NULL; i++) {
		const char *file;

		file = file_list[i];
		strncpy (path, sysfs_mnt_path, PATH_MAX);
		strcat_len (path, devpath);
		strcat_len (path, "/");
		strcat_len (path, file);
		/*printf("path1 = %s\n", path); */

		rc = stat (path, &stat_buf);

		/*printf("rc1 = %d\n", rc); */

		if (rc != 0)
			goto try_again;
	}	
	return 0;
}

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
	int is_add;
	int seqnum;

	if (argc != 2)
		return 1;

	openlog ("hal.hotplug", LOG_PID, LOG_USER);

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
		syslog (LOG_ERR, "DEVPATH is not set");
		goto out;
	}

	action = getenv ("ACTION");
	if (action == NULL) {
		syslog (LOG_ERR, "ACTION is not set");
		goto out;
	}
	if (strcmp (action, "add") == 0)
		is_add = 1;
	else
		is_add = 0;

	seqnum_str = getenv ("SEQNUM");
	if (seqnum_str == NULL) {
		syslog (LOG_ERR, "SEQNUM is not set");
		goto out;
	}
	seqnum = atoi (seqnum_str);

	if (is_add) {
		/* wait for information to be published in sysfs */
		wait_for_sysfs_info (devpath, subsystem);
	}

	memset (&saddr, 0x00, sizeof(struct sockaddr_un));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy (&saddr.sun_path[1], HALD_HELPER_SOCKET_PATH);
	addrlen = offsetof (struct sockaddr_un, sun_path) + strlen (saddr.sun_path+1) + 1;

	memset (&msg, 0x00, sizeof (msg));
	msg.magic = HALD_HELPER_MAGIC; 
	msg.is_hotplug_or_dev = 1;
	msg.is_add = is_add;
	msg.seqnum = seqnum;
	strncpy (msg.subsystem, subsystem, HALD_HELPER_STRLEN);
	strncpy (msg.sysfs_path, devpath, HALD_HELPER_STRLEN);

	if (sendto (fd, &msg, sizeof(struct hald_helper_msg), 0,
		    (struct sockaddr *)&saddr, addrlen) == -1) {
		/*syslog (LOG_INFO, "error sending message to hald");*/
		goto out;
	}

out:
	return 0;
}

