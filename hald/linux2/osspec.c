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
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

#include "util.h"
#include "hotplug.h"
#include "coldplug.h"

#include "hotplug_helper.h"

#include "ids.h"

#include "acpi.h"
#include "apm.h"

char hal_sysfs_path [HAL_PATH_MAX];
char hal_proc_path [HAL_PATH_MAX];

/* TODO: clean up netlink socket handling - this is almost a verbatim copy from 0.4.x */

static void
link_detection_handle_message (struct nlmsghdr *hdr)
{
	struct ifinfomsg *ifinfo;
	char ifname[1024];
	struct rtattr *attr;
	int attr_len;
	HalDevice *d;
	const char *hal_ifname;

	ifinfo = NLMSG_DATA (hdr);

	if (hdr->nlmsg_len < NLMSG_LENGTH (sizeof (struct ifinfomsg))) {
		HAL_ERROR (("Packet too small or truncated for ifinfomsg"));
		return;
	}

	memset (&ifname, 0, sizeof (ifname));

	attr = (struct rtattr *) ((unsigned char *)ifinfo + NLMSG_ALIGN (sizeof (struct ifinfomsg)));
	attr_len = NLMSG_PAYLOAD (hdr, sizeof (struct ifinfomsg));

	while (RTA_OK (attr, attr_len)) {
		if (attr->rta_type == IFLA_IFNAME) {
			unsigned int l = RTA_PAYLOAD (attr);

			if (l > sizeof (ifname) - 1)
				l = sizeof (ifname) - 1;

			strncpy (ifname, RTA_DATA (attr), l);
		}

		attr = RTA_NEXT (attr, attr_len);
	}

	/* bail out if there is no interface name */
	if (strlen (ifname) == 0)
		goto out;

	HAL_INFO (("type=0x%02x, SEQ=%d, ifi_flags=0x%04x, ifi_change=0x%04x, ifi_index=%d, ifname='%s'", 
		   hdr->nlmsg_type, 
		   hdr->nlmsg_seq,
		   ifinfo->ifi_flags,
		   ifinfo->ifi_change,
		   ifinfo->ifi_index,
		   ifname));

	/* find hal device object this event applies to */
	d = hal_device_store_match_key_value_int (hald_get_gdl (), "net.linux.ifindex", ifinfo->ifi_index);
	if (d == NULL) {
		HAL_WARNING (("No HAL device object corresponding to ifindex=%d, ifname='%s'",
			      ifinfo->ifi_index, ifname));
		goto out;
	}

	device_property_atomic_update_begin ();
	{

		/* handle link changes */
		if (ifinfo->ifi_flags & IFF_RUNNING) {
			if (hal_device_has_capability (d, "net.80203") && 
			    hal_device_property_get_bool (d, "net.80203.can_detect_link")) {
				if (!hal_device_property_get_bool (d, "net.80203.link")) {
					hal_device_property_set_bool (d, "net.80203.link", TRUE);
					HAL_INFO (("Assuming link speed is 100Mbps"));
					hal_device_property_set_uint64 (d, "net.80203.rate", 100 * 1000 * 1000);
				}
			}
		} else {
			if (hal_device_has_capability (d, "net.80203") && 
			    hal_device_property_get_bool (d, "net.80203.can_detect_link")) {
				if (hal_device_property_get_bool (d, "net.80203.link")) {
					hal_device_property_set_bool (d, "net.80203.link", FALSE);
					/* only have rate when we have a link */
					hal_device_property_remove (d, "net.80203.rate");
				}
			}
		}
		
		/* handle events for renaming */
		hal_ifname = hal_device_property_get_string (d, "net.interface");
		if (hal_ifname != NULL && strcmp (hal_ifname, ifname) != 0) {
			char new_sysfs_path[256];
			const char *sysfs_path;
			char *p;

			HAL_INFO (("Net interface '%s' renamed to '%s'", hal_ifname, ifname));

			hal_device_property_set_string (d, "net.interface", ifname);

			sysfs_path = hal_device_property_get_string (d, "net.linux.sysfs_path");
			strncpy (new_sysfs_path, sysfs_path, sizeof (new_sysfs_path) - 1);
			p = strrchr (new_sysfs_path, '/');
			if (p != NULL) {
				strncpy (p + 1, ifname, sizeof (new_sysfs_path) - 1 - (p + 1 - new_sysfs_path));
				hal_device_property_set_string (d, "net.linux.sysfs_path", new_sysfs_path);
			}
		}

		/* handle up/down status */
		if (ifinfo->ifi_flags & IFF_UP) {
			if (!hal_device_property_get_bool (d, "net.interface_up")) {
				hal_device_property_set_bool (d, "net.interface_up", TRUE);
			}
		} else {
			if (hal_device_property_get_bool (d, "net.interface_up")) {
				hal_device_property_set_bool (d, "net.interface_up", FALSE);
			}
		}
		
	}
	device_property_atomic_update_end ();

out:
	return;
}

#define VALID_NLMSG(h, s) ((NLMSG_OK (h, s) && \
                           s >= sizeof (struct nlmsghdr) && \
                           s >= h->nlmsg_len))

static gboolean
netlink_socket_data (GIOChannel *channel, GIOCondition cond, gpointer user_data)
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
		struct nlmsghdr *hdr = (struct nlmsghdr *) buf;
		guint offset = 0;

		while (offset < total_read &&
		       VALID_NLMSG (hdr, total_read - offset)) {

			if (hdr->nlmsg_type == NLMSG_DONE)
				break;

			if (hdr->nlmsg_type == RTM_NEWLINK ||
			    hdr->nlmsg_type == RTM_DELLINK)
				link_detection_handle_message (hdr);

			offset += hdr->nlmsg_len;
			hdr = (struct nlmsghdr *) (buf + offset);
		}

		if (offset < total_read &&
		    !VALID_NLMSG (hdr, total_read - offset)) {
			HAL_ERROR (("Packet too small or truncated"));
			return TRUE;
		}
	}

	return TRUE;
}


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

void
osspec_init (void)
{
	int netlinkfd;
	struct sockaddr_nl netlink_addr;
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


	/* Listen to the netlink socket */
	netlinkfd = socket (PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);

	if (netlinkfd < 0) {
		DIE (("Couldn't open socket"));
	}

	memset (&netlink_addr, 0, sizeof (netlink_addr));
	netlink_addr.nl_family = AF_NETLINK;
	netlink_addr.nl_pid = getpid ();
	netlink_addr.nl_groups = RTMGRP_LINK;
	if (bind (netlinkfd, (struct sockaddr *) &netlink_addr, sizeof (netlink_addr)) < 0) {
		DIE (("Unable to bind to netlink socket"));
		return;
	}
	channel = g_io_channel_unix_new (netlinkfd);
	g_io_add_watch (channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_NVAL,
			netlink_socket_data, NULL);
	
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
	HAL_INFO (("Synthesizing sysfs events..."));
	coldplug_synthesize_events ();
	HAL_INFO (("Synthesizing ACPI events..."));
	if (!acpi_synthesize_hotplug_events ()) {
		HAL_INFO (("No ACPI capabilities found; checking for APM"));
		apm_synthesize_hotplug_events ();
	}
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
