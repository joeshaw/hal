/***************************************************************************
 * CVSID: $Id$
 *
 * osspec.c : New and improved HAL backend for Linux 2.6
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005,2006 Kay Sievers, <kay.sievers@vrfy.org>
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
#define DMI_SYSFS_PATH "/sys/class/dmi/id"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <linux/types.h>
#include <net/if_arp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../hald.h"
#include "../hald_dbus.h"
#include "../hald_runner.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"
#include "../util_helper.h"
#include "../ids.h"

#include "acpi.h"
#include "apm.h"
#include "blockdev.h"
#include "coldplug.h"
#include "hotplug.h"
#include "pmu.h"

#include "osspec_linux.h"

static gboolean hald_done_synthesizing_coldplug = FALSE;

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

	char buf[4096];
	size_t bufpos = 0;
	const char *action = NULL;
	HotplugEvent *hotplug_event;

	memset(buf, 0x00, sizeof (buf));

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

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;

	while (bufpos < sizeof (buf)) {
		size_t keylen;
		char *key;
		char *str, *dstr;

		key = &buf[bufpos];
		keylen = strlen(key);
		if (keylen == 0)
			break;
		bufpos += keylen + 1;

		if (strncmp(key, "ACTION=", 7) == 0)
			action = &key[7];
		else if (strncmp(key, "DEVPATH=", 8) == 0) {

                        /* md devices are handled via looking at /proc/mdstat */
                        if (g_str_has_prefix (key + 8, "/block/md")) {
                                HAL_INFO (("skipping md event for %s", key + 8));
                                goto invalid;
                        }

			g_snprintf (hotplug_event->sysfs.sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path),
				    "/sys%s", &key[8]);
		} else if (strncmp(key, "DEVPATH_OLD=", 12) == 0) {

                        /* md devices are handled via looking at /proc/mdstat */
                        if (g_str_has_prefix (key + 12, "/block/md")) {
                                HAL_INFO (("skipping md event for %s", key + 8));
                                goto invalid;
                        }

			g_snprintf (hotplug_event->sysfs.sysfs_path_old, sizeof (hotplug_event->sysfs.sysfs_path_old),
				    "/sys%s", &key[12]);
		} else if (strncmp(key, "SUBSYSTEM=", 10) == 0)
			g_strlcpy (hotplug_event->sysfs.subsystem, &key[10], sizeof (hotplug_event->sysfs.subsystem));
		else if (strncmp(key, "DEVNAME=", 8) == 0)
			g_strlcpy (hotplug_event->sysfs.device_file, &key[8], sizeof (hotplug_event->sysfs.device_file));
		else if (strncmp(key, "SEQNUM=", 7) == 0)
			hotplug_event->sysfs.seqnum = strtoull(&key[7], NULL, 10);
		else if (strncmp(key, "IFINDEX=", 8) == 0)
			hotplug_event->sysfs.net_ifindex = strtoul(&key[8], NULL, 10);
		else if (strncmp(key, "ID_VENDOR=", 10) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[10])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.vendor, str, sizeof(hotplug_event->sysfs.vendor));
				g_free (str);
			}
		} else if (strncmp(key, "ID_MODEL=", 9) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[9])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.model, str, sizeof(hotplug_event->sysfs.model));
				g_free (str);
			}
		} else if (strncmp(key, "ID_REVISION=", 12) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[12])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.revision, str, sizeof(hotplug_event->sysfs.revision));
				g_free (str);
			}
		} else if (strncmp(key, "ID_SERIAL=", 10) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[10])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.serial, str, sizeof(hotplug_event->sysfs.serial));
				g_free (str);
			}
		} else if (strncmp(key, "ID_FS_USAGE=", 12) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[12])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.fsusage, str, sizeof(hotplug_event->sysfs.fsusage));
				g_free (str);
			}
		} else if (strncmp(key, "ID_FS_TYPE=", 11) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[11])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.fstype, str, sizeof(hotplug_event->sysfs.fstype));
				g_free (str);
			}
		} else if (strncmp(key, "ID_FS_VERSION=", 14) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[14])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.fsversion, str, sizeof(hotplug_event->sysfs.fsversion));
				g_free (str);
			}
		} else if (strncmp(key, "ID_FS_UUID=", 11) == 0) {
			if ((str = hal_util_strdup_valid_utf8(&key[11])) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.fsuuid, str, sizeof(hotplug_event->sysfs.fsuuid));
				g_free (str);
			}
		} else if (strncmp(key, "ID_FS_LABEL_ENC=", 16) == 0) {
			dstr = g_malloc0 (keylen - 15);
			hal_util_decode_escape (&key[16], dstr, sizeof(hotplug_event->sysfs.fslabel));

			if ((str = hal_util_strdup_valid_utf8(dstr)) != NULL ) {
				g_strlcpy (hotplug_event->sysfs.fslabel, str, sizeof(hotplug_event->sysfs.fslabel));
				g_free (str);
			}
			g_free (dstr);
		}
	}

	if (!action) {
		HAL_INFO (("missing ACTION"));
		goto invalid;
	}
	if (hotplug_event->sysfs.sysfs_path[0] == '\0') {
		HAL_INFO (("missing DEVPATH"));
		goto invalid;
	}
	if (hotplug_event->sysfs.subsystem[0] == '\0') {
		HAL_INFO (("missing SUBSYSTEM"));
		goto invalid;
	}

	/* This is a workaround for temporary cryptsetup devices, HAL should ignore them. 
	 * There is already a fix for this issue in the udev git master as soon as a new
	 * udev release is available and HAL starts to depend on the new version we should 
	 * remove this again. (added 2008-03-04)
	 */
	if (strncmp (hotplug_event->sysfs.device_file, "/dev/mapper/temporary-cryptsetup-", 33) == 0) {
		HAL_INFO (("Temporary workaround: ignoring temporary cryptsetup file"));
		goto invalid;
	}
	if (strstr (hotplug_event->sysfs.device_file, "/dm-") != NULL) {
		HAL_DEBUG (("Found a dm-device (%s), mark it", hotplug_event->sysfs.device_file));
		hotplug_event->sysfs.is_dm_device = TRUE;
	}
		

	HAL_INFO (("SEQNUM=%lld, ACTION=%s, SUBSYSTEM=%s, DEVPATH=%s, DEVNAME=%s, IFINDEX=%d",
		   hotplug_event->sysfs.seqnum, action, hotplug_event->sysfs.subsystem, hotplug_event->sysfs.sysfs_path,
		   hotplug_event->sysfs.device_file, hotplug_event->sysfs.net_ifindex));

	/* ignore module and driver events, until we really need them */
	if ((strcmp (hotplug_event->sysfs.subsystem, "drivers") != 0) && 
	    (strcmp (hotplug_event->sysfs.subsystem, "module") != 0)) {
		if (strcmp (action, "add") == 0) {
			hotplug_event->action = HOTPLUG_ACTION_ADD;
			hotplug_event_enqueue (hotplug_event);
			hotplug_event_process_queue ();
			goto out;
		}

		if (strcmp (action, "change") == 0) {
			hotplug_event->action = HOTPLUG_ACTION_CHANGE;
			hotplug_event_enqueue (hotplug_event);
			hotplug_event_process_queue ();
			goto out;
		}

		if (strcmp (action, "move") == 0) {
			hotplug_event->action = HOTPLUG_ACTION_MOVE;
			hotplug_event_enqueue (hotplug_event);
			hotplug_event_process_queue ();
			goto out;
		}

		if (strcmp (action, "remove") == 0) {
			hotplug_event->action = HOTPLUG_ACTION_REMOVE;
			hotplug_event_enqueue (hotplug_event);
			hotplug_event_process_queue ();
			goto out;
		}
	}

invalid:
	g_slice_free (HotplugEvent, hotplug_event);

out:
	return TRUE;
}

static gboolean
mount_tree_changed_event (GIOChannel *channel, GIOCondition cond, gpointer user_data)
{
	if (cond & ~G_IO_ERR)
		return TRUE;

	HAL_INFO (("/proc/mounts tells, that the mount has tree changed"));
	blockdev_refresh_mount_state (NULL);

	return TRUE;
}

static gboolean
mdstat_changed_event (GIOChannel *channel, GIOCondition cond, gpointer user_data)
{
	if (cond & ~G_IO_PRI)
		return TRUE;

	HAL_INFO (("/proc/mdstat changed"));

        blockdev_process_mdstat ();

	return TRUE;
}

static HalFileMonitor *file_monitor = NULL;

HalFileMonitor *
osspec_get_file_monitor (void)
{
        return file_monitor;
}

static GIOChannel *mdstat_channel = NULL;

GIOChannel *get_mdstat_channel (void)
{
        return mdstat_channel;
}

/* 
 * NOTE: We need to use this function to parse if HAL is privileged 
 *       since some of the dmi keys in sysfs are only readable by root
 */
static void 
osspec_privileged_init_preparse_set_dmi (gboolean set, HalDevice *d) 
{
	gchar *buf;
	static char *product_serial;
	static char *product_uuid;
	static char *board_serial;
	static gboolean parsed = FALSE; 

	if (g_file_test (DMI_SYSFS_PATH, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {

		if (!set){
			if ((buf = hal_util_get_string_from_file(DMI_SYSFS_PATH, "product_serial")) != NULL)
				product_serial = g_strdup ( buf );
			if ((buf = hal_util_get_string_from_file(DMI_SYSFS_PATH, "product_uuid")) != NULL)
				product_uuid = g_strdup ( buf );
			if ((buf = hal_util_get_string_from_file(DMI_SYSFS_PATH, "board_serial")) != NULL)
				board_serial = g_strdup ( buf );

			parsed = TRUE;
		} else {
			if (d != NULL && parsed) {
				hal_device_property_set_string (d, "system.hardware.serial", product_serial);		
				hal_device_property_set_string (d, "system.hardware.uuid", product_uuid);
				hal_device_property_set_string (d, "system.board.serial", board_serial);
				g_free (product_serial);
				g_free (product_uuid);
				g_free (board_serial);
				parsed = FALSE;
			}
		}
	}
}

void
osspec_privileged_init (void)
{
	GError *err = NULL;

        file_monitor = hal_file_monitor_new ();
        if (file_monitor == NULL) {
                DIE (("Cannot initialize file monitor"));
        }

	/* watch /proc/mdstat for md changes
	 * kernel 2.6.19 throws a POLLPRI event for every change
	 */
	mdstat_channel = g_io_channel_new_file ("/proc/mdstat", "r", &err);
	if (mdstat_channel != NULL) {
		g_io_add_watch (mdstat_channel, G_IO_PRI, mdstat_changed_event, NULL);
	} else {
		if (err != NULL)
			HAL_WARNING (("Unable to open /proc/mdstat: %s", err->message));

		/* if its not a reasonable error, abort */
		if (!g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			DIE (("Unable to read /proc/mdstat"));
	}
	
	if (err != NULL)
		g_error_free (err);

	osspec_privileged_init_preparse_set_dmi(FALSE, NULL);
}

void
osspec_init (void)
{
	int udev_socket;
	struct sockaddr_un saddr;
	socklen_t addrlen;
	const int on = 1;
	GIOChannel *udev_channel;
	GIOChannel *mounts_channel;

	/*
	 * setup socket for listening from messages from udev
	 */

	hal_device_store_index_property (hald_get_gdl (), "linux.sysfs_path");

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

	/* watch /proc/mounts for mount tree changes
	 * kernel 2.6.15 vfs throws a POLLERR event for every change
	 */
	mounts_channel = g_io_channel_new_file ("/proc/mounts", "r", NULL);
	if (mounts_channel == NULL)
		DIE (("Unable to read /proc/mounts"));
	g_io_add_watch (mounts_channel, G_IO_ERR, mount_tree_changed_event, NULL);

	/*
	 *Load various hardware id databases
	 */
	ids_init ();

	/* watch fdi directories */
	/* TODO: temporarily disabled... 
           watch_fdi_files (); */
}


static void 
computer_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);
}

void
hotplug_queue_now_empty (void)
{
	if (hald_is_initialising && hald_done_synthesizing_coldplug) {
		osspec_probe_done ();
        }
}


static void
computer_probing_helper_done (HalDevice *d)
{
	/* check if this is may a laptop */
	if (!hal_device_has_property (d, "system.formfactor") || 
	    (strcmp (hal_device_property_get_string (d, "system.formfactor"), "laptop") != 0)) {
		HAL_INFO (("Check if the machine is may a laptop ..."));
		acpi_check_is_laptop("BATTERY");
		acpi_check_is_laptop("LID");
	}
	/* if not set, set a default value */
	if (!hal_device_has_property (d, "system.formfactor")) {
		hal_device_property_set_string (d, "system.formfactor", "unknown");
	}

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

        hald_done_synthesizing_coldplug = TRUE;

	/* we try again to match again on computer, now we have done coldplug
	 * and completed probing. In an ideal world, we would do this before
	 * _and_ after the coldplug, but this seems to work well. */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

	hal_util_callout_device_add (d, computer_callouts_add_done, NULL, NULL);
}

static void 
computer_dmi_map (HalDevice *d, gboolean dmidecode) 
{
	/* Map the chassis type from dmidecode.c to a sensible type used in hal 
	 *
	 * See also 3.3.4.1 of the "System Management BIOS Reference Specification, 
	 * Version 2.6.1" (Preliminary Standard) document, available from 
	 * http://www.dmtf.org/standards/smbios.
	 *
	 * TODO: figure out WTF the mapping should be; "Lunch Box"? Give me a break :-)
	 */
	static const char *chassis_map[] = {
		"Other",                 "unknown", /* 0x01 */
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
		"Space-saving",          "desktop",
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
		"CompactPCI",		 "unknonw",
		"AdvancedTCA",		 "unknown", 
		"Blade",		 "server",
		"Blade Enclosure"	 "unknown", /* 0x1D */
		NULL
	};


	if (dmidecode) {
		/* do mapping from text to text type */ 
		unsigned int i;
		const char *chassis_type;

		/* now map the smbios.* properties to our generic system.formfactor property */
		if ((chassis_type = hal_device_property_get_string (d, "system.chassis.type")) != NULL) {
			
			for (i = 0; chassis_map[i] != NULL; i += 2) {
				if (strcmp (chassis_map[i], chassis_type) == 0) {
					hal_device_property_set_string (d, "system.formfactor", chassis_map[i+1]);
					break;
				}
			}
		} 
	} else {
		gint chassis_type;

		/* do mapping from integer type to text type*/
		/* get the chassis type and map it to the related text info */
		if (hal_util_get_int_from_file(DMI_SYSFS_PATH, "chassis_type", &chassis_type, 10)) {

			if ((chassis_type > 0) && (chassis_type < 28) && (chassis_map[(chassis_type-1)*2] != NULL)) {
				hal_device_property_set_string (d, "system.chassis.type", chassis_map[((chassis_type-1)*2)]);
				hal_device_property_set_string (d, "system.formfactor", chassis_map[((chassis_type-1)*2)+1]);
			}

		} else {
			hal_device_property_set_string (d, "system.chassis.type", "Unknown");
			hal_device_property_set_string (d, "system.formfactor", "unknown");
		}
	}

}

static void 
computer_probing_pcbios_helper_done (HalDevice *d, guint32 exit_type, 
	                             gint return_code, gchar **error, 
				     gpointer data1, gpointer data2)
{
	const char *system_manufacturer;
	const char *system_product;
	const char *system_version;

	if (exit_type == HALD_RUN_FAILED) {
		goto out;
	}

	if ((system_manufacturer = hal_device_property_get_string (d, "system.hardware.vendor")) != NULL &&
	    (system_product = hal_device_property_get_string (d, "system.hardware.product")) != NULL &&
	    (system_version = hal_device_property_get_string (d, "system.hardware.version")) != NULL) {
		char buf[128];

		if (strcmp(system_version, "Not Specified" ) != 0 ) {
			g_snprintf (buf, sizeof (buf), "%s %s", system_product, system_version);
			hal_device_property_set_string (d, "system.product", buf);
		} else {
			hal_device_property_set_string (d, "system.product", system_product);
		}
	}


	if (!hal_device_has_property (d, "system.formfactor")) {
		computer_dmi_map (d, TRUE);
	}
out:
	computer_probing_helper_done (d);
}

static void
set_suspend_hibernate_keys (HalDevice *d)
{
	gboolean can_suspend;
	gboolean can_hibernate;
	char *poweroptions;
	const char *pmtype;

	can_suspend = FALSE;
	can_hibernate = FALSE;

	/* try to find 'mem' and 'disk' in /sys/power/state */
	poweroptions = hal_util_get_string_from_file("/sys/power/", "state");
	if (poweroptions == NULL) {
		HAL_WARNING (("Contents of /sys/power/state invalid"));
		goto out;
	}
	if (strstr (poweroptions, "mem"))
		can_suspend = TRUE;
	if (strstr (poweroptions, "disk"))
		can_hibernate = TRUE;

	if (!can_suspend) {
		pmtype = hal_device_property_get_string (d, "power_management.type");
		if (pmtype != NULL && strcmp(pmtype, "pmu") == 0) {
			/* We got our own helper for suspend PMU machines */
			can_suspend = TRUE;
		}
	}

	if (!can_hibernate) {
		/* check for the presence of suspend2 */
		if (access ("/proc/software_suspend", F_OK) == 0) {
			can_hibernate = TRUE;
		} else if (access ("/proc/suspend2", F_OK) == 0) {
			can_hibernate = TRUE;
		} else if (access ("/sys/power/suspend2/version", F_OK) == 0) {
			can_hibernate = TRUE;
		}
	}
out:
	hal_device_property_set_bool (d, "power_management.can_suspend", can_suspend);
	hal_device_property_set_bool (d, "power_management.can_suspend_hybrid", FALSE);
	hal_device_property_set_bool (d, "power_management.can_hibernate", can_hibernate);
}

static void
get_openfirmware_entry(HalDevice *d, char *property, char *entry, 
                       gboolean multivalue) 
{
	char *contents;
	gsize length;
	if (!g_file_get_contents(entry, &contents, &length, NULL)) {
		return;
	}
	if (multivalue) {
		gsize offset = 0;
		while (offset < length) { 
			hal_device_property_strlist_append(d, property, contents + offset, FALSE);
			for (; offset < length - 1 && contents[offset] != '\0'; offset++)
				;
			offset++;
		}
	} else {
		hal_device_property_set_string(d, property, contents);
	}
	free(contents);
}

static void
detect_openfirmware_formfactor(HalDevice *root) 
{
	int x;
	struct { gchar *model; gchar *formfactor; } model_formfactor[] =
		{ 
			{ "RackMac"   , "server" },
			{ "AAPL,3400" , "laptop"  },
			{ "AAPL,3500" , "laptop"  },
			{ "PowerBook" , "laptop"  },
			{ "AAPL"      , "desktop" },
			{ "iMac"      , "desktop" },
			{ "PowerMac"  , "desktop" },
			{ "Pegasos"   , "desktop" },
			{NULL, NULL }
		};
	const gchar *model =
	  hal_device_property_get_string(root, "openfirmware.model");
	if (model == NULL) 
		return;
	for (x = 0 ; model_formfactor[x].model ; x++) {
		if (strstr(model, model_formfactor[x].model)) {
			hal_device_property_set_string (root, "system.formfactor",
				model_formfactor[x].formfactor);
			break;
		}
	}
}

static gboolean
decode_dmi_from_openfirmware (HalDevice *root)
{
#define DEVICE_TREE "/proc/device-tree/"
	if (!g_file_test(DEVICE_TREE, G_FILE_TEST_IS_DIR))
		return FALSE;

	get_openfirmware_entry(root, "openfirmware.model", 
		DEVICE_TREE "model", FALSE);
	get_openfirmware_entry(root, "openfirmware.compatible", 
		DEVICE_TREE "compatible", TRUE);
	detect_openfirmware_formfactor(root);
	return TRUE;
}

static gboolean
decode_dmi_from_sysfs (HalDevice *d)
{
	if (!g_file_test (DMI_SYSFS_PATH, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)))
		return FALSE;

	osspec_privileged_init_preparse_set_dmi(TRUE, d);
	hal_util_set_string_from_file(d, "system.firmware.vendor", DMI_SYSFS_PATH, "bios_vendor");
	hal_util_set_string_from_file(d, "system.firmware.version", DMI_SYSFS_PATH, "bios_version");
	hal_util_set_string_from_file(d, "system.firmware.release_date", DMI_SYSFS_PATH, "bios_date");
	hal_util_set_string_from_file(d, "system.hardware.vendor", DMI_SYSFS_PATH, "sys_vendor");
	hal_util_set_string_from_file(d, "system.hardware.product", DMI_SYSFS_PATH, "product_name");
	hal_util_set_string_from_file(d, "system.hardware.version", DMI_SYSFS_PATH, "product_version");
	hal_util_set_string_from_file(d, "system.chassis.manufacturer", DMI_SYSFS_PATH, "chassis_vendor");
	hal_util_set_string_from_file(d, "system.board.product", DMI_SYSFS_PATH, "board_name");
	hal_util_set_string_from_file(d, "system.board.version", DMI_SYSFS_PATH, "board_version");
	hal_util_set_string_from_file(d, "system.board.vendor", DMI_SYSFS_PATH, "board_vendor");
	computer_dmi_map (d, FALSE);

	return TRUE;
}

static void
decode_dmi (HalDevice *d)
{
	/* try to get the dmi infos from sysfs instead of call dmidecode*/
	if (decode_dmi_from_sysfs(d) ||
	    decode_dmi_from_openfirmware (d)) {
		HAL_INFO (("got DMI from files"));
		computer_probing_helper_done (d);
	} else {
		if (g_file_test ("/usr/sbin/dmidecode", G_FILE_TEST_IS_EXECUTABLE) ||
		    g_file_test ("/bin/dmidecode", G_FILE_TEST_IS_EXECUTABLE) ||
		    g_file_test ("/sbin/dmidecode", G_FILE_TEST_IS_EXECUTABLE) ||
		    g_file_test ("/usr/local/sbin/dmidecode", G_FILE_TEST_IS_EXECUTABLE)) {
			HAL_INFO (("getting DMI from prober"));
			hald_runner_run (d, "hald-probe-smbios", NULL, HAL_HELPER_TIMEOUT,
					 computer_probing_pcbios_helper_done, NULL, NULL);
		} else {
			/* no probing possible */
			HAL_INFO (("failed to probe DMI"));
			computer_probing_helper_done (d);
		}
	}
}

static void 
computer_probing_pm_is_supported_helper_done (HalDevice *d, guint32 exit_type, 
                                              gint return_code, gchar **error, 
                                              gpointer data1, gpointer data2)
{
	HAL_INFO (("In computer_probing_pm_is_supported_helper_done"));
	decode_dmi (d);
}

static void
get_primary_videocard (HalDevice *d)
{
        GDir *dir;
        const char *name;

        dir = g_dir_open ("/sys/bus/pci/devices", 0, NULL);
        if (dir == NULL)
                goto out;
        while ((name = g_dir_read_name (dir)) != NULL) {
                int class;
                char *path;
                path = g_strdup_printf ("/sys/bus/pci/devices/%s", name);
                if (hal_util_get_int_from_file (path, "class", &class, 0) && (class&0xffff00) == 0x030000 ) {
                        int vendor, device;
                        if (hal_util_get_int_from_file (path, "vendor", &vendor, 0) &&
                            hal_util_get_int_from_file (path, "device", &device, 0)) {
                                HAL_INFO (("got %x:%x as primary videocard", vendor, device));
                                hal_device_property_set_int (d, "system.hardware.primary_video.vendor", vendor);
                                hal_device_property_set_int (d, "system.hardware.primary_video.product", device);
                                g_free (path);
                                g_dir_close (dir);
                                goto out;
                        }
                }
                g_free (path);
        }
        g_dir_close (dir);
out:
        ;
}

void 
osspec_probe (void)
{
	HalDevice *root;
	struct utsname un;

	hald_runner_set_method_run_notify ((HaldRunnerRunNotify) hotplug_event_process_queue, NULL);
	root = hal_device_new ();
	hal_device_property_set_string (root, "info.subsystem", "unknown");
	hal_device_property_set_string (root, "info.product", "Computer");
	hal_device_set_udi (root, "/org/freedesktop/Hal/devices/computer");

	if (PACKAGE_VERSION) {
		int major, minor, micro;

		hal_device_property_set_string (root, "org.freedesktop.Hal.version", PACKAGE_VERSION);
		if ( sscanf( PACKAGE_VERSION, "%d.%d.%d", &major, &minor, &micro ) == 3 ) {
			hal_device_property_set_int (root, "org.freedesktop.Hal.version.major", major);
                        hal_device_property_set_int (root, "org.freedesktop.Hal.version.minor", minor);
                        hal_device_property_set_int (root, "org.freedesktop.Hal.version.micro", micro);
		}
	}

	if (uname (&un) >= 0) {
		hal_device_property_set_string (root, "system.kernel.name", un.sysname);
		hal_device_property_set_string (root, "system.kernel.version", un.release);
		if (un.release != NULL && un.release[0] != '\0') {
                        int major, minor, micro ;

			/* check if we can parse the major.minor.micro info and ignore the rest */
                        if ( sscanf( un.release, "%d.%d.%d", &major, &minor, &micro ) >= 3 ) {
				hal_device_property_set_int (root, "system.kernel.version.major", major);
				hal_device_property_set_int (root, "system.kernel.version.minor", minor);
				hal_device_property_set_int (root, "system.kernel.version.micro", micro);
			}
                }

		hal_device_property_set_string (root, "system.kernel.machine", un.machine);
	}

	/* Let computer be in TDL while synthesizing all other events because some may write to the object */
	hal_device_store_add (hald_get_tdl (), root);

	/*
	 * Populate the powermgmt keys according to the kernel options.
	 * NOTE: This may not mean the machine is able to suspend
	 *	 or hibernate successfully, only that the machine has
	 *	 support compiled into the kernel.
	 */
	set_suspend_hibernate_keys (root);

        /* set the vendor/product of primary video card */
        get_primary_videocard (root);

        /* Try and set the suspend/hibernate keys using pm-is-supported
         */
        if (g_file_test ("/usr/bin/pm-is-supported", G_FILE_TEST_IS_EXECUTABLE)) {
                hald_runner_run (root, "hal-system-power-pm-is-supported", NULL, HAL_HELPER_TIMEOUT,
                                 computer_probing_pm_is_supported_helper_done, NULL, NULL);
        } else {
                decode_dmi (root);
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

	if (sysfs_path == NULL) {
		HAL_WARNING (("hal_util_get_driver_name: sysfs_path == NULL"));
		return FALSE;
	}

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

	if (d == NULL || property_name == NULL || sysfs_path == NULL) {
		HAL_WARNING (("hal_util_set_driver: d, property_name or sysfs_path == NULL"));
		return FALSE;
	}

	memset (driver_name, '\0', sizeof (driver_name));
	ret = hal_util_get_driver_name (sysfs_path, driver_name);
	if (ret == TRUE)
		hal_device_property_set_string (d, property_name, driver_name);

	return ret;
}

static gboolean get_parent_device(char *path)
{
	/* go up one directory */
	if (!hal_util_path_ascend (path))
		return FALSE;
	if (g_str_has_suffix (path, "/class"))
		return FALSE;
	if (g_str_has_suffix (path, "/block"))
		return FALSE;
	if (g_str_has_suffix (path, "/devices"))
		return FALSE;
	return TRUE;
}

/* return the first already known parent device */
gboolean
hal_util_find_known_parent (const gchar *sysfs_path, HalDevice **parent, gchar **parent_path)
{
	gchar *target;
	HalDevice *parent_dev = NULL;
	gchar *parent_devpath;
	char parentdevpath[HAL_PATH_MAX];
	gboolean retval = FALSE;

	parent_devpath = g_strdup (sysfs_path);
	while (TRUE) {
		if (!get_parent_device (parent_devpath))
			break;

		parent_dev = hal_device_store_match_key_value_string (hald_get_gdl (),
								      "linux.sysfs_path",
								      parent_devpath);
		if (parent_dev != NULL)
			goto out;
	}
	g_free (parent_devpath);
	parent_devpath = NULL;

	/* try if the parent chain is constructed by the device-link */
	g_snprintf (parentdevpath, HAL_PATH_MAX, "%s/device", sysfs_path);
	if ((target = hal_util_readlink (parentdevpath)) != NULL) {
		parent_devpath = hal_util_get_normalized_path (sysfs_path, target);

		while (TRUE) {
			parent_dev = hal_device_store_match_key_value_string (hald_get_gdl (),
									      "linux.sysfs_path",
									      parent_devpath);
			if (parent_dev != NULL)
				goto out;

			/* go up one directory */
			if (!get_parent_device (parent_devpath))
				break;
		}
		g_free (parent_devpath);
		parent_devpath = NULL;
	}

out:
	if (parent_dev != NULL) {
		HAL_INFO (("hal_util_find_known_parent: '%s'->'%s'", sysfs_path, parent_devpath));
		retval = TRUE;
	}
	if (parent != NULL)
		*parent = parent_dev;
	if (parent_path != NULL)
		*parent_path = parent_devpath;
	else
		g_free (parent_devpath);
	return retval;
}

void
osspec_refresh_mount_state_for_block_device (HalDevice *d)
{
	blockdev_refresh_mount_state (d);
}
