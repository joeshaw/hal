/***************************************************************************
 * CVSID: $Id$
 *
 * osspec.c : New and improved HAL backend for Linux 2.6
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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

#define _GNU_SOURCE 1

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <mntent.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <net/if_arp.h> /* for ARPHRD_... */
#include <sys/socket.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../hald_dbus.h"
#include "../device_info.h"

#include "util.h"
#include "hotplug.h"
#include "coldplug.h"

#include "hotplug_helper.h"

#include "ids.h"

#include "acpi.h"
#include "apm.h"
#include "pmu.h"
#include "blockdev.h"

#include "osspec_linux.h"

char hal_sysfs_path [HAL_PATH_MAX];
char hal_proc_path [HAL_PATH_MAX];

const gchar *
get_hal_sysfs_path (void)
{
	return hal_sysfs_path;
}

const gchar *
get_hal_proc_path (void)
{
	return hal_proc_path;
}

static gboolean
hald_helper_data (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
	struct hald_helper_msg msg;
	int fd;
	int retval;
	struct msghdr smsg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct ucred *cred;
	char cred_msg[CMSG_SPACE(sizeof(struct ucred))];

	fd = g_io_channel_unix_get_fd (source);

	iov.iov_base = &msg;
	iov.iov_len = sizeof (struct hald_helper_msg);

	memset(&smsg, 0x00, sizeof (struct msghdr));
	smsg.msg_iov = &iov;
	smsg.msg_iovlen = 1;
	smsg.msg_control = cred_msg;
	smsg.msg_controllen = sizeof (cred_msg);

	retval = recvmsg (fd, &smsg, 0);
	if (retval <  0) {
		if (errno != EINTR)
			HAL_INFO (("Unable to receive message, errno=%d", errno));
		goto out;
	}
	cmsg = CMSG_FIRSTHDR (&smsg);
	cred = (struct ucred *) CMSG_DATA (cmsg);

	if (cmsg == NULL || cmsg->cmsg_type != SCM_CREDENTIALS) {
		HAL_INFO (("No sender credentials received, message ignored"));
		goto out;
	}

	if (cred->uid != 0) {
		HAL_INFO (("Sender uid=%i, message ignored", cred->uid));
		goto out;
	}

	if (msg.magic != HALD_HELPER_MAGIC) {
		HAL_INFO (("Magic is wrong, message ignored", cred->uid));
		goto out;
	}

	HAL_INFO (("SEQNUM=%lld, TIMESTAMP=%d, ACTION=%s, SUBSYS=%s, SYSFSPATH=%s, DEVNAME=%s, IFINDEX=%d", 
		   msg.seqnum, msg.time_stamp, msg.action, msg.subsystem, 
		   msg.sysfs_path, msg.device_name, msg.net_ifindex));

	if (strcmp (msg.action, "add") == 0) {
		HotplugEvent *hotplug_event;

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = TRUE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, msg.subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path), "%s%s", 
			    hal_sysfs_path, msg.sysfs_path);
		g_strlcpy (hotplug_event->sysfs.device_file, msg.device_name, sizeof (hotplug_event->sysfs.device_file));
		/* TODO: set wait_for_sysfs_path */
		hotplug_event->sysfs.net_ifindex = msg.net_ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();

	} else if (strcmp (msg.action, "remove") == 0) {
		HotplugEvent *hotplug_event;

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = FALSE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, msg.subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path), "%s%s", 
			    hal_sysfs_path, msg.sysfs_path);
		g_strlcpy (hotplug_event->sysfs.device_file, msg.device_name, sizeof (hotplug_event->sysfs.device_file));
		/* TODO: set wait_for_sysfs_path */
		hotplug_event->sysfs.net_ifindex = msg.net_ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();
	}


out:
	return TRUE;
}

#define VALID_NLMSG(h, s) ((NLMSG_OK (h, s) && \
                           s >= sizeof (struct nlmsghdr) && \
                           s >= h->nlmsg_len))

static gboolean
netlink_detection_data_ready (GIOChannel *channel, GIOCondition cond,
			   gpointer user_data)
{
	int fd;
	int bytes_read;
	guint total_read = 0;
	struct sockaddr_nl nladdr;
	socklen_t nladdrlen = sizeof(nladdr);
	char buf[1024];

	if (cond & ~(G_IO_IN | G_IO_PRI)) {
		HAL_ERROR (("Error occurred on netlink socket"));
		return TRUE;
	}

	fd = g_io_channel_unix_get_fd (channel);

	do {
		errno = 0;
		bytes_read = recvfrom (fd,
				   buf + total_read,
				   sizeof (buf) - total_read,
				   MSG_DONTWAIT,
				   (struct sockaddr*)&nladdr, &nladdrlen);
		if (nladdrlen != sizeof(nladdr)) {
			HAL_ERROR(("Bad address size reading netlink socket"));
			return TRUE;
		}
		if (nladdr.nl_pid) {
			HAL_ERROR(("Spoofed packet received on netlink socket"));
			return TRUE;
		}
		if (bytes_read > 0)
			total_read += bytes_read;
	} while (bytes_read > 0 || errno == EINTR);

	if (bytes_read < 0 && errno != EAGAIN) {
		HAL_ERROR (("Error reading data off netlink socket"));
		return TRUE;
	}

	if (total_read > 0) {
		HAL_INFO (("total_read=%d buf='%s'", total_read, buf));
	}

	/* Handle event: "mount@/block/hde" */
	if (g_str_has_prefix (buf, "mount")) {
		gchar sysfs_path[HAL_PATH_MAX];
		g_strlcpy (sysfs_path, get_hal_sysfs_path (), sizeof (sysfs_path));
		g_strlcat (sysfs_path, ((char *) buf) + sizeof ("mount"), sizeof (sysfs_path));
		blockdev_mount_status_changed (sysfs_path, TRUE);
	}

	/* Handle event: "umount@/block/hde" */
	if (g_str_has_prefix (buf, "umount")) {
		gchar sysfs_path[HAL_PATH_MAX];
		g_strlcpy (sysfs_path, get_hal_sysfs_path (), sizeof (sysfs_path));
		g_strlcat (sysfs_path, ((char *) buf) + sizeof ("umount"), sizeof (sysfs_path));
		blockdev_mount_status_changed (sysfs_path, FALSE);
	}

	return TRUE;
}

void
osspec_init (void)
{
	int socketfd;
	struct sockaddr_un saddr;
	socklen_t addrlen;
	GIOChannel *channel;	
	const int on = 1;
	static int netlink_fd = -1;
	struct sockaddr_nl netlink_addr;
	GIOChannel *netlink_channel;

	/* setup socket for listening from datagrams from the hal.hotplug helper */
	memset(&saddr, 0x00, sizeof(saddr));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy(&saddr.sun_path[1], HALD_HELPER_SOCKET_PATH);
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(saddr.sun_path+1) + 1;

	socketfd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (socketfd == -1) {
		DIE (("Couldn't open socket"));
	}

	if (bind(socketfd, (struct sockaddr *) &saddr, addrlen) < 0) {
		fprintf (stderr, "Error binding to %s: %s\n", HALD_HELPER_SOCKET_PATH, strerror(errno));
		exit (1);
	}
	/* enable receiving of the sender credentials */
	setsockopt(socketfd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	channel = g_io_channel_unix_new (socketfd);
	g_io_add_watch (channel, G_IO_IN, hald_helper_data, NULL);
	g_io_channel_unref (channel);

	/* Get mount points for /proc and /sys */
	if (!hal_util_get_fs_mnt_path ("sysfs", hal_sysfs_path, sizeof (hal_sysfs_path))) {
		HAL_ERROR (("Could not get sysfs mount point"));
		goto error;
	}
	HAL_INFO (("sysfs mount point is '%s'", hal_sysfs_path));
	if (!hal_util_get_fs_mnt_path ("proc", hal_proc_path, sizeof (hal_proc_path))) {
		HAL_ERROR (("Could not get proc mount point"));
		goto error;
	}
	HAL_INFO (("proc mount point is '%s'", hal_proc_path));

	/* hook up to netlink socket to receive events from the Kernel Events
	 * Layer (available since 2.6.10) - TODO: Don't use the constant 15 but
	 * rather the NETLINK_KOBJECT_UEVENT symbol
	 */
	netlink_fd = socket (PF_NETLINK, SOCK_DGRAM, 15/*NETLINK_KOBJECT_UEVENT*/);

	if (netlink_fd < 0) {
		DIE (("Unable to create netlink socket"));
	}

	memset (&netlink_addr, 0, sizeof (netlink_addr));
	netlink_addr.nl_family = AF_NETLINK;
	netlink_addr.nl_pid = getpid ();
	netlink_addr.nl_groups = 0xffffffff;//RTMGRP_LINK;//1 << 15 /*NETLINK_KOBJECT_UEVENT*/;

	if (bind (netlink_fd, (struct sockaddr *) &netlink_addr, sizeof (netlink_addr)) < 0) {
		DIE (("Unable to bind to netlink socket"));
	}

	netlink_channel = g_io_channel_unix_new (netlink_fd);

	g_io_add_watch (netlink_channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_NVAL,
			netlink_detection_data_ready, NULL);

	/* Load various hardware id databases */
	ids_init ();

error:
	;
}

void
osspec_shutdown (void)
{
}

static void 
computer_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Add callouts completed udi=%s", d->udi));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	/* start processing events */
	hotplug_event_process_queue ();
}

void 
osspec_probe (void)
{
	HalDevice *root;

	root = hal_device_new ();
	hal_device_property_set_string (root, "info.bus", "unknown");
	hal_device_property_set_string (root, "linux.sysfs_path_device", "(none)");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_property_set_string (root, "info.udi", "/org/freedesktop/Hal/devices/computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");

	/* Let computer be in TDL while synthesizing all other events because some may write to the object */
	hal_device_store_add (hald_get_tdl (), root);

	/* will enqueue hotplug events for entire system */
	HAL_INFO (("Synthesizing sysfs events..."));
	coldplug_synthesize_events ();
	HAL_INFO (("Synthesizing powermgmt events..."));
	if (acpi_synthesize_hotplug_events ()) {
		HAL_INFO (("ACPI capabilities found"));
	} else if (pmu_synthesize_hotplug_events ()) {
		HAL_INFO (("PMU capabilities found"));		
	} else if (apm_synthesize_hotplug_events ()) {
		HAL_INFO (("APM capabilities found"));		
	} else {
		HAL_INFO (("No powermgmt capabilities"));		
	}
	HAL_INFO (("Done synthesizing events"));

	di_search_and_merge (root, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (root, DEVICE_INFO_TYPE_POLICY);

	hal_util_callout_device_add (root, computer_callouts_add_done, NULL, NULL);

	/*osspec_probe_done ();*/
}

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

gboolean
osspec_device_rescan (HalDevice *d)
{
	return hotplug_rescan_device (d);
}

gboolean
osspec_device_reprobe (HalDevice *d)
{
	return hotplug_reprobe_tree (d);
}

/* Returns the path of the udevinfo program 
 *
 * @return                      Path or NULL if udevinfo program is not found
 */
static const gchar *
hal_util_get_udevinfo_path (void)
{
	guint i;
	struct stat s;
	static gchar *path = NULL;
	gchar *possible_paths[] = { 
		"/sbin/udevinfo",
		"/usr/bin/udevinfo",
		"/usr/sbin/udevinfo",
		"/usr/local/sbin/udevinfo"
	};

	if (path != NULL)
		return path;

	for (i = 0; i < sizeof (possible_paths) / sizeof (char *); i++) {
		if (stat (possible_paths[i], &s) == 0 && S_ISREG (s.st_mode)) {
			path = possible_paths[i];
			break;
		}
	}
	return path;
}

/** Get the name of the special device file given the sysfs path.
 *
 *  @param  sysfs_path          Path to class device in sysfs
 *  @param  dev_file            Where the special device file name should be stored
 *  @param  dev_file_length     Size of dev_file character array
 *  @return                     TRUE only if the device file could be found
 */
gboolean
hal_util_get_device_file (const gchar *sysfs_path, gchar *dev_file, gsize dev_file_length)
{
	int i;
	gsize sysfs_path_len;
	gsize sysfs_mount_path_len;
	gchar sysfs_path_trunc[HAL_PATH_MAX];
	gchar sysfs_path_dev_trunc[HAL_PATH_MAX + 4];
	char *udev_argv[7] = { NULL, 
			       "-r", "-q", "name", "-p",
			       sysfs_path_trunc, NULL };
	char *udev_stdout;
	char *udev_stderr;
	int udev_exitcode;
	struct stat statbuf;

	/* check for dev file in sysfs path */
	sysfs_path_len = strlen (sysfs_path);
	strncpy (sysfs_path_dev_trunc, sysfs_path, HAL_PATH_MAX);
	strncat (sysfs_path_dev_trunc + sysfs_path_len, "/dev", 4);
	if (stat (sysfs_path_dev_trunc, &statbuf) != 0)
		return FALSE;

	/* get path to udevinfo */
	udev_argv[0] = (char *) hal_util_get_udevinfo_path ();
	if (udev_argv[0] == NULL)
		return FALSE;

	/* compute truncated sysfs path as udevinfo doesn't want the sysfs_mount_path (e.g. /sys) prefix */
	sysfs_mount_path_len = strlen (hal_sysfs_path);
	if (strlen (sysfs_path) > sysfs_mount_path_len) {
		strncpy (sysfs_path_trunc, sysfs_path + sysfs_mount_path_len, HAL_PATH_MAX - sysfs_mount_path_len);
	}

	/* Now invoke udevinfo */
	if (udev_argv[0] == NULL || g_spawn_sync ("/",
						  udev_argv,
						  NULL,
						  0,
						  NULL,
						  NULL,
						  &udev_stdout,
						  &udev_stderr,
						  &udev_exitcode,
						  NULL) != TRUE) {
		HAL_ERROR (("Couldn't invoke %s", udev_argv[0]));
		return FALSE;
	}

	if (udev_exitcode != 0) {
		HAL_ERROR (("%s returned %d for %s", udev_argv[0], udev_exitcode, sysfs_path_trunc));
		return FALSE;
	}

	/* sanitize string returned by udev */
	for (i = 0; udev_stdout[i] != 0; i++) {
		if (udev_stdout[i] == '\r' || udev_stdout[i] == '\n') {
			udev_stdout[i] = 0;
			break;
		}
	}

	/*HAL_INFO (("got device file %s for %s", udev_stdout, sysfs_path));*/

	strncpy (dev_file, udev_stdout, dev_file_length);
	return TRUE;
}

gboolean
hal_util_set_driver (HalDevice *d, const char *property_name, const char *sysfs_path)
{
	gboolean ret;
	gchar driver_path[HAL_PATH_MAX];
	struct stat statbuf;

	ret = FALSE;

	g_snprintf (driver_path, sizeof (driver_path), "%s/driver", sysfs_path);
	if (stat (driver_path, &statbuf) == 0) {
		gchar buf[256];
		memset (buf, '\0', sizeof (buf));
		if (readlink (driver_path, buf, sizeof (buf) - 1) > 0) {
			hal_device_property_set_string (d, property_name, hal_util_get_last_element (buf));
			ret = TRUE;
		}
	}

	return ret;
}

/** Find the closest ancestor by looking at sysfs paths
 *
 *  @param  sysfs_path           Path into sysfs, e.g. /sys/devices/pci0000:00/0000:00:1d.7/usb1/1-0:1.0
 *  @return                      Parent Hal Device Object or #NULL if there is none
 */
HalDevice *
hal_util_find_closest_ancestor (const gchar *sysfs_path)
{	
	gchar buf[512];
	HalDevice *parent;

	parent = NULL;

	strncpy (buf, sysfs_path, sizeof (buf));
	do {
		char *p;

		p = strrchr (buf, '/');
		if (p == NULL)
			break;
		*p = '\0';

		parent = hal_device_store_match_key_value_string (hald_get_gdl (), 
								  "linux.sysfs_path_device", 
								  buf);
		if (parent != NULL)
			break;

	} while (TRUE);

	return parent;
}

