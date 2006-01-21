/***************************************************************************
 * CVSID: $Id$
 *
 * osspec.c : New and improved HAL backend for Linux 2.6
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Danny Kukawka, <danny.kukawka@web.de>
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
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <net/if_arp.h>
#include <sys/socket.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../hald_dbus.h"
#include "../hald_runner.h"
#include "../device_info.h"
#include "../util.h"

#include "hotplug.h"
#include "coldplug.h"

#include "hotplug_helper.h"

#include "ids.h"

#include "acpi.h"
#include "apm.h"
#include "pmu.h"
#include "blockdev.h"

#include "osspec_linux.h"

static char *hal_sysfs_path;
static char *hal_proc_path;

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
hald_udev_data (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
	int fd;
	int retval;
	struct msghdr smsg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct ucred *cred;
	char cred_msg[CMSG_SPACE(sizeof(struct ucred))];

	char buf[2048];
	size_t bufpos = 0;
	const char *devpath = NULL;
	const char *physdevpath = NULL;
	const char *action = NULL;
	const char *subsystem = NULL;
	const char *devname = NULL;
	int ifindex = -1;
	unsigned long long seqnum = 0;

	fd = g_io_channel_unix_get_fd (source);

	iov.iov_base = &buf;
	iov.iov_len = sizeof (buf);

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

	if (!strstr(buf, "@/")) {
		HAL_INFO (("invalid message format"));
		goto out;
	}

	while (bufpos < sizeof (buf)) {
		size_t keylen;
		char *key;

		key = &buf[bufpos];
		keylen = strlen(key);
		if (keylen == 0)
			break;
		bufpos += keylen + 1;

		if (strncmp(key, "ACTION=", 7) == 0)
			action = &key[7];
		else if (strncmp(key, "DEVPATH=", 8) == 0)
			devpath = &key[8];
		else if (strncmp(key, "SUBSYSTEM=", 10) == 0)
			subsystem = &key[10];
		else if (strncmp(key, "PHYSDEVPATH=", 12) == 0)
			physdevpath = &key[12];
		else if (strncmp(key, "DEVNAME=", 8) == 0)
			devname = &key[8];
		else if (strncmp(key, "SEQNUM=", 7) == 0)
			seqnum = strtoull(&key[7], NULL, 10);
		else if (strncmp(key, "IFINDEX=", 8) == 0)
			ifindex = strtoul(&key[8], NULL, 10);
	}

	if (!devpath) {
		HAL_INFO (("missing DEVPATH"));
		goto out;
	}
	if (!action) {
		HAL_INFO (("missing ACTION"));
		goto out;
	}
	if (!subsystem) {
		HAL_INFO (("missing SUSBSYSTEM"));
		goto out;
	}
	if (!devname)
		devname = "";

	HAL_INFO (("SEQNUM=%lld, ACTION=%s, SUBSYS=%s, SYSFSPATH=%s, DEVNAME=%s, IFINDEX=%d",
		   seqnum, action, subsystem, devpath, devname, ifindex));

	if (strcmp (action, "add") == 0) {
		HotplugEvent *hotplug_event;

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->action = HOTPLUG_ACTION_ADD;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path), "%s%s", 
			    hal_sysfs_path, devpath);
		g_strlcpy (hotplug_event->sysfs.device_file, devname, sizeof (hotplug_event->sysfs.device_file));
		hotplug_event->sysfs.net_ifindex = ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();

	} else if (strcmp (action, "remove") == 0) {
		HotplugEvent *hotplug_event;

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->action = HOTPLUG_ACTION_REMOVE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path), "%s%s", 
			    hal_sysfs_path, devpath);
		g_strlcpy (hotplug_event->sysfs.device_file, devname, sizeof (hotplug_event->sysfs.device_file));
		hotplug_event->sysfs.net_ifindex = ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();
	}

out:
	return TRUE;
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
		hotplug_event->action = HOTPLUG_ACTION_ADD;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, msg.subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path), "%s%s", 
			    hal_sysfs_path, msg.sysfs_path);
		g_strlcpy (hotplug_event->sysfs.device_file, msg.device_name, sizeof (hotplug_event->sysfs.device_file));
		hotplug_event->sysfs.net_ifindex = msg.net_ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();

	} else if (strcmp (msg.action, "remove") == 0) {
		HotplugEvent *hotplug_event;

		hotplug_event = g_new0 (HotplugEvent, 1);
		hotplug_event->action = HOTPLUG_ACTION_REMOVE;
		hotplug_event->type = HOTPLUG_EVENT_SYSFS;
		g_strlcpy (hotplug_event->sysfs.subsystem, msg.subsystem, sizeof (hotplug_event->sysfs.subsystem));
		g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path), "%s%s", 
			    hal_sysfs_path, msg.sysfs_path);
		g_strlcpy (hotplug_event->sysfs.device_file, msg.device_name, sizeof (hotplug_event->sysfs.device_file));
		hotplug_event->sysfs.net_ifindex = msg.net_ifindex;

		/* queue up and process */
		hotplug_event_enqueue (hotplug_event);
		hotplug_event_process_queue ();
	}

out:
	return TRUE;
}

static gboolean
mount_tree_changed_event (GIOChannel *channel, GIOCondition cond,
		    gpointer user_data)
{
	if (cond & ~G_IO_ERR)
		return TRUE;

	HAL_INFO (("/proc/mounts tells, that the mount has tree changed"));
	blockdev_refresh_mount_state (NULL);

	return TRUE;
}

void
osspec_init (void)
{
	gchar path[HAL_PATH_MAX];
	int udev_socket;
	int helper_socket;
	struct sockaddr_un saddr;
	socklen_t addrlen;
	GIOChannel *udev_channel;
	GIOChannel *helper_channel;
	const int on = 1;
	GIOChannel *mounts_channel;

	/*
	 * setup socket for listening from messages from udev
	 */
	memset(&saddr, 0x00, sizeof(saddr));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy(&saddr.sun_path[1], "/org/freedesktop/hal/udev_event");
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(saddr.sun_path+1) + 1;

	udev_socket = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (udev_socket == -1) {
		DIE (("Couldn't open socket"));
	}

	if (bind(udev_socket, (struct sockaddr *) &saddr, addrlen) < 0) {
		fprintf (stderr, "Error binding udev_event socket: %s\n", strerror(errno));
		exit (1);
	}
	/* enable receiving of the sender credentials */
	setsockopt(udev_socket, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	udev_channel = g_io_channel_unix_new (udev_socket);
	g_io_add_watch (udev_channel, G_IO_IN, hald_udev_data, NULL);
	g_io_channel_unref (udev_channel);

	/*
	 * setup socket for listening from datagrams from the hal.hotplug helper
	 */
	memset(&saddr, 0x00, sizeof(saddr));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy(&saddr.sun_path[1], HALD_HELPER_SOCKET_PATH);
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(saddr.sun_path+1) + 1;

	helper_socket = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (helper_socket == -1) {
		DIE (("Couldn't open socket"));
	}

	if (bind(helper_socket, (struct sockaddr *) &saddr, addrlen) < 0) {
		fprintf (stderr, "Error binding to %s: %s\n", HALD_HELPER_SOCKET_PATH, strerror(errno));
		exit (1);
	}
	/* enable receiving of the sender credentials */
	setsockopt(helper_socket, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	helper_channel = g_io_channel_unix_new (helper_socket);
	g_io_add_watch (helper_channel, G_IO_IN, hald_helper_data, NULL);
	g_io_channel_unref (helper_channel);

	/*
	 * set mount points for /proc and /sys, possibly overridden for testing
	 */
	hal_sysfs_path = getenv ("SYSFS_PATH");
	if (hal_sysfs_path == NULL)
		hal_sysfs_path = "/sys";

	hal_proc_path = getenv ("PROC_PATH");
	if (hal_proc_path == NULL)
		hal_proc_path = "/proc";

	/*
	 * watch /proc/mounts for mount tree changes
	 * kernel 2.6.15 vfs throws a POLLERR event for every change
	 */
	g_snprintf (path, sizeof (path), "%s/mounts", get_hal_proc_path ());
	mounts_channel = g_io_channel_new_file (path, "r", NULL);
	if (mounts_channel == NULL)
		DIE (("Unable to read /proc/mounts"));
	g_io_add_watch (mounts_channel, G_IO_ERR, mount_tree_changed_event, NULL);

	/*
	 *Load various hardware id databases
	 */
	ids_init ();
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
hotplug_queue_now_empty (void)
{
	if (hald_is_initialising)
		osspec_probe_done ();
}


static void
computer_probing_helper_done (HalDevice *d)
{
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

	hal_util_callout_device_add (d, computer_callouts_add_done, NULL, NULL);
}

static void 
computer_probing_pcbios_helper_done (HalDevice *d, guint32 exit_type, 
		                                 gint return_code, gchar **error, 
				                             gpointer data1, gpointer data2)
{
	const char *chassis_type;
	const char *system_manufacturer;
	const char *system_product;
	const char *system_version;

	if ((system_manufacturer = hal_device_property_get_string (d, "smbios.system.manufacturer")) != NULL &&
	    (system_product = hal_device_property_get_string (d, "smbios.system.product")) != NULL &&
	    (system_version = hal_device_property_get_string (d, "smbios.system.version")) != NULL) {
		char buf[128];

		hal_device_property_set_string (d, "system.vendor", system_manufacturer);

		if (strcmp(system_version, "Not Specified" ) != 0 ) {
			g_snprintf (buf, sizeof (buf), "%s %s", system_product, system_version);
			hal_device_property_set_string (d, "system.product", buf);
		} else {
			hal_device_property_set_string (d, "system.product", system_product);
		}
	}


	/* now map the smbios.* properties to our generic system.formfactor property */
	if ((chassis_type = hal_device_property_get_string (d, "smbios.chassis.type")) != NULL) {
		unsigned int i;

		/* Map the chassis type from dmidecode.c to a sensible type used in hal 
		 *
		 * See also 3.3.4.1 of the "System Management BIOS Reference Specification, 
		 * Version 2.3.4" document, available from http://www.dmtf.org/standards/smbios.
		 *
		 * TODO: figure out WTF the mapping should be; "Lunch Box"? Give me a break :-)
		 */
		static const char *chassis_map[] = {
			"Other",                 "unknown",
			"Unknown",               "unknown",
			"Desktop",               "desktop",
			"Low Profile Desktop",   "desktop",
			"Pizza Box",             "server",
			"Mini Tower",            "desktop",
			"Tower",                 "desktop",
			"Portable",              "laptop",
			"Laptop",                "laptop",
			"Notebook",              "laptop",
			"Hand Held",             "handheld",
			"Docking Station",       "laptop",
			"All In One",            "unknown",
			"Sub Notebook",          "laptop",
			"Space-saving",          "unknown",
			"Lunch Box",             "unknown",
			"Main Server Chassis",   "server",
			"Expansion Chassis",     "unknown",
			"Sub Chassis",           "unknown",
			"Bus Expansion Chassis", "unknown",
			"Peripheral Chassis",    "unknown",
			"RAID Chassis",          "unknown",
			"Rack Mount Chassis",    "unknown",
			"Sealed-case PC",        "unknown",
			"Multi-system",          "unknown",
			NULL
		};

		for (i = 0; chassis_map[i] != NULL; i += 2) {
			if (strcmp (chassis_map[i], chassis_type) == 0) {
				/* check if the key is already set to prevent overwrite keys */
				if (!hal_device_has_property(d, "system.formfactor"))
					hal_device_property_set_string (d, "system.formfactor", chassis_map[i+1]);
				break;
			}
		}
	       
	}

	computer_probing_helper_done (d);
}

static void
set_suspend_hibernate_keys (HalDevice *d)
{
	int can_suspend;
	int can_hibernate;
	ssize_t read;
	unsigned int len;
	char *poweroptions;
	FILE *fp;

	can_suspend = FALSE;
	can_hibernate = FALSE;

	/* try to find 'mem' and 'disk' in /sys/power/state */
	fp = fopen ("/sys/power/state", "r");
	if (fp == NULL) {
		HAL_WARNING (("Could not open /sys/power/state"));
		goto out;
	}
	poweroptions = NULL;
	len = 0;
	read = getline (&poweroptions, &len, fp);
	fclose (fp);
	if (poweroptions == NULL) {
		HAL_WARNING (("Contents of /sys/power/state invalid"));
		goto out;
	}
	if (strstr (poweroptions, "mem"))
		can_suspend = TRUE;
	if (strstr (poweroptions, "disk"))
		can_hibernate = TRUE;
	free (poweroptions);

	/* check for the presence of suspend2 */
	if (access ("/proc/software_suspend", F_OK) == 0)
		can_hibernate = TRUE;
	if (access ("/proc/suspend2", F_OK) == 0)
		can_hibernate = TRUE;
out:
	hal_device_property_set_bool (d, "power_management.can_suspend_to_ram", can_suspend);
	hal_device_property_set_bool (d, "power_management.can_suspend_to_disk", can_hibernate);
}

void 
osspec_probe (void)
{
	HalDevice *root;
	struct utsname un;
	gboolean should_decode_dmi;

	should_decode_dmi = FALSE;

	root = hal_device_new ();
	hal_device_property_set_string (root, "info.bus", "unknown");
	hal_device_property_set_string (root, "linux.sysfs_path_device", "(none)");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_property_set_string (root, "info.udi", "/org/freedesktop/Hal/devices/computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");

	if (uname (&un) >= 0) {
		hal_device_property_set_string (root, "system.kernel.name", un.sysname);
		hal_device_property_set_string (root, "system.kernel.version", un.release);
		hal_device_property_set_string (root, "system.kernel.machine", un.machine);
	}

	/* can be overridden by dmidecode, others */
	hal_device_property_set_string (root, "system.formfactor", "unknown");


	/* Let computer be in TDL while synthesizing all other events because some may write to the object */
	hal_device_store_add (hald_get_tdl (), root);

	/* will enqueue hotplug events for entire system */
	HAL_INFO (("Synthesizing sysfs events..."));
	coldplug_synthesize_events ();

	HAL_INFO (("Synthesizing powermgmt events..."));
	if (acpi_synthesize_hotplug_events ()) {
		HAL_INFO (("ACPI capabilities found"));
		should_decode_dmi = TRUE;
	} else if (pmu_synthesize_hotplug_events ()) {
		HAL_INFO (("PMU capabilities found"));		
	} else if (apm_synthesize_hotplug_events ()) {
		HAL_INFO (("APM capabilities found"));		
		should_decode_dmi = TRUE;
	} else {
		HAL_INFO (("No powermgmt capabilities"));		
	}
	HAL_INFO (("Done synthesizing events"));

	/*
	 * Populate the powermgmt keys according to the kernel options.
	 * NOTE: This may not mean the machine is able to suspend
	 *	 or hibernate successfully, only that the machine has
	 *	 support compiled into the kernel.
	 */
	set_suspend_hibernate_keys (root);

	/* TODO: add prober for PowerMac's */
	if (should_decode_dmi) {
		hald_runner_run (root, "hald-probe-smbios", NULL,
                         HAL_HELPER_TIMEOUT,
                         computer_probing_pcbios_helper_done, 
                         NULL, NULL);
	} else {
		/* no probing */
		computer_probing_helper_done (root);
  }

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

gboolean
hal_util_get_driver_name (const char *sysfs_path, gchar *driver_name)
{
	gchar driver_path[HAL_PATH_MAX];
	struct stat statbuf;

	g_snprintf (driver_path, sizeof (driver_path), "%s/driver", sysfs_path);
	if (stat (driver_path, &statbuf) == 0) {
		gchar buf[256];
		memset (buf, '\0', sizeof (buf));
		if (readlink (driver_path, buf, sizeof (buf) - 1) > 0) {
			g_snprintf (driver_name, strlen(buf), "%s", hal_util_get_last_element(buf));
			return TRUE;
		}
	}
	return FALSE;
}

gboolean
hal_util_set_driver (HalDevice *d, const char *property_name, const char *sysfs_path)
{
	gboolean ret;
	gchar driver_name[256];

	memset (driver_name, '\0', sizeof (driver_name));
	ret = hal_util_get_driver_name (sysfs_path, driver_name);
	if (ret == TRUE)
		hal_device_property_set_string (d, property_name, driver_name);

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

