/***************************************************************************
 * CVSID: $Id$
 *
 * classdev.c : Handling of functional kernel devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2005 Richard Hughes, <richard@hughsie.com>
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

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../device_store.h"
#include "../hald.h"
#include "../hald_runner.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"

#include "coldplug.h"
#include "hotplug_helper.h"
#include "osspec_linux.h"

#include "classdev.h"

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
input_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (physdev != NULL) {
		hal_device_property_set_string (d, "input.physical_device", hal_device_get_udi (physdev));
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}
	hal_device_property_set_string (d, "info.category", "input");
	hal_device_add_capability (d, "input");

	hal_device_property_set_string (d, "input.device", device_file);

	return d;
}

static const gchar *
input_get_prober (HalDevice *d)
{
	return "hald-probe-input";
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
bluetooth_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, 
	       const gchar *sysfs_path_in_devices)
{
	HalDevice *d;

	d = NULL;

	if (physdev == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));

	hal_device_property_set_string (d, "info.category", "bluetooth_hci");
	hal_device_add_capability (d, "bluetooth_hci");

	hal_device_property_set_string (d, "bluetooth_hci.physical_device", hal_device_get_udi (physdev));
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
net_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	const gchar *ifname;
	guint media_type;
	gint flags;

	d = NULL;

	if (physdev == NULL)
		goto error;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));

	hal_device_property_set_string (d, "info.category", "net");
	hal_device_add_capability (d, "net");

	hal_device_property_set_string (d, "net.physical_device", hal_device_get_udi (physdev));

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
		gboolean is_wireless;
		struct stat s;

		snprintf (wireless_path, HAL_PATH_MAX, "%s/wireless", sysfs_path);
                if (stat (wireless_path, &s) == 0 && (s.st_mode & S_IFDIR)) { 
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
		id = hal_util_get_last_element(hal_device_property_get_string(d, "net.physical_device"));
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
scsi_generic_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
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

static HalDevice *
scsi_host_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	gint host_num;
	const gchar *last_elem;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path_in_devices);
	
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));

	hal_device_property_set_string (d, "info.category", "scsi_host");
	hal_device_add_capability (d, "scsi_host");

	hal_device_property_set_string (d, "info.product", "SCSI Host Adapter");

	last_elem = hal_util_get_last_element (sysfs_path);
	sscanf (last_elem, "host%d", &host_num);
	hal_device_property_set_int (d, "scsi_host.host", host_num);

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
usbclass_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	gint host_num;
	const gchar *last_elem;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL || device_file == NULL) {
		goto out;
	}

	last_elem = hal_util_get_last_element (sysfs_path);
	if (sscanf (last_elem, "hiddev%d", &host_num) == 1) {

		d = hal_device_new ();
		hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
		hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path_in_devices);
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));

		hal_device_property_set_string (d, "info.category", "hiddev");
		hal_device_add_capability (d, "hiddev");

		hal_device_property_set_string (d, "info.product", "USB HID Device");

		hal_device_property_set_string (d, "hiddev.device", device_file);
	} else if (sscanf (last_elem, "lp%d", &host_num) == 1) {

		d = hal_device_new ();
		hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
		hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path_in_devices);
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));

		hal_device_property_set_string (d, "info.category", "printer");
		hal_device_add_capability (d, "printer");

		hal_device_property_set_string (d, "info.product", "Printer");
		hal_device_property_set_string (d, "printer.device", device_file);

		hal_device_property_set_string (d, "printer.physical_device", hal_device_get_udi (physdev));
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
usbraw_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
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
video4linux_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
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
dvb_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
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


/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
sound_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
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

	if (physdev == NULL || sysfs_path_in_devices == NULL) {
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
		/* handle ALSA and OSS devices with physdev link in sys */
		if (sscanf (device, "controlC%d", &cardnum) == 1) {
			
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.device_file", device_file);
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
			hal_device_property_set_string (d, "alsa.physical_device", hal_device_get_udi (physdev));
			hal_device_property_set_int (d, "alsa.card", cardnum);
			hal_device_property_set_string (d, "alsa.type", "control");
	
			snprintf (aprocdir, sizeof (aprocdir), "%s/asound/card%d", get_hal_proc_path (), cardnum);
			hal_util_set_string_from_file (d, "alsa.card_id", aprocdir, "id");
	
			snprintf (buf, sizeof (buf), "%s ALSA Control Device", 
				hal_device_property_get_string (d, "alsa.card_id"));
			hal_device_property_set_string (d, "info.product", buf);
	
		} else if (sscanf (device, "pcmC%dD%d%c", &cardnum, &devicenum, &type) == 3) {
			
			hal_device_property_set_string (d, "info.category", "alsa");
			hal_device_add_capability (d, "alsa");
			hal_device_property_set_string (d, "alsa.device_file", device_file);
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
			hal_device_property_set_string (d, "alsa.physical_device", hal_device_get_udi (physdev));
			hal_device_property_set_int (d, "alsa.card", cardnum);
			hal_device_property_set_int (d, "alsa.device", devicenum);
	
			snprintf (aprocdir, sizeof (aprocdir), "%s/asound/card%d", get_hal_proc_path (), cardnum);
			hal_util_set_string_from_file (d, "alsa.card_id", aprocdir, "id");

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
			hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
			hal_device_property_set_string (d, "oss.physical_device", hal_device_get_udi (physdev));
			hal_device_property_set_int (d, "oss.card", cardnum);
	
			snprintf (aprocdir, sizeof (aprocdir), "%s/asound/card%d", get_hal_proc_path (), cardnum);
			hal_util_set_string_from_file (d, "oss.card_id", aprocdir, "id");
	
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
		/* handle global ALAS devices */
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
serial_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	int portnum;
	HalDevice *d;
	const gchar *last_elem;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL || device_file == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
	hal_device_property_set_string (d, "info.category", "serial");
	hal_device_add_capability (d, "serial");
	hal_device_property_set_string (d, "serial.physical_device", hal_device_get_udi (physdev));
	hal_device_property_set_string (d, "serial.device", device_file);

	last_elem = hal_util_get_last_element(sysfs_path);
	if (sscanf (last_elem, "ttyS%d", &portnum) == 1) {
		hal_device_property_set_int (d, "serial.port", portnum);
		hal_device_property_set_string (d, "serial.type", "platform");
		hal_device_property_set_string (d, "info.product",
						hal_device_property_get_string (physdev, "info.product"));
	} else if (sscanf (last_elem, "ttyUSB%d", &portnum) == 1) {
		HalDevice *usbdev;

		hal_device_property_set_int (d, "serial.port", portnum);
		hal_device_property_set_string (d, "serial.type", "usb");

		usbdev = hal_device_store_find (hald_get_gdl (), 
						hal_device_property_get_string (physdev, "info.parent"));
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
	  HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	const gchar *dev_entry;

	if (physdev == NULL)
		return NULL;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));
	hal_device_property_set_string (d, "info.category", "tape");
	hal_device_add_capability (d, "tape");
	hal_device_add_capability (physdev, "tape");

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
mmc_host_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev, const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	gint host_num;
	const gchar *last_elem;

	d = NULL;

	if (physdev == NULL || sysfs_path_in_devices == NULL) {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "linux.sysfs_path_device", sysfs_path_in_devices);

	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (physdev));

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

/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
classdev_remove (HalDevice *d)
{
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

struct ClassDevHandler_s;
typedef struct ClassDevHandler_s ClassDevHandler;

struct ClassDevHandler_s
{
	const gchar *subsystem;
	HalDevice *(*add) (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent, const gchar *sysfs_path_in_devices);
	const gchar *(*get_prober)(HalDevice *d);
	gboolean (*post_probing) (HalDevice *d);
	gboolean (*compute_udi) (HalDevice *d);
	gboolean (*remove) (HalDevice *d);
}; 

/*--------------------------------------------------------------------------------------------------------------*/

static ClassDevHandler classdev_handler_input = 
{ 
	.subsystem    = "input",
	.add          = input_add,
	.get_prober   = input_get_prober,
	.post_probing = input_post_probing,
	.compute_udi  = input_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_bluetooth = 
{ 
	.subsystem    = "bluetooth",
	.add          = bluetooth_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = bluetooth_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_net = 
{ 
	.subsystem    = "net",
	.add          = net_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = net_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_scsi_generic =
{ 
	.subsystem    = "scsi_generic",
	.add          = scsi_generic_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = scsi_generic_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_scsi_host = 
{ 
	.subsystem    = "scsi_host",
	.add          = scsi_host_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = scsi_host_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_usbclass = 
{ 
	.subsystem    = "usb",
	.add          = usbclass_add,
	.get_prober   = usbclass_get_prober,
	.post_probing = NULL,
	.compute_udi  = usbclass_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_usbraw =
{ 
	.subsystem    = "usb_device",
	.add          = usbraw_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = usbraw_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_video4linux =
{ 
	.subsystem    = "video4linux",
	.add          = video4linux_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = video4linux_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_dvb =
{ 
	.subsystem    = "dvb",
	.add          = dvb_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = dvb_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_sound = 
{ 
	.subsystem    = "sound",
	.add          = sound_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = sound_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_serial = 
{ 
	.subsystem    = "tty",
	.add          = serial_add,
	.get_prober   = serial_get_prober,
	.post_probing = NULL,
	.compute_udi  = serial_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_tape =
{
	.subsystem    = "tape",
	.add          = tape_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = tape_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_tape390 =
{
	.subsystem    = "tape390",
	.add          = tape_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = tape_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler classdev_handler_mmc_host =
{
	.subsystem    = "mmc_host",
	.add          = mmc_host_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = mmc_host_compute_udi,
	.remove       = classdev_remove
};

static ClassDevHandler *classdev_handlers[] = {
	&classdev_handler_input,
	&classdev_handler_bluetooth,
	&classdev_handler_net,
	&classdev_handler_scsi_generic,
	&classdev_handler_scsi_host,
	&classdev_handler_usbclass,
	&classdev_handler_usbraw,
	&classdev_handler_video4linux,
	&classdev_handler_dvb,
	&classdev_handler_sound,
	&classdev_handler_serial,
	&classdev_handler_tape,
	&classdev_handler_tape390,
	&classdev_handler_mmc_host,
	NULL
};

/*--------------------------------------------------------------------------------------------------------------*/

static void 
classdev_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;

	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);

	hotplug_event_end (end_token);
}

static void 
classdev_callouts_remove_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
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
add_classdev_after_probing (HalDevice *d, ClassDevHandler *handler, void *end_token)
{
	/* Merge properties from .fdi files */
	di_search_and_merge (d, DEVICE_INFO_TYPE_INFORMATION);
	di_search_and_merge (d, DEVICE_INFO_TYPE_POLICY);

	/* Compute UDI */
	if (!handler->compute_udi (d)) {
		hal_device_store_remove (hald_get_tdl (), d);
		g_object_unref (d);
		hotplug_event_end (end_token);
		goto out;
	}
	
	/* TODO: Merge persistent properties */

	/* Run callouts */
	hal_util_callout_device_add (d, classdev_callouts_add_done, end_token, NULL);

out:
	;
}

static void 
add_classdev_probing_helper_done (HalDevice *d, guint32 exit_type, 
                                  gint return_code, char **error,
                                  gpointer data1, gpointer data2) 
{
	void *end_token = (void *) data1;
	ClassDevHandler *handler = (ClassDevHandler *) data2;

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

	add_classdev_after_probing (d, handler, end_token);

out:
	;
}

static void 
classdev_callouts_preprobing_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	void *end_token = (void *) userdata1;
	ClassDevHandler *handler = (ClassDevHandler *) userdata2;
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
		                    add_classdev_probing_helper_done,
		                    (gpointer) end_token, (gpointer) handler);
	} else {
		add_classdev_after_probing (d, handler, end_token);
	}
out:
  ;
}

void
hotplug_event_begin_add_classdev (const gchar *subsystem, const gchar *sysfs_path, const gchar *device_file, 
				  HalDevice *physdev, const gchar *sysfs_path_in_devices, void *end_token)
{
	guint i;

	HAL_INFO (("class_add: subsys=%s sysfs_path=%s dev=%s physdev=0x%08x", subsystem, sysfs_path, device_file, physdev));

	/* update driver property of the physical device, cause manual driver bind/unbind
	 * may change change this without sending events for the bus device
	 */
	if (physdev != NULL)
		hal_util_set_driver (physdev, "info.linux.driver", sysfs_path_in_devices);

	if (physdev != NULL && hal_device_property_get_bool (physdev, "info.ignore")) {
		HAL_INFO (("Ignoring class_add since physdev has info.ignore==TRUE"));
		hotplug_event_end (end_token);
		goto out;
	}

	for (i = 0; classdev_handlers [i] != NULL; i++) {
		ClassDevHandler *handler;

		handler = classdev_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			HalDevice *d;

			/* attempt to add the device */
			d = handler->add (sysfs_path, device_file, physdev, sysfs_path_in_devices);
			if (d == NULL) {
				/* didn't find anything - thus, ignore this hotplug event */
				hotplug_event_end (end_token);
				goto out;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_SYSFS_CLASS);
			hal_device_property_set_string (d, "linux.subsystem", subsystem);
			
			if (device_file != NULL && strlen (device_file) > 0)
				hal_device_property_set_string (d, "linux.device_file", device_file);

			/* Add to temporary device store */
			hal_device_store_add (hald_get_tdl (), d);

			/* Process preprobe fdi files */
			di_search_and_merge (d, DEVICE_INFO_TYPE_PREPROBE);

			/* Run preprobe callouts */
			hal_util_callout_device_preprobe (d, classdev_callouts_preprobing_done, end_token, handler);
			goto out;
		}
	}

	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out:
	;
}

void
hotplug_event_begin_remove_classdev (const gchar *subsystem, const gchar *sysfs_path, void *end_token)
{
	guint i;
	HalDevice *d;


	HAL_INFO (("class_rem: subsys=%s sysfs_path=%s", subsystem, sysfs_path));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Error removing device"));
	} else {

		for (i = 0; classdev_handlers [i] != NULL; i++) {
			ClassDevHandler *handler;
			
			handler = classdev_handlers[i];
			if (strcmp (handler->subsystem, subsystem) == 0) {
				handler->remove (d);

				hal_util_callout_device_remove (d, classdev_callouts_remove_done, end_token, NULL);
				goto out;
			}
		}
	}

	/* didn't find anything - thus, ignore this hotplug event */
	hotplug_event_end (end_token);
out:
	;
}

gboolean
classdev_rescan_device (HalDevice *d)
{
	return FALSE;
}


HotplugEvent *
classdev_generate_add_hotplug_event (HalDevice *d)
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
classdev_generate_remove_hotplug_event (HalDevice *d)
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
