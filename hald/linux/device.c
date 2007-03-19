/***************************************************************************
 * Linux kernel device handling
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Richard Hughes, <richard@hughsie.com>
 * Copyright (C) 2005 Danny Kukawka, <danny.kukawka@web.de>
 * Copyright (C) 2006 Kay Sievers <kay.sievers@vrfy.org>
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
#include <limits.h>
#include <sys/socket.h>   /* for ifru_* has incomplete type */
#include <linux/types.h>
#include <linux/if_arp.h> /* for ARPHRD_... */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <asm/byteorder.h>
#include <fcntl.h>
#include <linux/input.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../device_store.h"
#include "../hald.h"
#include "../hald_runner.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"
#include "../ids.h"

#include "coldplug.h"
#include "hotplug_helper.h"
#include "osspec_linux.h"

#include "device.h"

/*--------------------------------------------------------------------------------------------------------------*/

/* this is kinda messy... but acpi.c + friends use this */
gboolean _have_sysfs_lid_button = FALSE;
gboolean _have_sysfs_power_button = FALSE;
gboolean _have_sysfs_sleep_button = FALSE;

/* we must use this kernel-compatible implementation */
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

static int
input_str_to_bitmask (const char *s, long *bitmask, size_t max_size)
{
	int i, j;
	char **v;
	int num_bits_set = 0;

	memset (bitmask, 0, max_size);
	v = g_strsplit (s, " ", max_size);
	for (i = g_strv_length (v) - 1, j = 0; i >= 0; i--, j++) {
		unsigned long val;

		val = strtoul (v[i], NULL, 16);
		bitmask[j] = val;

		while (val != 0) {
			num_bits_set++;
			val &= (val - 1);
		}
	}

	return num_bits_set;
}

static void
input_test_rel (HalDevice *d, const char *sysfs_path)
{
	char *s;
	long bitmask[NBITS(REL_MAX)];
	int num_bits;

	s = hal_util_get_string_from_file (sysfs_path, "../capabilities/rel");
	if (s == NULL)
		goto out;

	num_bits = input_str_to_bitmask (s, bitmask, sizeof (bitmask));

	/* TODO: this test can be improved */
	if (test_bit (REL_X, bitmask) && test_bit (REL_Y, bitmask)) {
		hal_device_add_capability (d, "input.mouse");
	}
out:
	;
}

static void
input_test_key (HalDevice *d, const char *sysfs_path)
{
	int i;
	char *s;
	long bitmask[NBITS(KEY_MAX)];
	int num_bits;

	s = hal_util_get_string_from_file (sysfs_path, "../capabilities/key");
	if (s == NULL)
		goto out;

	num_bits = input_str_to_bitmask (s, bitmask, sizeof (bitmask));

	if (num_bits == 1) {
		/* this is for input devices originating from the ACPI layer et. al. */

		/* TODO: potentially test for BUS_HOST */

		hal_device_add_capability (d, "button");
		hal_device_property_set_bool (d, "button.has_state", FALSE);
		if (test_bit (KEY_POWER, bitmask)) {
			hal_device_property_set_string (d, "button.type", "power");
			_have_sysfs_power_button = TRUE;
		} else if (test_bit (KEY_SLEEP, bitmask)) {
			hal_device_property_set_string (d, "button.type", "sleep");
			_have_sysfs_sleep_button = TRUE;
		} else if (test_bit (KEY_SUSPEND, bitmask)) {
			hal_device_property_set_string (d, "button.type", "hibernate");
		}
	} else {
		/* TODO: we probably should require lots of bits set to classify as keyboard. Oh well */

		/* All keys that are not buttons are less than BTN_MISC */
		for (i = KEY_RESERVED + 1; i < BTN_MISC; i++) {
			if (test_bit (i, bitmask)) {
				hal_device_add_capability (d, "input.keyboard");
				break;
			}
		}
	}
out:
	;
}

static void
input_test_switch (HalDevice *d, const char *sysfs_path)
{
	char *s;
	long bitmask[NBITS(SW_MAX)];
	int num_bits;

	s = hal_util_get_string_from_file (sysfs_path, "../capabilities/sw");
	if (s == NULL)
		goto out;

	num_bits = input_str_to_bitmask (s, bitmask, sizeof (bitmask));
	if (num_bits <= 0)
		goto out;

	hal_device_add_capability (d, "input.switch");
	if (num_bits == 1) {
		hal_device_add_capability (d, "button");
		hal_device_property_set_bool (d, "button.has_state", TRUE);
		/* NOTE: button.state.value will be set from our prober in hald/linux/probing/probe-input.c */
		hal_device_property_set_bool (d, "button.state.value", FALSE);
		if (test_bit (SW_LID, bitmask)) {
			hal_device_property_set_string (d, "button.type", "lid");
			_have_sysfs_lid_button = TRUE;
		} else if (test_bit (SW_TABLET_MODE, bitmask)) {
			hal_device_property_set_string (d, "button.type", "tablet_mode");
		} else if (test_bit (SW_HEADPHONE_INSERT, bitmask)) {
			hal_device_property_set_string (d, "button.type", "headphone_insert");
		}
	}

out:
	;
}

static void
input_test_abs (HalDevice *d, const char *sysfs_path)
{
	char *s;
	long bitmask[NBITS(ABS_MAX)];
	int num_bits;

	s = hal_util_get_string_from_file (sysfs_path, "../capabilities/abs");
	if (s == NULL)
		goto out;
	num_bits = input_str_to_bitmask (s, bitmask, sizeof (bitmask));

	if (test_bit(ABS_X, bitmask) && !test_bit(ABS_Y, bitmask)) {
		long bitmask_touch[NBITS(KEY_MAX)];

		hal_device_add_capability (d, "input.joystick");

		s = hal_util_get_string_from_file (sysfs_path, "../capabilities/key");
		if (s == NULL)
			goto out;
		input_str_to_bitmask (s, bitmask_touch, sizeof (bitmask_touch));

		if (test_bit(BTN_TOUCH, bitmask_touch)) {
			hal_device_add_capability (d, "input.tablet");
		}
	}
out:
	;
}

static HalDevice *
input_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	int eventdev_num;
	HalDevice *d = NULL;

	if (device_file == NULL)
		goto out;

	/* only care about evdev input devices */
	if (sscanf (hal_util_get_last_element (sysfs_path), "event%d", &eventdev_num) != 1)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "input.originating_device", hal_device_get_udi (parent_dev));
		hal_device_property_set_string (d, "input.physical_device", hal_device_get_udi (parent_dev));
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}
	hal_device_property_set_string (d, "info.category", "input");
	hal_device_add_capability (d, "input");

	hal_device_property_set_string (d, "input.device", device_file);

	hal_util_set_string_from_file (d, "info.product", sysfs_path, "../name");
	hal_util_set_string_from_file (d, "input.product", sysfs_path, "../name");

	/* check for keys */
	input_test_key (d, sysfs_path);

	/* check for mice etc. */
	input_test_rel (d, sysfs_path);

	/* check for joysticks etc. */
	input_test_abs (d, sysfs_path);

	/* check for switches */
	input_test_switch (d, sysfs_path);

out:
	return d;
}

static const gchar *
input_get_prober (HalDevice *d)
{
	const char *prober = NULL;

	/* need privileges to check state of switch */
	if (hal_device_property_get_bool (d, "button.has_state")) {
		prober = "hald-probe-input";
	}

	return prober;
}

static gboolean
input_post_probing (HalDevice *d)
{
	return TRUE;
}

static gboolean
input_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_logicaldev_input",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
bluetooth_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, 
	       const gchar *parent_path)
{
	HalDevice *d;

	d = NULL;

	if (parent_dev == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_device_property_set_string (d, "info.category", "bluetooth_hci");
	hal_device_add_capability (d, "bluetooth_hci");

	hal_device_property_set_string (d, "bluetooth_hci.originating_device", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "bluetooth_hci.physical_device", hal_device_get_udi (parent_dev));
	hal_util_set_string_from_file (d, "bluetooth_hci.interface_name", sysfs_path, "name");

	hal_device_property_set_string (d, "info.product", "Bluetooth Host Controller Interface");

out:
	return d;
}

static gboolean
bluetooth_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_bluetooth_hci",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
net_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *ifname;
	guint media_type;
	gint flags;

	d = NULL;

	if (parent_dev == NULL)
		goto error;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_device_property_set_string (d, "info.category", "net");
	hal_device_add_capability (d, "net");

	hal_device_property_set_string (d, "net.originating_device", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "net.physical_device", hal_device_get_udi (parent_dev));

	ifname = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "net.interface", ifname);

	if (!hal_util_set_string_from_file (d, "net.address", sysfs_path, "address")) {
		hal_device_property_set_string (d, "net.address", "00:00:00:00:00:00");	
	}

	if (!hal_util_set_int_from_file (d, "net.linux.ifindex", sysfs_path, "ifindex", 10))
		goto error;

	if (!hal_util_set_int_from_file (d, "net.arp_proto_hw_id", sysfs_path, "type", 10))
		goto error;

	if (!hal_util_get_int_from_file (sysfs_path, "flags", &flags, 16))
		goto error;

	media_type = hal_device_property_get_int (d, "net.arp_proto_hw_id");
	if (media_type == ARPHRD_ETHER) {
		const char *addr;
		char wireless_path[HAL_PATH_MAX];
		char wiphy_path[HAL_PATH_MAX];
		gboolean is_wireless;
		struct stat s;

		snprintf (wireless_path, HAL_PATH_MAX, "%s/wireless", sysfs_path);
		/* wireless dscape stack e.g. from rt2500pci driver*/
		snprintf (wiphy_path, HAL_PATH_MAX, "%s/wiphy", sysfs_path);

                if ((stat (wireless_path, &s) == 0 && (s.st_mode & S_IFDIR)) ||
		    (stat (wiphy_path, &s) == 0 && (s.st_mode & S_IFDIR))) { 
			hal_device_property_set_string (d, "info.product", "WLAN Interface");
			hal_device_property_set_string (d, "info.category", "net.80211");
			hal_device_add_capability (d, "net.80211");
			is_wireless = TRUE;
		} else {
			hal_device_property_set_string (d, "info.product", "Networking Interface");
			hal_device_property_set_string (d, "info.category", "net.80203");
			hal_device_add_capability (d, "net.80203");
			is_wireless = FALSE;
		}

		addr = hal_device_property_get_string (d, "net.address");
		if (addr != NULL) {
			unsigned int a5, a4, a3, a2, a1, a0;
			
			if (sscanf (addr, "%x:%x:%x:%x:%x:%x",
				    &a5, &a4, &a3, &a2, &a1, &a0) == 6) {
				dbus_uint64_t mac_address;
				
				mac_address = 
					((dbus_uint64_t)a5<<40) |
					((dbus_uint64_t)a4<<32) | 
					((dbus_uint64_t)a3<<24) | 
					((dbus_uint64_t)a2<<16) | 
					((dbus_uint64_t)a1<< 8) | 
					((dbus_uint64_t)a0<< 0);
				
				hal_device_property_set_uint64 (d, is_wireless ? "net.80211.mac_address" : 
								"net.80203.mac_address",
								mac_address);
			}
		}
	} else if (media_type == ARPHRD_IRDA) {
		hal_device_property_set_string (d, "info.product", "Networking Interface");
		hal_device_property_set_string (d, "info.category", "net.irda");
		hal_device_add_capability (d, "net.irda");
	}
#if defined(ARPHRD_IEEE80211_RADIOTAP) && defined(ARPHRD_IEEE80211_PRISM)
	else if (media_type == ARPHRD_IEEE80211 || media_type == ARPHRD_IEEE80211_PRISM || 
		   media_type == ARPHRD_IEEE80211_RADIOTAP) {
		hal_device_property_set_string (d, "info.product", "Networking Wireless Control Interface");
		hal_device_property_set_string (d, "info.category", "net.80211control");
		hal_device_add_capability (d, "net.80211control");
	}
#else
#warning ARPHRD_IEEE80211_RADIOTAP and/or ARPHRD_IEEE80211_PRISM not defined!
#endif

	return d;
error:
	if (d != NULL) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		d = NULL;
	}

	return d;
}

static gboolean
net_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const gchar *id;

	id = hal_device_property_get_string (d, "net.address");
	if (id == NULL || (strcmp (id, "00:00:00:00:00:00") == 0)) {
		/* Need to fall back to something else if mac not available. */
		id = hal_util_get_last_element(hal_device_property_get_string(d, "net.originating_device"));
	}
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/net_%s",
			      id);
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
scsi_generic_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "scsi_generic");
	hal_device_add_capability (d, "scsi_generic");
	hal_device_property_set_string (d, "info.product", "SCSI Generic Interface");
	hal_device_property_set_string (d, "scsi_generic.device", device_file);

out:
	return d;
}

static gboolean
scsi_generic_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_scsi_generic",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

/* scsi-sysfs totally sucks, we don't get events for the devices in the
 * devpath, but totally useless class devices; we hook into the scsi_device
 * event, and synthesize the missing host event
 */
static gboolean
missing_scsi_host (const gchar *sysfs_path, HotplugEvent *device_event, HotplugActionType action)
{
	gchar path[HAL_PATH_MAX];
	HalDevice *d;
	HotplugEvent *host_event;
	int rc = FALSE;

	g_strlcpy(path, sysfs_path, sizeof(path));
	/* skip device */
	if (!hal_util_path_ascend (path))
		goto out;
	/* skip target */
	if (!hal_util_path_ascend (path))
		goto out;
	if (strstr (path, "/host") == NULL)
		goto out;

	d = hal_device_store_match_key_value_string (hald_get_gdl (),
						     "linux.sysfs_path",
						     path);
	if (action == HOTPLUG_ACTION_ADD && d != NULL)
		goto out;
	if (action == HOTPLUG_ACTION_REMOVE && d == NULL)
		goto out;
	rc = TRUE;

	host_event = g_new0 (HotplugEvent, 1);
	host_event->action = action;
	host_event->type = HOTPLUG_EVENT_SYSFS_DEVICE;
	g_strlcpy (host_event->sysfs.subsystem, "scsi_host", sizeof (host_event->sysfs.subsystem));
	g_strlcpy (host_event->sysfs.sysfs_path, path, sizeof (host_event->sysfs.sysfs_path));
	host_event->sysfs.net_ifindex = -1;

	if (action == HOTPLUG_ACTION_ADD) {
		hotplug_event_enqueue_at_front (device_event);
		hotplug_event_enqueue_at_front (host_event);
		hotplug_event_reposted (device_event);
		goto out;
	}
	if (action == HOTPLUG_ACTION_REMOVE)
		hotplug_event_enqueue (host_event);

out:
	return rc;
}

static HalDevice *
scsi_host_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	gint host_num;
	const gchar *last_elem;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL) {
		goto out;
	}

	/* ignore useless class device */
	if (strstr(sysfs_path, "class/scsi_host") != NULL)
		goto out;

	last_elem = hal_util_get_last_element (sysfs_path);
	if (sscanf (last_elem, "host%d", &host_num) != 1)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_int (d, "scsi_host.host", host_num);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "scsi_host");
	hal_device_add_capability (d, "scsi_host");
	hal_device_property_set_string (d, "info.product", "SCSI Host Adapter");
out:
	return d;
}

static gboolean
scsi_host_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_scsi_host",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
usbclass_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	gint host_num;
	const gchar *last_elem;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL || device_file == NULL) {
		goto out;
	}

	last_elem = hal_util_get_last_element (sysfs_path);
	if (sscanf (last_elem, "hiddev%d", &host_num) == 1) {

		d = hal_device_new ();
		hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

		hal_device_property_set_string (d, "info.category", "hiddev");
		hal_device_add_capability (d, "hiddev");

		hal_device_property_set_string (d, "info.product", "USB HID Device");

		hal_device_property_set_string (d, "hiddev.device", device_file);
	} else if (sscanf (last_elem, "lp%d", &host_num) == 1) {

		d = hal_device_new ();
		hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

		hal_device_property_set_string (d, "info.category", "printer");
		hal_device_add_capability (d, "printer");

		hal_device_property_set_string (d, "info.product", "Printer");
		hal_device_property_set_string (d, "printer.device", device_file);

		hal_device_property_set_string (d, "printer.originating_device", hal_device_get_udi (parent_dev));
		hal_device_property_set_string (d, "printer.physical_device", hal_device_get_udi (parent_dev));
	}

out:
	return d;
}

static const gchar *
usbclass_get_prober (HalDevice *d)
{
	if (hal_device_has_capability (d, "hiddev"))
		return "hald-probe-hiddev";
	else if (hal_device_has_capability (d, "printer"))
		return "hald-probe-printer";
	else
		return NULL;
}

static gboolean
usbclass_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_has_capability (d, "hiddev")) {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_hiddev",
				      hal_device_property_get_string (d, "info.parent"));
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
	} else if (hal_device_has_capability (d, "printer")) {
		const char *serial;

		serial = hal_device_property_get_string (d, "printer.serial");
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_printer_%s",
				      hal_device_property_get_string (d, "info.parent"),
				      serial != NULL ? serial : "noserial");
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
	}

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
usbraw_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "usbraw");
	hal_device_add_capability (d, "usbraw");
	hal_device_property_set_string (d, "info.product", "USB Raw Device Access");
	hal_device_property_set_string (d, "usbraw.device", device_file);

out:
	return d;
}

static gboolean
usbraw_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi), "%s_usbraw",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
video4linux_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "video4linux");
	hal_device_add_capability (d, "video4linux");
	hal_device_property_set_string (d, "info.product", "Video Device");
	hal_device_property_set_string (d, "video4linux.device", device_file);

out:
	return d;
}

static gboolean
video4linux_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi), "%s_video4linux",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
dvb_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "dvb");
	hal_device_add_capability (d, "dvb");
	hal_device_property_set_string (d, "info.product", "DVB Device");
	hal_device_property_set_string (d, "dvb.device", device_file);

out:
	return d;
}

static gboolean
dvb_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi), "%s_dvb",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

static void
asound_card_id_set (int cardnum, HalDevice *d, const char *propertyname)
{
	char aprocdir[256];
	char linestart[5];
	gchar *alsaname;

	snprintf (aprocdir, sizeof (aprocdir), "%s/asound", get_hal_proc_path ());
	snprintf (linestart, sizeof (linestart), "%2d [", cardnum);
	alsaname = hal_util_grep_file_next_line (aprocdir, "cards", linestart, FALSE);
	if (alsaname != NULL) {
		gchar *end;
		end = strstr (alsaname, " at ");
		if (end != NULL) {
			end[0] = '\0';
		}
		alsaname = g_strstrip (alsaname);
		hal_device_property_set_string (d, propertyname, alsaname);
	}
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
sound_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	int cardnum, devicenum;
	char type;
	const gchar *device;
	gchar *device_id;
	char aprocdir[256];
	char buf[256];

	d = NULL;

	if (device_file == NULL) 
		goto out;	

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	device = hal_util_get_last_element(sysfs_path);

	if (parent_dev == NULL || parent_path == NULL) {
 		/* handle global ALSA and OSS devices, these devices are for all ALSA/OSS Sound devices
		   so we append them to /org/freedesktop/Hal/devices/computer */
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");

		if (!strncmp (device, "timer", 5)){
			/* handle global ALSA Timer device */
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.type", "timer");
			hal_device_property_set_string (d, "info.product", "ALSA Timer Device");
			hal_device_property_set_string (d, "alsa.device_file", device_file);
		} else if (!strncmp (device, "sequencer", 9)){
			/* handle global OSS sequencer devices */
			hal_device_property_set_string (d, "info.category", "oss");
			hal_device_add_capability (d, "oss");
			hal_device_property_set_string (d, "oss.type", "sequencer");
			hal_device_property_set_string (d, "info.product", "OSS Sequencer Device");
			hal_device_property_set_string (d, "oss.device_file", device_file);
		} else if (!strncmp (device, "seq", 3) && strlen(device) == 3) { 
			/* handle global ALSA sequencer devices */
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.type", "sequencer");
			hal_device_property_set_string (d, "info.product", "ALSA Sequencer Device");
			hal_device_property_set_string (d, "alsa.device_file", device_file);	
		} else {
			goto error;
		}
	} else {
		/* handle ALSA and OSS devices with parent_dev link in sys */
		if (sscanf (device, "controlC%d", &cardnum) == 1) {
			
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.device_file", device_file);
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "alsa.originating_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "alsa.physical_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_int (d, "alsa.card", cardnum);
			hal_device_property_set_string (d, "alsa.type", "control");
	
			asound_card_id_set (cardnum, d, "alsa.card_id");
	
			snprintf (buf, sizeof (buf), "%s ALSA Control Device", 
				hal_device_property_get_string (d, "alsa.card_id"));
			hal_device_property_set_string (d, "info.product", buf);
	
		} else if (sscanf (device, "pcmC%dD%d%c", &cardnum, &devicenum, &type) == 3) {
			
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.device_file", device_file);
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "alsa.originating_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "alsa.physical_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_int (d, "alsa.card", cardnum);
			hal_device_property_set_int (d, "alsa.device", devicenum);
	
			asound_card_id_set (cardnum, d, "alsa.card_id");

			if (!hal_util_set_string_from_file (d, "alsa.pcm_class", sysfs_path, "pcm_class"))
				 hal_device_property_set_string (d, "alsa.pcm_class", "unknown");
	
			snprintf (aprocdir, sizeof (aprocdir), "%s/asound/card%d/pcm%d%c", 
				get_hal_proc_path (), cardnum, devicenum, type);
			device_id = hal_util_grep_file (aprocdir, "info", "name: ", FALSE);
			if (device_id != NULL) {
				hal_device_property_set_string (d, "alsa.device_id", device_id);
			}
	
			if (type == 'p') {
				hal_device_property_set_string (d, "alsa.type", "playback");
				if (device_id != NULL) {
					snprintf (buf, sizeof (buf), "%s ALSA Playback Device", device_id);
					hal_device_property_set_string (d, "info.product", buf);
				} else
					hal_device_property_set_string (d, "info.product", "ALSA Playback Device");
			} else if (type == 'c') {
				hal_device_property_set_string (d, "alsa.type", "capture");
				if (device_id != NULL) {
					snprintf (buf, sizeof (buf), "%s ALSA Capture Device", device_id);
					hal_device_property_set_string (d, "info.product", buf);
				} else
					hal_device_property_set_string (d, "info.product", "ALSA Capture Device");
			} else {
				hal_device_property_set_string (d, "alsa.type", "unknown");
				if (device_id != NULL) {
					snprintf (buf, sizeof (buf), "%s ALSA Device", device_id);
					hal_device_property_set_string (d, "info.product", buf);
				} else
					hal_device_property_set_string (d, "info.product", "ALSA Device");
			}
		} else if ((sscanf (device, "hwC%dD%d", &cardnum, &devicenum) == 2) ||
			   (sscanf (device, "midiC%dD%d", &cardnum, &devicenum) == 2)) {
			
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.device_file", device_file);
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "alsa.originating_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "alsa.physical_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_int (d, "alsa.card", cardnum);
			hal_device_property_set_int (d, "alsa.device", devicenum);
	
			asound_card_id_set (cardnum, d, "alsa.card_id");

			if (!strncmp (device, "hwC", 3)) {
				hal_device_property_set_string (d, "alsa.type", "hw_specific");
				snprintf (buf, sizeof (buf), "%s ALSA hardware specific Device", hal_device_property_get_string (d, "alsa.card_id"));
			} else if (!strncmp (device, "midiC", 5)) {
				hal_device_property_set_string (d, "alsa.type", "midi");
				snprintf (buf, sizeof (buf), "%s ALSA MIDI Device", hal_device_property_get_string (d, "alsa.card_id"));
			}
			hal_device_property_set_string (d, "info.product", buf);
	
		} else if (!strncmp (device, "dsp", 3) || !strncmp (device, "adsp", 4) || 
			   !strncmp (device, "midi", 4) || !strncmp (device, "amidi", 5) ||
			   !strncmp (device, "audio", 5) || !strncmp (device, "mixer", 5)) {
			
			/* handle OSS-Devices */
			ClassDevOSSDeviceTypes type;
	
			if (!strncmp (device, "dsp", 3)) {
				if(sscanf (device, "dsp%d", &cardnum) != 1) cardnum = 0;
				type = OSS_DEVICE_TYPE_DSP;
			} else if (!strncmp (device, "adsp", 4)) {
				if(sscanf (device, "adsp%d", &cardnum) != 1) cardnum = 0;
				type = OSS_DEVICE_TYPE_ADSP;
			} else if (!strncmp (device, "midi", 4)) {
				if(sscanf (device, "midi%d", &cardnum) != 1) cardnum = 0;
				type = OSS_DEVICE_TYPE_MIDI;
			} else if (!strncmp (device, "amidi", 5)) {
				if(sscanf (device, "amidi%d", &cardnum) != 1) cardnum = 0;
				type = OSS_DEVICE_TYPE_AMIDI;
			} else if (!strncmp (device, "audio", 5)) {
				if(sscanf (device, "audio%d", &cardnum) != 1) cardnum = 0;
				type = OSS_DEVICE_TYPE_AUDIO;
			} else if (!strncmp (device, "mixer", 5)) {
				if(sscanf (device, "mixer%d", &cardnum) != 1) cardnum = 0;
				type = OSS_DEVICE_TYPE_MIXER;
			} else {
				cardnum = 0;
				type = OSS_DEVICE_TYPE_UNKNOWN;
			}

			hal_device_property_set_string (d, "info.category", "oss");
			hal_device_add_capability (d, "oss");
			hal_device_property_set_string (d, "oss.device_file", device_file);
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "oss.originating_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_string (d, "oss.physical_device", hal_device_get_udi (parent_dev));
			hal_device_property_set_int (d, "oss.card", cardnum);
	
			asound_card_id_set (cardnum, d, "oss.card_id");
	
			snprintf (aprocdir, sizeof (aprocdir), "%s/asound/card%d/pcm0p", 
				get_hal_proc_path (), cardnum);
			device_id = hal_util_grep_file (aprocdir, "info", "name: ", FALSE);
			if (device_id != NULL) {
				hal_device_property_set_string (d, "oss.device_id", device_id);
			} 

			switch (type) { 
				case OSS_DEVICE_TYPE_MIXER:
					hal_device_property_set_string (d, "oss.type", "mixer");
					if (device_id != NULL) 
						snprintf (buf, sizeof (buf), "%s OSS Control Device", device_id); 
					else
						snprintf (buf, sizeof (buf), "%s OSS Control Device",
						          hal_device_property_get_string (d, "oss.card_id"));
					break;
				case OSS_DEVICE_TYPE_DSP:
				case OSS_DEVICE_TYPE_AUDIO:
				case OSS_DEVICE_TYPE_ADSP:
					if (type == OSS_DEVICE_TYPE_ADSP)
						hal_device_property_set_int (d, "oss.device", 1);
					else 
						hal_device_property_set_int (d, "oss.device", 0);

					hal_device_property_set_string (d, "oss.type", "pcm");
					if (device_id != NULL) 
						snprintf (buf, sizeof (buf), "%s OSS PCM Device", device_id); 
					else
						snprintf (buf, sizeof (buf), "%s OSS PCM Device",
						          hal_device_property_get_string (d, "oss.card_id"));
					break;
				case OSS_DEVICE_TYPE_MIDI:
				case OSS_DEVICE_TYPE_AMIDI:
					if (type == OSS_DEVICE_TYPE_AMIDI)
						hal_device_property_set_int (d, "oss.device", 1);
					else
						hal_device_property_set_int (d, "oss.device", 0);
					hal_device_property_set_string (d, "oss.type", "midi");
					if (device_id != NULL) 
						snprintf (buf, sizeof (buf), "%s OSS MIDI Device", device_id); 
					else
						snprintf (buf, sizeof (buf), "%s OSS MIDI Device",
						          hal_device_property_get_string (d, "oss.card_id"));
					break;
				case OSS_DEVICE_TYPE_UNKNOWN:
				default:
					hal_device_property_set_string (d, "oss.type", "unknown");
					if (device_id != NULL) 
						snprintf (buf, sizeof (buf), "%s OSS Device", device_id); 
					else
						snprintf (buf, sizeof (buf), "%s OSS Device",
						          hal_device_property_get_string (d, "oss.card_id"));
					break;
			}
			hal_device_property_set_string (d, "info.product", buf);
		}
		else {
			goto error;
		}
	}
out:
	return d;

error: 
	g_object_unref (d);
	d = NULL;
	return d;
}

static gboolean
sound_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_has_property(d, "alsa.card")) {
		/* don't include card number as it may not be persistent across reboots */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_alsa_%s_%i",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_string (d, "alsa.type"),
				      hal_device_property_get_int (d, "alsa.device"));
	} else if (hal_device_has_property(d, "oss.card")) {
		/* don't include card number as it may not be persistent across reboots */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_oss_%s_%i",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_string (d, "oss.type"),
				      hal_device_property_get_int (d, "oss.device"));
	} else if (hal_device_has_property(d, "alsa.type")) {
		/* handle global ALSA devices */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_alsa_%s",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_string (d, "alsa.type"));
	} else if (hal_device_has_property(d, "oss.type")) {
		/* handle global OSS devices */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_oss_%s",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_string (d, "oss.type"));
	} else {
		/* fallback */
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi), "%s_sound_unknown",
				      hal_device_property_get_string (d, "info.parent"));
	} 
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
serial_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	int portnum;
	HalDevice *d;
	const gchar *last_elem;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL || device_file == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "serial");
	hal_device_add_capability (d, "serial");
	hal_device_property_set_string (d, "serial.originating_device", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "serial.physical_device", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "serial.device", device_file);

	last_elem = hal_util_get_last_element(sysfs_path);
	if (sscanf (last_elem, "ttyS%d", &portnum) == 1) {
		hal_device_property_set_int (d, "serial.port", portnum);
		hal_device_property_set_string (d, "serial.type", "platform");
		hal_device_property_set_string (d, "info.product",
						hal_device_property_get_string (parent_dev, "info.product"));
	} else if (sscanf (last_elem, "ttyUSB%d", &portnum) == 1) {
		HalDevice *usbdev;

		hal_device_property_set_int (d, "serial.port", portnum);
		hal_device_property_set_string (d, "serial.type", "usb");

		usbdev = hal_device_store_find (hald_get_gdl (), 
						hal_device_property_get_string (parent_dev, "info.parent"));
		if (usbdev != NULL) {
			hal_device_property_set_string (d, "info.product",
							hal_device_property_get_string (usbdev, "info.product"));
		} else {
			hal_device_property_set_string (d, "info.product", "USB Serial Port");
		}
	} else {
		int len;
		int i;

		len = strlen (last_elem);

		for (i = len - 1; i >= 0 && isdigit (last_elem[i]); --i)
			;
		if (i == len - 1)
			portnum = 0;
		else
			portnum = atoi (last_elem + i + 1);

		hal_device_property_set_int (d, "serial.port", portnum);
		hal_device_property_set_string (d, "serial.type", "unknown");
		hal_device_property_set_string (d, "info.product", "Serial Port");
	}

out:
	return d;
}

static const gchar *
serial_get_prober (HalDevice *d)
{
	/* FIXME TODO: check if there is an other way, to call the porber only
		 on ttyS* devices, than check the name of the device file */
	if (!strncmp(hal_device_property_get_string (d, "linux.device_file"), "/dev/ttyS", 9))
		return "hald-probe-serial";
	else 
		return NULL;
}

static gboolean
serial_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_serial_%s_%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_string (d, "serial.type"),
			      hal_device_property_get_int (d, "serial.port"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
tape_add (const gchar *sysfs_path, const gchar *device_file, 
	  HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *dev_entry;

	if (parent_dev == NULL)
		return NULL;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "tape");
	hal_device_add_capability (d, "tape");
	hal_device_add_capability (parent_dev, "tape");

	dev_entry = hal_util_get_string_from_file (sysfs_path, "dev");
	if (dev_entry != NULL) {
		unsigned int major, minor;

		if (sscanf (dev_entry, "%d:%d", &major, &minor) != 2) {
			hal_device_property_set_int (d, "tape.major", major);
			hal_device_property_set_int (d, "tape.minor", minor);
		}
	}
	return d;
}

static gboolean
tape_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const gchar *sysfs_name;

	sysfs_name = hal_util_get_last_element (hal_device_property_get_string
						(d, "linux.sysfs_path"));
	if (!sysfs_name)
		return FALSE;
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/tape_%s",
			      sysfs_name);
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
mmc_host_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	gint host_num;
	const gchar *last_elem;

	d = NULL;

	if (parent_dev == NULL || parent_path == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);

	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_device_property_set_string (d, "info.category", "mmc_host");
	hal_device_add_capability (d, "mmc_host");

	hal_device_property_set_string (d, "info.product", "MMC/SD Host Adapter");

	last_elem = hal_util_get_last_element (sysfs_path);
	sscanf (last_elem, "mmc%d", &host_num);
	hal_device_property_set_int (d, "mmc_host.host", host_num);

out:
	return d;
}

static gboolean
mmc_host_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_mmc_host",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

static HalDevice *
pci_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	gint device_class;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "pci");
	hal_device_property_set_string (d, "info.bus", "pci");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_device_property_set_string (d, "pci.linux.sysfs_path", sysfs_path);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_int_from_file (d, "pci.product_id", sysfs_path, "device", 16);
	hal_util_set_int_from_file (d, "pci.vendor_id", sysfs_path, "vendor", 16);
	hal_util_set_int_from_file (d, "pci.subsys_product_id", sysfs_path, "subsystem_device", 16);
	hal_util_set_int_from_file (d, "pci.subsys_vendor_id", sysfs_path, "subsystem_vendor", 16);

	if (hal_util_get_int_from_file (sysfs_path, "class", &device_class, 16)) {
		hal_device_property_set_int (d, "pci.device_class", ((device_class >> 16) & 0xff));
		hal_device_property_set_int (d, "pci.device_subclass", ((device_class >> 8) & 0xff));
		hal_device_property_set_int (d, "pci.device_protocol", (device_class & 0xff));
	}

	{
		gchar buf[64];
		char *vendor_name;
		char *product_name;
		char *subsys_vendor_name;
		char *subsys_product_name;

		ids_find_pci (hal_device_property_get_int (d, "pci.vendor_id"), 
			      hal_device_property_get_int (d, "pci.product_id"), 
			      hal_device_property_get_int (d, "pci.subsys_vendor_id"), 
			      hal_device_property_get_int (d, "pci.subsys_product_id"), 
			      &vendor_name, &product_name, &subsys_vendor_name, &subsys_product_name);

		if (vendor_name != NULL) {
			hal_device_property_set_string (d, "pci.vendor", vendor_name);
			hal_device_property_set_string (d, "info.vendor", vendor_name);
		} else {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
				    hal_device_property_get_int (d, "pci.vendor_id"));
			hal_device_property_set_string (d, "info.vendor", buf);
		}

		if (product_name != NULL) {
			hal_device_property_set_string (d, "pci.product", product_name);
			hal_device_property_set_string (d, "info.product", product_name);
		} else {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
				    hal_device_property_get_int (d, "pci.product_id"));
			hal_device_property_set_string (d, "info.product", buf);
		}

		if (subsys_vendor_name != NULL) {
			hal_device_property_set_string (d, "pci.subsys_vendor", subsys_vendor_name);
		} 
		if (subsys_product_name != NULL) {
			hal_device_property_set_string (d, "pci.subsys_product", subsys_product_name);
		}
	}

	return d;
}

static gboolean
pci_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pci_%x_%x",
			      hal_device_property_get_int (d, "pci.vendor_id"),
			      hal_device_property_get_int (d, "pci.product_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void 
usbif_set_name (HalDevice *d, int ifclass, int ifsubclass, int ifprotocol)
{
	const char *name;

	switch (ifclass) {
	default:
	case 0x00:
		name = "USB Interface";
		break;
	case 0x01:
		name = "USB Audio Interface";
		break;
	case 0x02:
		name = "USB Communications Interface";
		break;
	case 0x03:
		name = "USB HID Interface";
		break;
	case 0x06:
		name = "USB Imaging Interface";
		break;
	case 0x07:
		name = "USB Printer Interface";
		break;
	case 0x08:
		name = "USB Mass Storage Interface";
		break;
	case 0x09:
		name = "USB Hub Interface";
		break;
	case 0x0a:
		name = "USB Data Interface";
		break;
	case 0x0b:
		name = "USB Chip/Smartcard Interface";
		break;
	case 0x0d:
		name = "USB Content Security Interface";
		break;
	case 0x0e:
		name = "USB Video Interface";
		break;
	case 0xdc:
		name = "USB Diagnostic Interface";
		break;
	case 0xe0:
		name = "USB Wireless Interface";
		break;
	case 0xef:
		name = "USB Miscelleneous Interface";
		break;
	case 0xfe:
		name = "USB Application Specific Interface";
		break;
	case 0xff:
		name = "USB Vendor Specific Interface";
		break;
	}

	hal_device_property_set_string (d, "usb.product", name);
	hal_device_property_set_string (d, "info.product", name);
}

static HalDevice *
usb_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	}

	/* only USB interfaces got a : in the bus_id */
	bus_id = hal_util_get_last_element (sysfs_path);
	if (strchr (bus_id, ':') == NULL) {
		gint bmAttributes;

		hal_device_property_set_string (d, "info.subsystem", "usb_device");
		hal_device_property_set_string (d, "info.bus", "usb_device");

		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		hal_device_property_set_string (d, "usb_device.linux.sysfs_path", sysfs_path);

		hal_util_set_int_from_file (d, "usb_device.configuration_value", sysfs_path, "bConfigurationValue", 10);
		hal_util_set_int_from_file (d, "usb_device.num_configurations", sysfs_path, "bNumConfigurations", 10);
		hal_util_set_int_from_file (d, "usb_device.num_interfaces", sysfs_path, "bNumInterfaces", 10);

		hal_util_set_int_from_file (d, "usb_device.device_class", sysfs_path, "bDeviceClass", 16);
		hal_util_set_int_from_file (d, "usb_device.device_subclass", sysfs_path, "bDeviceSubClass", 16);
		hal_util_set_int_from_file (d, "usb_device.device_protocol", sysfs_path, "bDeviceProtocol", 16);

		hal_util_set_int_from_file (d, "usb_device.vendor_id", sysfs_path, "idVendor", 16);
		hal_util_set_int_from_file (d, "usb_device.product_id", sysfs_path, "idProduct", 16);

		{
			gchar buf[64];
			char *vendor_name;
			char *product_name;

			ids_find_usb (hal_device_property_get_int (d, "usb_device.vendor_id"), 
				      hal_device_property_get_int (d, "usb_device.product_id"), 
				      &vendor_name, &product_name);

			if (vendor_name != NULL) {
				hal_device_property_set_string (d, "usb_device.vendor", vendor_name);
				hal_device_property_set_string (d, "info.vendor", vendor_name);
			} else {
				if (!hal_util_set_string_from_file (d, "usb_device.vendor", 
								    sysfs_path, "manufacturer")) {
					g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
						    hal_device_property_get_int (d, "usb_device.vendor_id"));
					hal_device_property_set_string (d, "info.vendor", buf); 
				} else {
                                        hal_device_property_set_string (
                                                d, "info.vendor",
                                                hal_device_property_get_string (d, "usb_device.vendor"));
                                }
			}

			if (product_name != NULL) {
				hal_device_property_set_string (d, "usb_device.product", product_name);
				hal_device_property_set_string (d, "info.product", product_name);
			} else {
				if (!hal_util_set_string_from_file (d, "usb_device.product", 
								    sysfs_path, "product")) {
					g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", 
						    hal_device_property_get_int (d, "usb_device.product_id"));
					hal_device_property_set_string (d, "info.product", buf); 
				} else {
                                        hal_device_property_set_string (
                                                d, "info.product",
                                                hal_device_property_get_string (d, "usb_device.product"));
                                }
			}
		}

		hal_util_set_int_from_file (d, "usb_device.device_revision_bcd", sysfs_path, "bcdDevice", 16);

		hal_util_set_int_from_file (d, "usb_device.max_power", sysfs_path, "bMaxPower", 10);
		hal_util_set_int_from_file (d, "usb_device.num_ports", sysfs_path, "maxchild", 10);
		hal_util_set_int_from_file (d, "usb_device.linux.device_number", sysfs_path, "devnum", 10);

		hal_util_set_string_from_file (d, "usb_device.serial", sysfs_path, "serial");

		hal_util_set_string_from_file (d, "usb_device.serial", sysfs_path, "serial");
		hal_util_set_bcd2_from_file (d, "usb_device.speed_bcd", sysfs_path, "speed");
		hal_util_set_bcd2_from_file (d, "usb_device.version_bcd", sysfs_path, "version");

		hal_util_get_int_from_file (sysfs_path, "bmAttributes", &bmAttributes, 16);
		hal_device_property_set_bool (d, "usb_device.is_self_powered", (bmAttributes & 0x40) != 0);
		hal_device_property_set_bool (d, "usb_device.can_wake_up", (bmAttributes & 0x20) != 0);

		if (strncmp (bus_id, "usb", 3) == 0)
			hal_device_property_set_int (d, "usb_device.bus_number", atoi (bus_id + 3));
		else
			hal_device_property_set_int (d, "usb_device.bus_number", atoi (bus_id));

		/* TODO:  .level_number .parent_number  */

	} else {
		hal_device_property_set_string (d, "info.subsystem", "usb");
		hal_device_property_set_string (d, "info.bus", "usb");

		/* take all usb_device.* properties from parent and make them usb.* on this object */
		if (parent_dev != NULL)
			hal_device_merge_with_rewrite (d, parent_dev, "usb.", "usb_device.");

		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		hal_device_property_set_string (d, "usb.linux.sysfs_path", sysfs_path);

		hal_util_set_int_from_file (d, "usb.interface.number", sysfs_path, "bInterfaceNumber", 10);

		hal_util_set_int_from_file (d, "usb.interface.class", sysfs_path, "bInterfaceClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.subclass", sysfs_path, "bInterfaceSubClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.protocol", sysfs_path, "bInterfaceProtocol", 16);

		usbif_set_name (d, 
				hal_device_property_get_int (d, "usb.interface.class"),
				hal_device_property_get_int (d, "usb.interface.subclass"),
				hal_device_property_get_int (d, "usb.interface.protocol"));
	}

	return d;
}

static gboolean
usb_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_has_property (d, "usb.interface.number")) {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_if%d",
				      hal_device_property_get_string (d, "info.parent"),
				      hal_device_property_get_int (d, "usb.interface.number"));
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
	} else {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "/org/freedesktop/Hal/devices/usb_device_%x_%x_%s",
				      hal_device_property_get_int (d, "usb_device.vendor_id"),
				      hal_device_property_get_int (d, "usb_device.product_id"),
				      hal_device_has_property (d, "usb_device.serial") ?
				        hal_device_property_get_string (d, "usb_device.serial") :
				        "noserial");
		hal_device_set_udi (d, udi);
		hal_device_property_set_string (d, "info.udi", udi);
	}

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
ide_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	guint host, channel;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "ide");
	hal_device_property_set_string (d, "info.bus", "ide");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);

	sscanf (bus_id, "%d.%d", &host, &channel);
	hal_device_property_set_int (d, "ide.host", host);
	hal_device_property_set_int (d, "ide.channel", channel);

	if (channel == 0) {
		hal_device_property_set_string (d, "info.product", "IDE device (master)");
	} else {
		hal_device_property_set_string (d, "info.product", "IDE device (slave)");
	}
	
	return d;
}

static gboolean
ide_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_ide_%d_%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_int (d, "ide.host"),
			      hal_device_property_get_int (d, "ide.channel"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pnp_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "pnp");
	hal_device_property_set_string (d, "info.bus", "pnp");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_string_from_file (d, "pnp.id", sysfs_path, "id");
	if (hal_device_has_property (d, "pnp.id")) {
		gchar *pnp_description;
		ids_find_pnp (hal_device_property_get_string (d, "pnp.id"), &pnp_description);
		if (pnp_description != NULL) {
			hal_device_property_set_string (d, "pnp.description", pnp_description);
			hal_device_property_set_string (d, "info.product", pnp_description);
		}
	}

	if (!hal_device_has_property (d, "info.product")) {
		gchar buf[64];
		g_snprintf (buf, sizeof (buf), "PnP Device (%s)", hal_device_property_get_string (d, "pnp.id"));
		hal_device_property_set_string (d, "info.product", buf);
	}

	
	return d;
}

static gboolean
pnp_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pnp_%s",
			      hal_device_property_get_string (d, "pnp.id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
platform_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "platform");
	hal_device_property_set_string (d, "info.bus", "platform");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);

	hal_device_property_set_string (d, "platform.id", dev_id);

	g_snprintf (buf, sizeof (buf), "Platform Device (%s)", hal_device_property_get_string (d, "platform.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
platform_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/platform_%s",
			      hal_device_property_get_string (d, "platform.id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
serio_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "serio");
	hal_device_property_set_string (d, "info.bus", "serio");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "serio.id", bus_id);
	if (!hal_util_set_string_from_file (d, "serio.description", sysfs_path, "description")) {
		hal_device_property_set_string (d, "serio.description", hal_device_property_get_string (d, "serio.id"));
	}
	hal_device_property_set_string (d, "info.product", hal_device_property_get_string (d, "serio.description"));
	
	return d;
}

static gboolean
serio_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_%s",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_string (d, "serio.description"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pcmcia_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	guint socket, function;
	const char *prod_id1;
	const char *prod_id2;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "pcmcia");
	hal_device_property_set_string (d, "info.bus", "pcmcia");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	bus_id = hal_util_get_last_element (sysfs_path);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	/* not sure if %d.%d means socket function - need to revisit */
	sscanf (bus_id, "%d.%d", &socket, &function);
	hal_device_property_set_int (d, "pcmcia.socket_number", socket);

	hal_util_set_string_from_file (d, "pcmcia.prod_id1", sysfs_path, "prod_id1");
	hal_util_set_string_from_file (d, "pcmcia.prod_id2", sysfs_path, "prod_id2");
	hal_util_set_string_from_file (d, "pcmcia.prod_id3", sysfs_path, "prod_id3");
	hal_util_set_string_from_file (d, "pcmcia.prod_id4", sysfs_path, "prod_id4");

	hal_util_set_int_from_file (d, "pcmcia.manf_id", sysfs_path, "manf_id", 16);
	hal_util_set_int_from_file (d, "pcmcia.card_id", sysfs_path, "card_id", 16);
	hal_util_set_int_from_file (d, "pcmcia.func_id", sysfs_path, "func_id", 16);

	prod_id1 = hal_device_property_get_string (d, "pcmcia.prod_id1");
	prod_id2 = hal_device_property_get_string (d, "pcmcia.prod_id2");

	/* Provide best-guess of vendor, goes in Vendor property */
	if (prod_id1 != NULL) {
		hal_device_property_set_string (d, "pcmcia.vendor", prod_id1);
		hal_device_property_set_string (d, "info.vendor", prod_id1);
	} else {
		char buf[50];
		g_snprintf (buf, sizeof(buf), "Unknown (0x%04x)", hal_device_property_get_int (d, "pcmcia.manf_id"));
		hal_device_property_set_string (d, "info.vendor", buf);
	}

	/* Provide best-guess of name, goes in Product property */
	if (prod_id2 != NULL) {
		hal_device_property_set_string (d, "pcmcia.product", prod_id1);
		hal_device_property_set_string (d, "info.product", prod_id2);
	} else {
		char buf[50];
		g_snprintf (buf, sizeof(buf), "Unknown (0x%04x)", hal_device_property_get_int (d, "pcmcia.card_id"));
		hal_device_property_set_string (d, "info.product", buf);
	}

	return d;
}

static gboolean
pcmcia_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pcmcia_%d_%d",
			      hal_device_property_get_int (d, "pcmcia.manfid1"),
			      hal_device_property_get_int (d, "pcmcia.manfid2"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
scsi_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d = NULL;
	const gchar *bus_id;
	gint host_num, bus_num, target_num, lun_num;
	int type;

	if (parent_dev == NULL)
		goto out;

	bus_id = hal_util_get_last_element (sysfs_path);
	if (sscanf (bus_id, "%d:%d:%d:%d", &host_num, &bus_num, &target_num, &lun_num) != 4)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "scsi");
	hal_device_property_set_string (d, "info.bus", "scsi");
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_int (d, "scsi.host", host_num);
	hal_device_property_set_int (d, "scsi.bus", bus_num);
	hal_device_property_set_int (d, "scsi.target", target_num);
	hal_device_property_set_int (d, "scsi.lun", lun_num);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "info.product", "SCSI Device");

	hal_util_set_string_from_file (d, "scsi.model", sysfs_path, "model");
	hal_util_set_string_from_file (d, "scsi.vendor", sysfs_path, "vendor");
	hal_util_get_int_from_file (sysfs_path, "type", &type, 0);
	HAL_INFO (("%s/type -> %d (-> scsi.type)", sysfs_path, type));
	switch (type) {
	case 0:  /* TYPE_DISK (disk) */
	case 7:  /* TYPE_MOD (Magneto-optical disk) */
	case 14: /* TYPE_RBC (Reduced Block Commands)
		  * Simple Direct Access Device, set it to disk
		  * (some Firewire Disks use it)
		  */
		hal_device_property_set_string (d, "scsi.type", "disk");
		break;
	case 1: /* TYPE_TAPE (Tape) */
		hal_device_property_set_string (d, "scsi.type", "tape");
		break;
	case 2:
		/* TYPE_PRINTER (Tape) */
		hal_device_property_set_string (d, "scsi.type", "printer");
		break;
	case 3:  /* TYPE_PROCESSOR */
		hal_device_property_set_string (d, "scsi.type", "processor");
		break;
	case 4: /* TYPE_WORM */
	case 5: /* TYPE_ROM (CD-ROM) */
		hal_device_property_set_string (d, "scsi.type", "cdrom");
		break;
	case 6: /* TYPE_SCANNER */
		hal_device_property_set_string (d, "scsi.type", "scanner");
		break;
	case 8: /* TYPE_MEDIUM_CHANGER */
		hal_device_property_set_string (d, "scsi.type", "medium_changer");
		break;
	case 9: /* TYPE_COMM */
		hal_device_property_set_string (d, "scsi.type", "comm");
		break;
	case 12: /* TYPE_RAID */
		hal_device_property_set_string (d, "scsi.type", "raid");
		break;
	default:
		hal_device_property_set_string (d, "scsi.type", "unknown");
	}

out:
	return d;
}

static gboolean
scsi_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_scsi_device_lun%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_int (d, "scsi.lun"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
mmc_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	gint host_num, rca, manfid, oemid;
	gchar *scr;

	if (parent_dev == NULL) {
		d = NULL;
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "mmc");
	hal_device_property_set_string (d, "info.bus", "mmc");
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);
	sscanf (bus_id, "mmc%d:%x", &host_num, &rca);
	hal_device_property_set_int (d, "mmc.rca", rca);
	
	hal_util_set_string_from_file (d, "mmc.cid", sysfs_path, "cid");
	hal_util_set_string_from_file (d, "mmc.csd", sysfs_path, "csd");
	
	scr = hal_util_get_string_from_file (sysfs_path, "scr");
	if (scr != NULL) {
		if (strcmp (scr, "0000000000000000") == 0)
			scr = NULL;
		else
			hal_device_property_set_string (d, "mmc.scr", scr);
	}

	if (!hal_util_set_string_from_file (d, "info.product", sysfs_path, "name")) {
		if (scr != NULL) {
			hal_device_property_set_string (d, "info.product", "SD Card");
			hal_device_property_set_string (d, "mmc.product", "SD Card");
		} else {
			hal_device_property_set_string (d, "mmc.product", "MMC Card");
                }
	}
	
	if (hal_util_get_int_from_file (sysfs_path, "manfid", &manfid, 16)) {
		/* TODO: Here we should have a mapping to a name */
		char vendor[256];
		snprintf(vendor, 256, "Unknown (%d)", manfid);
		hal_device_property_set_string (d, "info.vendor", vendor);
		hal_device_property_set_string (d, "mmc.vendor", vendor);
	}
	if (hal_util_get_int_from_file (sysfs_path, "oemid", &oemid, 16)) {
		/* TODO: Here we should have a mapping to a name */
		char oem[256];
		snprintf(oem, 256, "Unknown (%d)", oemid);
		hal_device_property_set_string (d, "mmc.oem", oem);
	}

	hal_util_set_string_from_file (d, "mmc.date", sysfs_path, "date");
	hal_util_set_int_from_file (d, "mmc.hwrev", sysfs_path, "hwrev", 16);
	hal_util_set_int_from_file (d, "mmc.fwrev", sysfs_path, "fwrev", 16);
	hal_util_set_int_from_file (d, "mmc.serial", sysfs_path, "serial", 16);

out:
	return d;
}

static gboolean
mmc_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_mmc_card_rca%d",
			      hal_device_property_get_string (d, "info.parent"),
			      hal_device_property_get_int (d, "mmc.rca"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
xen_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *devtype;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "xen");
	hal_device_property_set_string (d, "info.bus", "xen");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "xen.bus_id",
					hal_util_get_last_element (sysfs_path));

	hal_util_set_string_from_file (d, "xen.path", sysfs_path, "nodename");

	devtype = hal_util_get_string_from_file (sysfs_path, "devtype");
	if (devtype != NULL) {
		hal_device_property_set_string (d, "xen.type", devtype);

		if (strcmp (devtype, "pci") == 0) {
			hal_device_property_set_string (d, "info.product", "Xen PCI Device");
		} else if (strcmp (devtype, "vbd") == 0) {
			hal_device_property_set_string (d, "info.product", "Xen Virtual Block Device");
		} else if (strcmp (devtype, "vif") == 0) {
			hal_device_property_set_string (d, "info.product", "Xen Virtual Network Device");
		} else if (strcmp (devtype, "vtpm") == 0) {
			hal_device_property_set_string (d, "info.product", "Xen Virtual Trusted Platform Module");
		} else {
			char buf[64];
			g_snprintf (buf, sizeof (buf), "Xen Device (%s)", devtype);
			hal_device_property_set_string (d, "info.product", buf);
		}
	} else {
		 hal_device_property_set_string (d, "info.product", "Xen Device (unknown)");
	}

	return d;
}

static gboolean
xen_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/xen_%s",
			      hal_device_property_get_string (d, "xen.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
ieee1394_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	long long unsigned int guid;
	gint host_id;
	const gchar *bus_id;
	gchar buf[64];

	d = NULL;

	if (parent_dev == NULL)
		goto out;

	/* skip the useless class devices, which should be removed from the kernel */
	if (strstr(sysfs_path, "/class/") != NULL)
		goto out;

	bus_id = hal_util_get_last_element (sysfs_path);

	if (sscanf (bus_id, "fw-host%d", &host_id) == 1)
		goto out;

	if (sscanf (bus_id, "%llx-%d", &guid, &host_id) !=2 )
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "ieee1394");
	hal_device_property_set_string (d, "info.bus", "ieee1394");
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_uint64 (d, "ieee1394.guid", guid);
	hal_util_set_int_from_file    (d, "ieee1394.vendor_id", sysfs_path, "../vendor_id", 16);
	hal_util_set_int_from_file    (d, "ieee1394.specifier_id", sysfs_path, "specifier_id", 16);
	hal_util_set_int_from_file    (d, "ieee1394.version", sysfs_path, "version", 16);

	if (!hal_util_set_string_from_file (d, "ieee1394.vendor", sysfs_path, "../vendor_oui")) {
		g_snprintf (buf, sizeof (buf), "Unknown (0x%06x)", 
			    hal_device_property_get_int (d, "ieee1394.vendor_id"));
		hal_device_property_set_string (d, "ieee1394.vendor", buf);
	}

	/* not all devices have product_id */
	if (hal_util_set_int_from_file    (d, "ieee1394.product_id", sysfs_path, "model_id", 16)) {
		if (!hal_util_set_string_from_file (d, "ieee1394.product", sysfs_path, "model_name_kv")) {
			g_snprintf (buf, sizeof (buf), "Unknown (0x%06x)", 
				    hal_device_property_get_int (d, "ieee1394.product_id"));
			hal_device_property_set_string (d, "ieee1394.product", buf);
		}
	} else {
		hal_device_property_set_int (d, "ieee1394.product_id", 0x000000);
		hal_device_property_set_string (d, "ieee1394.product",
						hal_device_property_get_string (d, "ieee1394.vendor"));
	}
		
	hal_device_property_set_string (d, "info.vendor",
					hal_device_property_get_string (d, "ieee1394.vendor"));
	hal_device_property_set_string (d, "info.product",
					hal_device_property_get_string (d, "ieee1394.product"));

out:
	return d;
}

static gboolean
ieee1394_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/ieee1394_guid_%0llx",
			      hal_device_property_get_uint64 (d, "ieee1394.guid"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

#define CSR_OFFSET	0x40
#define CSR_LEAF	0x80
#define CSR_DIRECTORY	0xc0

#define CSR_DESCRIPTOR		0x01
#define CSR_VENDOR		0x03
#define CSR_HARDWARE_VERSION	0x04
#define CSR_NODE_CAPABILITIES	0x0c
#define CSR_UNIT		0x11
#define CSR_SPECIFIER_ID	0x12
#define CSR_VERSION		0x13
#define CSR_DEPENDENT_INFO	0x14
#define CSR_MODEL		0x17
#define CSR_INSTANCE		0x18

#define SBP2_COMMAND_SET_SPECIFIER	0x38
#define SBP2_COMMAND_SET		0x39
#define SBP2_COMMAND_SET_REVISION	0x3b
#define SBP2_FIRMWARE_REVISION		0x3c

static char *
decode_textual_descriptor(uint32_t *block, char *buffer, size_t size)
{
	unsigned int length;
	uint32_t *p, *end;
	
	length = block[0] >> 16;
	if (block[1] != 0 || block[2] != 0) {
		snprintf(buffer, length, "unknown encoding/language: 0x%08x/0x%08x\n",
			 buffer[1], buffer[2]);
		return buffer;
	}
	
	p = &block[3];
	memset(buffer, 0, size);
	if (length - 2 > size / 4)
		end = &block[3 + size / 4];
	else
		end = &block[length + 1];
	
	while (p < end) {
		* (uint32_t *) buffer = __cpu_to_be32(*p);
		buffer += 4;
		p++;
	}
	
	return buffer;
}

#define ARRAY_LENGTH(a) (sizeof(a)/sizeof((a)[0]))


struct csr_iterator {
	uint32_t *p;
	uint32_t *end;
};

static void
csr_iterator_init(struct csr_iterator *ci, uint32_t *p)
{
	ci->p = p + 1;
	ci->end = ci->p + (p[0] >> 16);
}

static int
csr_iterator_next(struct csr_iterator *ci, int *key, int *value)
{
	*key = *ci->p >> 24;
	*value = *ci->p & 0xffffff;

	return ci->p++ < ci->end;
}

static void
firewire_parse_config_rom (HalDevice *d, uint32_t *rom)
{
	struct csr_iterator ci;
	int key, value, last_key = 0, vendor = 0, model = 0;
	uint64_t guid;
	char buffer[256];
	
	guid = ((__u64)rom[3] << 32) | rom[4];
	hal_device_property_set_uint64 (d, "ieee1394.guid", guid);
	
	csr_iterator_init(&ci, rom + 5);
	while (csr_iterator_next(&ci, &key, &value)) {
		switch (key) {
		case CSR_VENDOR:
			vendor = value;
			hal_device_property_set_int (d, "ieee1394.vendor_id", vendor);
			break;
		case CSR_MODEL:
			model = value;
			hal_device_property_set_int (d, "ieee1394.product_id", vendor);
			break;
		case CSR_DESCRIPTOR | CSR_LEAF:
			if (last_key == CSR_VENDOR) {
				decode_textual_descriptor(ci.p - 1 + value,
							  buffer, sizeof buffer);
				hal_device_property_set_string (d, "ieee1394.vendor", buffer);
			} else if (last_key == CSR_MODEL) {
				decode_textual_descriptor(ci.p - 1 + value,
							  buffer, sizeof buffer);
				hal_device_property_set_string (d, "ieee1394.product", buffer);
			}
			break;
		case CSR_HARDWARE_VERSION:
			hal_device_property_set_int (d, "ieee1394.hardware_version", value);
			break;
			
		}

		last_key = key;
	}
}


static void
decode_sbp2_entry (HalDevice *d, int key, int value)
{
	switch (key) {
	case SBP2_FIRMWARE_REVISION:
		hal_device_property_set_int (d, "ieee1394_unit.sbp2.firmware_revision", value);
		break;
	}
}

struct specifier {
	int specifier_id;
	int version;
	void (*decode_entry) (HalDevice *d, int key, int value);
} specifiers[] = {
	{
		0x00609e,
		0x010483,
		decode_sbp2_entry
	}
};

static void
firewire_unit_parse_config_rom (HalDevice *d, uint32_t *rom, int index)
{
	struct csr_iterator ci;
	unsigned int i;
	int key;
	int value;
	int specifier_id = 0;
	int version = 0;
	
	csr_iterator_init(&ci, rom + index);
	while (csr_iterator_next(&ci, &key, &value)) {
		switch (key) {
		case CSR_SPECIFIER_ID:
			specifier_id = value;
			hal_device_property_set_int (d, "ieee1394_unit.specifier_id", specifier_id);
			break;
		case CSR_VERSION:
			version = value;
			hal_device_property_set_int (d, "ieee1394_unit.version", version);
			break;
			
		default:
			if (key < 0x38)
				break;
			
			/* Specifier dependent key/value pair. */
			for (i = 0; i < ARRAY_LENGTH(specifiers); i++) {
				if (specifiers[i].specifier_id == specifier_id &&
				    specifiers[i].version == version) {
					specifiers[i].decode_entry (d, key, value);
					break;
				}
			}
		}
	}
	
	if (specifier_id == 0x00609e && version == 0x010483) {
		hal_device_add_capability (d, "ieee1394_unit.sbp2");
	} else if (specifier_id == 0x00a02d) {
		if ((version & 0xffff00) == 0x000100)
			hal_device_add_capability (d, "ieee1394_unit.iidc");
		if ((version & 0xff0001) == 0x010001)
			hal_device_add_capability (d, "ieee1394_unit.avc");
		if ((version & 0xff0002) == 0x010002)
			hal_device_add_capability (d, "ieee1394_unit.cal");
		if ((version & 0xff0004) == 0x010004)
			hal_device_add_capability (d, "ieee1394_unit.ehs");
		if ((version & 0xff0004) == 0x010008)
			hal_device_add_capability (d, "ieee1394_unit.havi");
	} else if (specifier_id == 0x000b09d && version == 0x800002) {
		/* Workaround for pointgrey cameras taken from libdc1394 */
		hal_device_add_capability (d, "ieee1394_unit.iidc");
	}

#if 0
	/* TODO */

	/* In the AV/C case we should send a unit info command to the
	 * device to see what kind of AV/C device it is (audio, camcorder,
	 * etc).  We might want to do this in a helper app.  And we need
	 * the device file for this...  This will be another 200 lines of
	 * code.
	 */
	if (specifier_id == 0x00a02d && (version & 0xff0001) == 0x010001) {
		query_unit_info("/dev/fw1.0");
	}
#endif
}


static HalDevice *
firewire_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	int device_id;
	int unit_id;
	const char *bus_id;
	gboolean is_device = FALSE;
	gboolean is_unit = FALSE;
	char rom[256];
	ssize_t rom_size;
	char *str;
	int fd;

	d = NULL;

	if (parent_dev == NULL)
		goto out;

	bus_id = hal_util_get_last_element (sysfs_path);

	if (sscanf (bus_id, "fw%d.%d", &device_id, &unit_id) == 2 ) {
		is_unit = TRUE;
	} else if (sscanf (bus_id, "fw%d", &device_id) == 1) {
		is_device = TRUE;
	} else {
		goto out;
	}

	if (is_device) {

		if (device_file == NULL)
			goto out;

		str = g_strdup_printf ("%s/config_rom", sysfs_path);
		fd = open (str, O_RDONLY);
		if (fd < 0) {
			HAL_ERROR (("Cannot open firewire config rom at %s", str));
			g_free (str);
			goto out;
		}
		if ((rom_size = read (fd, rom, sizeof (rom))) < 0) {
			HAL_ERROR (("Cannot read firewire config rom at %s", str));
			g_free (str);
			close (fd);
			goto out;
		}
		g_free (str);
		close (fd);

		HAL_INFO (("firewire config rom is %d bytes", rom_size));

		d = hal_device_new ();
		hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
		hal_device_property_set_string (d, "info.subsystem", "ieee1394");
		hal_device_property_set_string (d, "info.bus", "ieee1394");
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
		hal_device_add_capability (d, "ieee1394");
		hal_device_property_set_string (d, "ieee1394.device", device_file);
		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		firewire_parse_config_rom (d, (uint32_t *) rom);
	} else {
		int rom_index;

		str = g_strdup_printf ("%s/../config_rom", sysfs_path);
		fd = open (str, O_RDONLY);
		if (fd < 0) {
			HAL_ERROR (("Cannot open firewire config rom at %s", str));
			g_free (str);
			goto out;
		}
		if ((rom_size = read (fd, rom, sizeof (rom))) < 0) {
			HAL_ERROR (("Cannot read firewire config rom at %s", str));
			g_free (str);
			close (fd);
			goto out;
		}
		g_free (str);
		close (fd);

		if (!hal_util_get_int_from_file (sysfs_path, "rom_index", &rom_index, 0)) {
			HAL_ERROR (("Cannot read get %s/rom_index", sysfs_path));
			goto out;
		}

		HAL_INFO (("firewire config rom is %d bytes - unit rom index is %d", rom_size, rom_index));

		d = hal_device_new ();
		hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
		hal_device_property_set_string (d, "info.subsystem", "ieee1394_unit");
		hal_device_property_set_string (d, "info.bus", "ieee1394_unit");
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
		hal_device_property_set_string (d, "ieee1394_unit.originating_device", 
						hal_device_get_udi (parent_dev));
		hal_device_property_set_int (d, "ieee1394_unit.unit_index", unit_id);
		hal_device_add_capability (d, "ieee1394_unit");
		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		firewire_unit_parse_config_rom (d, (uint32_t *) rom, rom_index);
	}


out:
	return d;
}

static const gchar *
firewire_get_prober (HalDevice *d)
{
	const char *prober = NULL;

	/* run prober only for AVC devices */
	if (hal_device_has_capability (d, "ieee1394_unit.avc")) {
		prober = "hald-probe-ieee1394-unit";
	}

	return prober;
}

static gboolean
firewire_post_probing (HalDevice *d)
{
	return TRUE;
}


static gboolean
firewire_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_has_capability (d, "ieee1394")) {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "/org/freedesktop/Hal/devices/ieee1394_guid%0llx",
				      hal_device_property_get_uint64 (d, "ieee1394.guid"));
	} else {
		hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
				      "%s_unit%d",
				      hal_device_property_get_string (d, "ieee1394_unit.originating_device"),
				      hal_device_property_get_int (d, "ieee1394_unit.unit_index"));
	}

	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static inline void
ccw_add_dasd_properties (HalDevice *d, const gchar *sysfs_path)
{
	const gchar *disc;
	
	hal_util_set_int_from_file (d, "ccw.dasd.use_diag", sysfs_path,
				    "use_diag", 2);
	hal_util_set_int_from_file (d, "ccw.dasd.readonly", sysfs_path,
				    "readonly", 2);
	disc = hal_util_get_string_from_file (sysfs_path, "discipline");
	if (disc)
		hal_device_property_set_string(d, "ccw.dasd.discipline", disc);
}

static inline void
ccw_add_zfcp_properties (HalDevice *d, const gchar *sysfs_path)
{
	int online;

	/* zfcp adapter properties are only valid for online devices. */
	if (!hal_util_get_int_from_file (sysfs_path, "online", &online, 2))
		return;
	if (!online)
		return;

	hal_util_set_int_from_file (d, "ccw.zfcp.in_recovery", sysfs_path,
				    "in_recovery", 2);
	hal_util_set_int_from_file (d, "ccw.zfcp.failed", sysfs_path,
				    "failed", 2);
}

static inline void
ccw_add_tape_properties (HalDevice *d, const gchar *sysfs_path)
{
	int medium_state, online;

	const gchar *state_text[3] = {"unknown", "loaded", "no medium"};

	hal_util_set_string_from_file (d, "ccw.tape.state", sysfs_path, "state");
	hal_util_set_string_from_file (d, "ccw.tape.operation", sysfs_path,
				       "operation");
	/* The following properties are only valid for online devices. */
	if (!hal_util_get_int_from_file (sysfs_path, "online", &online, 2))
		return;
	if (!online)
		return;
	hal_util_set_int_from_file (d, "ccw.tape.blocksize", sysfs_path,
				    "blocksize", 10);
	if (!hal_util_get_int_from_file (sysfs_path, "medium_state",
					&medium_state, 10))
		return;
	hal_device_property_set_string (d, "ccw.tape.medium_state",
					state_text[medium_state]);
}

static inline void
ccw_add_3270_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_int_from_file (d, "ccw.3270.model", sysfs_path,
				    "model", 10);
	hal_util_set_int_from_file (d, "ccw.3270.rows", sysfs_path, "rows", 10);
	hal_util_set_int_from_file (d, "ccw.3270.columns", sysfs_path,
				    "columns", 10);
}

static HalDevice *
ccw_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	const gchar *pimpampom;
	int pim, pam, pom;
	const gchar *chpids;
	int chpid[8];
	gchar attr[25];
	int i;
	gchar driver_name[256];

	bus_id = hal_util_get_last_element (sysfs_path);

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "ccw");
	hal_device_property_set_string (d, "info.bus", "ccw");
	if (parent_dev != NULL)
                hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
        else
                hal_device_property_set_string
		  (d, "info.parent",
		   "/org/freedesktop/Hal/devices/computer");

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "ccw.bus_id", bus_id);
	hal_util_set_int_from_file (d, "ccw.online", sysfs_path, "online", 2);
	hal_util_set_string_from_file (d, "ccw.availablity", sysfs_path,
				       "availability");
	hal_util_set_int_from_file (d, "ccw.cmb_enable", sysfs_path,
				    "cmb_enable", 2);
	hal_util_set_string_from_file (d, "ccw.cutype", sysfs_path, "cutype");
	hal_util_set_string_from_file (d, "ccw.devtype", sysfs_path, "devtype");

	/* Get some values from the higher level subchannel structure.*/
	pimpampom = hal_util_get_string_from_file (sysfs_path, "../pimpampom");
	if (pimpampom) {
		sscanf (pimpampom, "%x %x %x", &pim, &pam, &pom);
		hal_device_property_set_int (d, "ccw.subchannel.pim", pim);
		hal_device_property_set_int (d, "ccw.subchannel.pam", pam);
		hal_device_property_set_int (d, "ccw.subchannel.pom", pom);
	}

	chpids = hal_util_get_string_from_file (sysfs_path, "../chpids");
	if (chpids) {
		sscanf (chpids, "%x %x %x %x %x %x %x %x", &chpid[0], &chpid[1],
			&chpid[2], &chpid[3], &chpid[4], &chpid[5], &chpid[6],
			&chpid[7]);
		for (i=0; i<8 && (chpid[i] != 0); i++) {
			g_snprintf (attr, sizeof (attr),
				    "ccw.subchannel.chpid%x", i);
			hal_device_property_set_int (d, attr, chpid[i]);
		}
	}

	/* Add some special properties. */
	if (hal_util_get_driver_name (sysfs_path, driver_name)) {
		if (!strncmp (driver_name, "dasd", 4))
			/* Same attributes for dasd_eckd and dasd_fba. */
			ccw_add_dasd_properties (d, sysfs_path);
		if (!strncmp (driver_name, "zfcp", 4))
			ccw_add_zfcp_properties (d, sysfs_path);
		if (!strncmp (driver_name, "tape_3", 6))
			/* For all channel attached tapes. */
			ccw_add_tape_properties (d, sysfs_path);
		if (!strncmp (driver_name, "3270", 4))
			ccw_add_3270_properties (d, sysfs_path);
	}
	return d;
}

static gboolean
ccw_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/ccw_%s",
			      hal_device_property_get_string
			      (d, "ccw.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static inline void
ccwgroup_add_qeth_properties (HalDevice *d, const gchar *sysfs_path)
{
	int is_layer2;

	/* Some attributes are not applicable for devices in layer2 mode. */
	hal_util_get_int_from_file (sysfs_path, "layer2", &is_layer2, 2);

	hal_util_set_string_from_file (d, "ccwgroup.qeth.large_send",
				       sysfs_path, "large_send");
	hal_util_set_string_from_file (d, "ccwgroup.qeth.card_type", sysfs_path,
				       "card_type");
	hal_util_set_string_from_file (d, "ccwgroup.qeth.checksumming",
				       sysfs_path, "checksumming");
	if (!is_layer2) {
		/* CH: the next two are only valid for token ring devices */
		hal_util_set_int_from_file (d,
					    "ccwgroup.qeth.canonical_macaddr",
					    sysfs_path, "canonical_macaddr", 2);
		hal_util_set_string_from_file (d,
					       "ccwgroup.qeth.broadcast_mode",
					       sysfs_path, "broadcast_mode");
		hal_util_set_int_from_file (d, "ccwgroup.qeth.fake_broadcast",
					    sysfs_path, "fake_broadcast", 2);
		hal_util_set_int_from_file (d, "ccwgroup.qeth.fake_ll",
					    sysfs_path, "fake_ll", 2);
	}
	hal_device_property_set_int (d, "ccwgroup.qeth.layer2", is_layer2);
	hal_util_set_string_from_file (d, "ccwgroup.qeth.portname", sysfs_path,
				       "portname");
	hal_util_set_int_from_file (d, "ccwgroup.qeth.portno", sysfs_path,
				    "portno", 10);
	hal_util_set_int_from_file (d, "ccwgroup.qeth.buffer_count", sysfs_path,
				    "buffer_count", 10);
	hal_util_set_int_from_file (d, "ccwgroup.qeth.add_hhlen", sysfs_path,
				    "add_hhlen", 10);
	hal_util_set_string_from_file (d, "ccwgroup.qeth.priority_queueing",
				       sysfs_path, "priority_queueing");
	if (!is_layer2) {
		hal_util_set_string_from_file (d, "ccwgroup.qeth.route4",
					       sysfs_path, "route4");
		hal_util_set_string_from_file (d, "ccwgroup.qeth.route6",
					       sysfs_path, "route6");
	}
	hal_util_set_string_from_file (d, "ccwgroup.qeth.state", sysfs_path,
				       "state");
}

static inline void
ccwgroup_add_ctc_properties (HalDevice *d, const gchar *sysfs_path)
{
	/* CH: use protocol descriptions? */
	hal_util_set_int_from_file (d, "ccwgroup.ctc.protocol", sysfs_path,
				    "protocol", 2);
	hal_util_set_string_from_file (d, "ccwgroup.ctc.type", sysfs_path,
				       "type");
	hal_util_set_int_from_file (d, "ccwgroup.ctc.buffer", sysfs_path,
				    "buffer", 10);
}

static inline void
ccwgroup_add_lcs_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_int_from_file (d, "ccwgroup.lcs.portnumber", sysfs_path,
				    "portno", 10);
	hal_util_set_string_from_file (d, "ccwgroup.lcs.type", sysfs_path,
				       "type");
	hal_util_set_int_from_file (d, "ccwgroup.lcs.lancmd_timeout",
				    sysfs_path, "lancmd_timeout", 10);
}

static inline void
ccwgroup_add_claw_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_string_from_file (d, "ccwgroup.claw.api_type", sysfs_path,
				       "api_type");
	hal_util_set_string_from_file (d, "ccwgroup.claw.adapter_name",
				       sysfs_path, "adapter_name");
	hal_util_set_string_from_file (d, "ccwgroup.claw.host_name", sysfs_path,
				       "host_name");
	hal_util_set_int_from_file (d, "ccwgroup.claw.read_buffer", sysfs_path,
				    "read_buffer", 10);
	hal_util_set_int_from_file (d, "ccwgroup.claw.write_buffer", sysfs_path,
				    "write_buffer", 10);
}

static HalDevice *
ccwgroup_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	gchar driver_name[256];

	bus_id = hal_util_get_last_element (sysfs_path);

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "ccwgroup");
	hal_device_property_set_string (d, "info.bus", "ccwgroup");
	if (parent_dev != NULL)
                hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
        else
                hal_device_property_set_string
		  (d, "info.parent",
		   "/org/freedesktop/Hal/devices/computer");

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "ccwgroup.bus_id", bus_id);
	hal_util_set_int_from_file (d, "ccwgroup.online", sysfs_path,
				    "online", 2);

	/* Some devices have extra properties. */
	if (hal_util_get_driver_name (sysfs_path, driver_name)) {
		if (!strncmp (driver_name, "qeth", 4))
			ccwgroup_add_qeth_properties (d, sysfs_path);
		if (!strncmp (driver_name, "ctc", 3))
			ccwgroup_add_ctc_properties (d, sysfs_path);
		if (!strncmp (driver_name, "lcs", 3))
			ccwgroup_add_lcs_properties (d, sysfs_path);
		if (!strncmp (driver_name, "claw", 4))
			ccwgroup_add_claw_properties (d, sysfs_path);
	}
	return d;
}

static gboolean
ccwgroup_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/ccwgroup_%s",
			      hal_device_property_get_string
			      (d, "ccwgroup.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static inline void
iucv_add_netiucv_properties (HalDevice *d, const gchar *sysfs_path)
{
	hal_util_set_string_from_file (d, "iucv.netiucv.user", sysfs_path,
				       "user");
	hal_util_set_int_from_file (d, "iucv.netiucv.buffer", sysfs_path,
				    "buffer", 10);
}

static HalDevice *
iucv_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	gchar driver_name[256];

	bus_id = hal_util_get_last_element (sysfs_path);

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "iucv");
	hal_device_property_set_string (d, "info.bus", "iucv");
	if (parent_dev != NULL)
                hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
        else
                hal_device_property_set_string
		  (d, "info.parent",
		   "/org/freedesktop/Hal/devices/computer");

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "iucv.bus_id", bus_id);

	if (hal_util_get_driver_name (sysfs_path, driver_name)) {
		if (!strncmp (driver_name, "netiucv", 7))
			iucv_add_netiucv_properties (d, sysfs_path);
	}
	return d;
}

static gboolean
iucv_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/iucv_%s",
			      hal_device_property_get_string
			      (d, "iucv.bus_id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

static HalDevice *
backlight_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev,
	       const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	int max_brightness;

	d = hal_device_new ();
	hal_device_add_capability (d, "laptop_panel");
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	hal_device_property_set_string (d, "info.category", "laptop_panel");
	hal_device_property_set_string (d, "info.product", "Generic Backlight Device");
	hal_device_property_set_string (d, "laptop_panel.access_method", "general");

	hal_util_get_int_from_file (sysfs_path, "max_brightness", &max_brightness, 10);
	hal_device_property_set_int (d, "laptop_panel.num_levels", max_brightness + 1);
	return d;
}

static gboolean
backlight_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const char *dir;
	const char *name;

	dir = hal_device_property_get_string (d, "linux.sysfs_path");

	name = hal_util_get_last_element(dir);
	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_backlight",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pseudo_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "pseudo");
	hal_device_property_set_string (d, "info.bus", "pseudo");
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "pseudo.id", dev_id);

	g_snprintf (buf, sizeof (buf), "SCSI Debug Device (%s)", hal_device_property_get_string (d, "pseudo.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
pseudo_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "/org/freedesktop/Hal/devices/pseudo",
			      hal_device_property_get_string (d, "platform.id"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
dev_remove (HalDevice *d)
{
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

struct DevHandler_s;
typedef struct DevHandler_s DevHandler;

struct DevHandler_s
{
	const gchar *subsystem;
	HalDevice *(*add) (const gchar *sysfs_path, const gchar *device_file,
			   HalDevice *parent_dev, const gchar *parent_path);
	const gchar *(*get_prober)(HalDevice *d);
	gboolean (*post_probing) (HalDevice *d);
	gboolean (*compute_udi) (HalDevice *d);
	gboolean (*remove) (HalDevice *d);
};

/*--------------------------------------------------------------------------------------------------------------*/

static DevHandler dev_handler_input = 
{ 
	.subsystem    = "input",
	.add          = input_add,
	.get_prober   = input_get_prober,
	.post_probing = input_post_probing,
	.compute_udi  = input_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_bluetooth = 
{ 
	.subsystem    = "bluetooth",
	.add          = bluetooth_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = bluetooth_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_net = 
{ 
	.subsystem    = "net",
	.add          = net_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = net_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_scsi_generic =
{ 
	.subsystem    = "scsi_generic",
	.add          = scsi_generic_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = scsi_generic_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_scsi_host = 
{ 
	.subsystem    = "scsi_host",
	.add          = scsi_host_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = scsi_host_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_usbclass = 
{ 
	.subsystem    = "usb",
	.add          = usbclass_add,
	.get_prober   = usbclass_get_prober,
	.post_probing = NULL,
	.compute_udi  = usbclass_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_usbraw =
{ 
	.subsystem    = "usb_device",
	.add          = usbraw_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = usbraw_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_video4linux =
{ 
	.subsystem    = "video4linux",
	.add          = video4linux_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = video4linux_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_dvb =
{ 
	.subsystem    = "dvb",
	.add          = dvb_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = dvb_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_sound = 
{ 
	.subsystem    = "sound",
	.add          = sound_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = sound_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_serial = 
{ 
	.subsystem    = "tty",
	.add          = serial_add,
	.get_prober   = serial_get_prober,
	.post_probing = NULL,
	.compute_udi  = serial_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_tape =
{
	.subsystem    = "tape",
	.add          = tape_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = tape_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_tape390 =
{
	.subsystem    = "tape390",
	.add          = tape_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = tape_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_mmc_host =
{
	.subsystem    = "mmc_host",
	.add          = mmc_host_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = mmc_host_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_pci = { 
	.subsystem   = "pci",
	.add         = pci_add,
	.compute_udi = pci_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_usb = { 
	.subsystem   = "usb",
	.add         = usb_add,
	.compute_udi = usb_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_ide = { 
	.subsystem   = "ide",
	.add         = ide_add,
	.compute_udi = ide_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_pnp = { 
	.subsystem   = "pnp",
	.add         = pnp_add,
	.compute_udi = pnp_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_platform = {
	.subsystem   = "platform",
	.add         = platform_add,
	.compute_udi = platform_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_serio = { 
	.subsystem   = "serio",
	.add         = serio_add,
	.compute_udi = serio_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_pcmcia = { 
	.subsystem   = "pcmcia",
	.add         = pcmcia_add,
	.compute_udi = pcmcia_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_scsi = { 
	.subsystem   = "scsi",
	.add         = scsi_add,
	.compute_udi = scsi_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_mmc = { 
	.subsystem   = "mmc",
	.add         = mmc_add,
	.compute_udi = mmc_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_ieee1394 = { 
	.subsystem   = "ieee1394",
	.add         = ieee1394_add,
	.compute_udi = ieee1394_compute_udi,
	.remove      = dev_remove
};

/* krh's new firewire stack */
static DevHandler dev_handler_firewire = { 
	.subsystem    = "firewire",
	.add          = firewire_add,
	.get_prober   = firewire_get_prober,
	.post_probing = firewire_post_probing,
	.compute_udi  = firewire_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_xen = {
	.subsystem   = "xen",
	.add         = xen_add,
	.compute_udi = xen_compute_udi,
	.remove      = dev_remove
};

/* s390 specific busses */
static DevHandler dev_handler_ccw = {
	.subsystem   = "ccw",
	.add         = ccw_add,
	.compute_udi = ccw_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_ccwgroup = {
	.subsystem   = "ccwgroup",
	.add         = ccwgroup_add,
	.compute_udi = ccwgroup_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_iucv = {
	.subsystem   = "iucv",
	.add         = iucv_add,
	.compute_udi = iucv_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_backlight =
{
       .subsystem    = "backlight",
       .add          = backlight_add,
       .compute_udi  = backlight_compute_udi,
       .remove       = dev_remove
};

/* SCSI debug, to test thousends of fake devices */
static DevHandler dev_handler_pseudo = {
	.subsystem   = "pseudo",
	.add         = pseudo_add,
	.compute_udi = pseudo_compute_udi,
	.remove      = dev_remove
};

/*--------------------------------------------------------------------------------------------------------------*/

static DevHandler *dev_handlers[] = {
	&dev_handler_pci,
	&dev_handler_usb,
	&dev_handler_ide,
	&dev_handler_pnp,
	&dev_handler_platform,
	&dev_handler_serio,
	&dev_handler_pcmcia,
	&dev_handler_scsi,
	&dev_handler_mmc,
	&dev_handler_ieee1394,
	&dev_handler_xen,
	&dev_handler_ccw,
	&dev_handler_ccwgroup,
	&dev_handler_iucv,
	&dev_handler_pseudo,
	&dev_handler_input,
	&dev_handler_bluetooth,
	&dev_handler_net,
	&dev_handler_scsi_generic,
	&dev_handler_scsi_host,
	&dev_handler_usbclass,
	&dev_handler_usbraw,
	&dev_handler_video4linux,
	&dev_handler_dvb,
	&dev_handler_sound,
	&dev_handler_serial,
	&dev_handler_tape,
	&dev_handler_tape390,
	&dev_handler_mmc_host,
	&dev_handler_backlight,
	&dev_handler_firewire,
	NULL
};

/*--------------------------------------------------------------------------------------------------------------*/

static void 
dev_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
dev_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Remove callouts completed udi=%s", hal_device_get_udi (d)));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	g_object_unref (d);

	hotplug_event_end (end_token);
}

static void 
add_dev_after_probing (HalDevice *d, DevHandler *handler, void *end_token)
{
	/* Compute UDI */
	if (!handler->compute_udi (d)) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		hotplug_event_end (end_token);
		goto out;
	}
	
	/* Merge properties from .fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);
	
	/* TODO: Merge persistent properties */

	/* Run callouts */
	hal_util_callout_device_add (d, dev_callouts_add_done, end_token, NULL);

out:
	;
}

static void 
add_dev_probing_helper_done (HalDevice *d, guint32 exit_type, 
                                  gint return_code, char **error,
                                  gpointer data1, gpointer data2) 
{
	void *end_token = (void *) data1;
	DevHandler *handler = (DevHandler *) data2;

	HAL_INFO (("entering; exit_type=%d, return_code=%d", exit_type, return_code));

	if (d == NULL) {
		HAL_INFO (("Device object already removed"));
		hotplug_event_end (end_token);
		goto out;
	}

	/* Discard device if probing reports failure */
	if (exit_type != HALD_RUN_SUCCESS || return_code != 0) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		hotplug_event_end (end_token);
		goto out;
	}

	/* Do things post probing */
	if (handler->post_probing != NULL) {
		if (!handler->post_probing (d)) {
			hotplug_event_end (end_token);
			goto out;
		}
	}

	add_dev_after_probing (d, handler, end_token);

out:
	;
}

static void 
dev_callouts_preprobing_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;
	DevHandler *handler = (DevHandler *) userdata2;
	const gchar *prober;

	if (hal_device_property_get_bool (d, "info.ignore")) {
		/* Leave the device here with info.ignore==TRUE so we won't pick up children 
		 * Also remove category and all capabilities
		 */
		hal_device_property_remove (d, "info.category");
		hal_device_property_remove (d, "info.capabilities");
		hal_device_property_set_string (d, "info.udi", "/org/freedesktop/Hal/devices/ignored-device");
		hal_device_property_set_string (d, "info.product", "Ignored Device");

		HAL_INFO (("Preprobing merged info.ignore==TRUE"));
		
		/* Move from temporary to global device store */
		hal_device_store_remove (hald_get_tdl (), d);
		hal_device_store_add (hald_get_gdl (), d);
		
		hotplug_event_end (end_token);
		goto out;
	}
	
	if (handler->get_prober != NULL)
		prober = handler->get_prober (d);
	else
		prober = NULL;
	if (prober != NULL) {
		/* probe the device */
		hald_runner_run(d, 
		                    prober, NULL, 
		                    HAL_HELPER_TIMEOUT, 
		                    add_dev_probing_helper_done,
		                    (gpointer) end_token, (gpointer) handler);
	} else {
		add_dev_after_probing (d, handler, end_token);
	}
out:
  ;
}

void
hotplug_event_begin_add_dev (const gchar *subsystem, const gchar *sysfs_path, const gchar *device_file,
				  HalDevice *parent_dev, const gchar *parent_path,
				  void *end_token)
{
	guint i;

	HAL_INFO (("add_dev: subsys=%s sysfs_path=%s dev=%s parent_dev=0x%08x", subsystem, sysfs_path, device_file, parent_dev));

	/* update driver property of the parent device, cause manual driver bind/unbind
	 * may change change this without sending events for the bus device
	 */
	if (parent_dev != NULL)
		hal_util_set_driver (parent_dev, "info.linux.driver", parent_path);

	if (parent_dev != NULL && hal_device_property_get_bool (parent_dev, "info.ignore")) {
		HAL_INFO (("Ignoring add_dev since parent_dev has info.ignore==TRUE"));
		hotplug_event_end (end_token);
		goto out;
	}

	for (i = 0; dev_handlers [i] != NULL; i++) {
		DevHandler *handler;

		handler = dev_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			HalDevice *d;

			if (strcmp (subsystem, "scsi") == 0)
				if (missing_scsi_host(sysfs_path, (HotplugEvent *)end_token, HOTPLUG_ACTION_ADD))
					goto out;

			/* attempt to add the device */
			d = handler->add (sysfs_path, device_file, parent_dev, parent_path);
			if (d == NULL) {
				/* didn't find anything - thus, ignore this hotplug event */
				hotplug_event_end (end_token);
				goto out;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_SYSFS_DEVICE);
			hal_device_property_set_string (d, "linux.subsystem", subsystem);

			if (device_file != NULL && strlen (device_file) > 0)
				hal_device_property_set_string (d, "linux.device_file", device_file);

			/* Add to temporary device store */
			hal_device_store_add (hald_get_tdl (), d);

			/* Process preprobe fdi files */
			di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);

			/* Run preprobe callouts */
			hal_util_callout_device_preprobe (d, dev_callouts_preprobing_done, end_token, handler);
			goto out;
		}
	}

	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out:
	;
}

void
hotplug_event_begin_remove_dev (const gchar *subsystem, const gchar *sysfs_path, void *end_token)
{
	guint i;
	HalDevice *d;


	HAL_INFO (("remove_dev: subsys=%s sysfs_path=%s", subsystem, sysfs_path));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Error removing device"));
	} else {
		for (i = 0; dev_handlers [i] != NULL; i++) {
			DevHandler *handler;

			handler = dev_handlers[i];
			if (strcmp (handler->subsystem, subsystem) == 0) {
				if (strcmp (subsystem, "scsi") == 0)
					missing_scsi_host(sysfs_path, (HotplugEvent *)end_token, HOTPLUG_ACTION_REMOVE);

				handler->remove (d);
				hal_util_callout_device_remove (d, dev_callouts_remove_done, end_token, NULL);
				goto out;
			}
		}
	}

	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out:
	;
}

static void 
dev_rescan_device_done (HalDevice *d, 
			guint32 exit_type, 
			gint return_code, 
			char **error,
			gpointer data1, 
			gpointer data2) 
{
	HAL_INFO (("dev_rescan_device_done: exit_type=%d, return_code=%d", exit_type, return_code));
}

gboolean
dev_rescan_device (HalDevice *d)
{
	gboolean ret;

	ret = FALSE;

	/* rescan button state on Rescan() */
	if (hal_device_property_get_bool (d, "button.has_state")) {

		hald_runner_run (d, 
				 "hald-probe-input", 
				 NULL,
				 HAL_HELPER_TIMEOUT, 
				 dev_rescan_device_done,
				 NULL, 
				 NULL);
		
		ret = TRUE;
		goto out;
	}

out:
	return ret;
}


HotplugEvent *
dev_generate_add_hotplug_event (HalDevice *d)
{
	const char *subsystem;
	const char *sysfs_path;
	const char *device_file;
	HotplugEvent *hotplug_event;

	subsystem = hal_device_property_get_string (d, "linux.subsystem");
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");
	device_file = hal_device_property_get_string (d, "linux.device_file");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_ADD;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	if (device_file != NULL)
		g_strlcpy (hotplug_event->sysfs.device_file, device_file, sizeof (hotplug_event->sysfs.device_file));
	else
		hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}

HotplugEvent *
dev_generate_remove_hotplug_event (HalDevice *d)
{
	const char *subsystem;
	const char *sysfs_path;
	HotplugEvent *hotplug_event;

	subsystem = hal_device_property_get_string (d, "linux.subsystem");
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	hotplug_event = g_new0 (HotplugEvent, 1);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}
