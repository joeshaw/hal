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

#include <stdio.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

#include "util.h"
#include "hotplug.h"
#include "coldplug.h"

#include "hotplug_helper.h"

#include "ids.h"

char hal_sysfs_path [HAL_PATH_MAX];
char hal_proc_path [HAL_PATH_MAX];

static gboolean
hald_helper_data (GIOChannel *source, 
		  GIOCondition condition, 
		  gpointer user_data)
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
		g_strlcpy (hotplug_event->subsystem, msg.subsystem, sizeof (hotplug_event->subsystem));
		g_snprintf (hotplug_event->sysfs_path, sizeof (hotplug_event->sysfs_path), "%s%s", 
			    hal_sysfs_path, msg.sysfs_path);
		g_strlcpy (hotplug_event->device_file, msg.device_name, sizeof (hotplug_event->device_file));
		/* TODO: set wait_for_sysfs_path */
		hotplug_event->net_ifindex = msg.net_ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();

	} else if (strcmp (msg.action, "remove") == 0) {
		HotplugEvent *hotplug_event;

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->is_add = FALSE;
		g_strlcpy (hotplug_event->subsystem, msg.subsystem, sizeof (hotplug_event->subsystem));
		g_snprintf (hotplug_event->sysfs_path, sizeof (hotplug_event->sysfs_path), "%s%s", 
			    hal_sysfs_path, msg.sysfs_path);
		g_strlcpy (hotplug_event->device_file, msg.device_name, sizeof (hotplug_event->device_file));
		/* TODO: set wait_for_sysfs_path */
		hotplug_event->net_ifindex = msg.net_ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();
	}


out:
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

	/* Load various hardware id databases */
	ids_init ();

error:
	;
}

void
osspec_shutdown (void)
{
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

	hal_device_store_add (hald_get_tdl (), root);

	di_search_and_merge (root);

	hal_device_store_remove (hald_get_tdl (), root);
	hal_device_store_add (hald_get_gdl (), root);

	/* will enqueue hotplug events for entire system */
	HAL_INFO (("Synthesizing events..."));
	coldplug_synthesize_events ();
	HAL_INFO (("Done synthesizing events"));

	/* start processing events */
	hotplug_event_process_queue ();

	/*osspec_probe_done ();*/
}

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
