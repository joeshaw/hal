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

#ifdef HAL_LINUX_INPUT_HEADER_H
  #include HAL_LINUX_INPUT_HEADER_H
#else
  #include <linux/input.h>
#endif

/* for wireless extensions */
#include <linux/if.h>
#include <linux/wireless.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../device_info.h"
#include "../device_pm.h"
#include "../device_store.h"
#include "../hald.h"
#include "../hald_dbus.h"
#include "../hald_runner.h"
#include "../logger.h"
#include "../osspec.h"
#include "../util.h"
#include "../util_pm.h"
#include "../ids.h"

#include "coldplug.h"
#include "hotplug_helper.h"
#include "osspec_linux.h"

#include "device.h"

/* this is kinda messy... but acpi.c + friends use this */
gboolean _have_sysfs_lid_button = FALSE;
gboolean _have_sysfs_power_button = FALSE;
gboolean _have_sysfs_sleep_button = FALSE;
gboolean _have_sysfs_power_supply = FALSE; 
static gboolean battery_poll_running = FALSE;

#define POWER_SUPPLY_BATTERY_POLL_INTERVAL 30  /* in seconds */
#define DOCK_STATION_UNDOCK_POLL_INTERVAL 300  /* in milliseconds */

/* we must use this kernel-compatible implementation */
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

/*--------------------------------------------------------------------------------------------------------------*/
/* 		 	PLEASE KEEP THE SUBSYSTEMS IN ALPHABETICAL ORDER !!!					*/
/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
backlight_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev,
	       const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	int max_brightness;
	const char *id;

	d = hal_device_new ();
	hal_device_add_capability (d, "laptop_panel");
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	hal_device_property_set_string (d, "info.category", "laptop_panel");
	hal_device_property_set_string (d, "info.product", "Generic Backlight Device");
	hal_device_property_set_string (d, "laptop_panel.access_method", "general");

	id = hal_util_get_last_element (sysfs_path);
	if (strstr(id, "acpi_video") != NULL) {
		/* looks like the generic acpi video module */
		const char *param;

		/* Try to check the module parameter to decide if brightness_in_hardware should get set to true.
		   NOTE: this leads to wrong values if someone change the module parameter via 
                         sysfs while HAL is running, but we can live with this situation! */
		param = hal_util_get_string_from_file ("/sys/module/video/parameters/", "brightness_switch_enabled");

		if (param && !strcmp(param, "Y")) {
			hal_device_property_set_bool (d, "laptop_panel.brightness_in_hardware", TRUE);
		} else {
			hal_device_property_set_bool (d, "laptop_panel.brightness_in_hardware", FALSE);
		}
	}	

	hal_util_get_int_from_file (sysfs_path, "max_brightness", &max_brightness, 10);
	hal_device_property_set_int (d, "laptop_panel.num_levels", max_brightness + 1);
	return d;
}

static gboolean
backlight_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "%s_backlight",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
bluetooth_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, 
	       const gchar *parent_path)
{
	HalDevice *d;
	const char *type_entry;
	const char *addr_entry;
        unsigned int a5, a4, a3, a2, a1, a0;
        dbus_uint64_t address;

	d = NULL;

	if (parent_dev == NULL) {
		goto out;
	}

	addr_entry = hal_util_get_string_from_file (sysfs_path, "address");
        if (addr_entry == NULL)
                goto out;

        if (sscanf (addr_entry, "%x:%x:%x:%x:%x:%x", &a5, &a4, &a3, &a2, &a1, &a0) != 6) {
                goto out;
        }
        address = ((dbus_uint64_t)a5<<40) |
                ((dbus_uint64_t)a4<<32) | 
                ((dbus_uint64_t)a3<<24) | 
                ((dbus_uint64_t)a2<<16) | 
                ((dbus_uint64_t)a1<< 8) | 
                ((dbus_uint64_t)a0<< 0);

	type_entry = hal_util_get_string_from_file (sysfs_path, "type");
        if (type_entry == NULL)
                goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	if (strcmp (type_entry, "ACL") == 0) {
		hal_device_property_set_string (d, "info.category", "bluetooth_acl");
		hal_device_add_capability (d, "bluetooth_acl");
                hal_device_property_set_uint64 (d, "bluetooth_acl.address", address);
		hal_device_property_set_string (d, "info.product", "Bluetooth Asynchronous Connection-oriented Link");
		hal_device_property_set_string (d, "bluetooth_acl.originating_device", hal_device_get_udi (parent_dev));

	} else if (strcmp (type_entry, "SCO") == 0) {
		hal_device_property_set_string (d, "info.category", "bluetooth_sco");
		hal_device_add_capability (d, "bluetooth_sco");
                hal_device_property_set_uint64 (d, "bluetooth_sco.address", address);
		hal_device_property_set_string (d, "info.product", "Bluetooth Synchronous Connection-oriented Link");
		hal_device_property_set_string (d, "bluetooth_sco.originating_device", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.category", "bluetooth_hci");
		hal_device_add_capability (d, "bluetooth_hci");
		hal_device_property_set_string (d, "info.product", "Bluetooth Host Controller Interface");
		hal_device_property_set_string (d, "bluetooth_hci.originating_device", hal_device_get_udi (parent_dev));
                hal_device_property_set_uint64 (d, "bluetooth_hci.address", address);
	}

out:
	return d;
}

static gboolean
bluetooth_compute_udi (HalDevice *d)
{
	gchar udi[256];

	if (hal_device_has_capability (d, "bluetooth_acl")) {
		hald_compute_udi (udi, sizeof (udi),
                                  "/org/freedesktop/Hal/devices/bluetooth_acl_%0llx",
                                  hal_device_property_get_uint64 (d, "bluetooth_acl.address"));
	} else if (hal_device_has_capability (d, "bluetooth_sco")) {
		hald_compute_udi (udi, sizeof (udi),
                                  "/org/freedesktop/Hal/devices/bluetooth_acl_%0llx",
                                  hal_device_property_get_uint64 (d, "bluetooth_acl.address"));
	} else {
		hald_compute_udi (udi, sizeof (udi),
                                  "%s_bluetooth_hci_%0llx",
                                  hal_device_property_get_string (d, "info.parent"),
                                  hal_device_property_get_uint64 (d, "bluetooth_hci.address"));
	}
	hal_device_set_udi (d, udi);
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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/ccw_%s",
			  hal_device_property_get_string
			  (d, "ccw.bus_id"));
	hal_device_set_udi (d, udi);
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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/ccwgroup_%s",
			  hal_device_property_get_string
			  (d, "ccwgroup.bus_id"));
	hal_device_set_udi (d, udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
drm_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d = NULL;

	d = hal_device_new ();

	hal_device_add_capability (d, "drm");

	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
		hal_device_copy_property( parent_dev, "info.vendor", d, "info.vendor");
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_device_property_set_string (d, "info.product", "Direct Rendering Manager Device");
	hal_device_property_set_string (d, "info.category", "drm");
	hal_device_property_set_string (d, "linux.device_file", device_file);
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path); /* not sure if this is needed/set */
	hal_util_set_string_from_file (d, "drm.dri_library", sysfs_path, "dri_library_name");
	hal_util_set_string_from_file (d, "drm.version", sysfs_path, "../version");
	
	return d;
}

static gboolean
drm_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const char *dir;
	const char *name;

	dir = hal_device_property_get_string (d, "linux.sysfs_path");

	name = hal_util_get_last_element(dir);

	/* generate e.g.: /org/freedesktop/Hal/devices/pci_8086_2a02_drm_i915_card0 */
	hald_compute_udi (udi, sizeof (udi),
			  "%s_drm_%s_%s",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_string (d, "drm.dri_library"),
			  name);

	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi), "%s_dvb",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
firewire_add_device (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev)
{
	HalDevice *d = NULL;
	gchar buf[64];

	if (device_file == NULL || device_file[0] == '\0')
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "ieee1394");
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_add_capability (d, "ieee1394");
	hal_device_property_set_string (d, "ieee1394.device", device_file);
	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_uint64_from_file (d, "ieee1394.guid", sysfs_path, "guid", 16);	
	hal_util_set_int_from_file  (d, "ieee1394.vendor_id", sysfs_path, "vendor", 16);
	hal_util_set_int_from_file  (d, "ieee1394.product_id", sysfs_path, "model", 16);
	hal_util_set_int_from_file  (d, "ieee1394.harware_version", sysfs_path, "harware_version", 16);

	if (!hal_util_set_string_from_file (d, "ieee1394.vendor", sysfs_path, "vendor_name")) {
		/* FIXME: We should do a OUI lookup here, see
		 * http://standards.ieee.org/regauth/oui/oui.txt */

		g_snprintf (buf, sizeof (buf), "Unknown (0x%06x)", 
			    hal_device_property_get_int (d, "ieee1394.vendor_id"));
		hal_device_property_set_string (d, "ieee1394.vendor", buf);
	}

	hal_util_set_string_from_file (d, "ieee1394.product", sysfs_path, "model_name");

 out:
	return d;
}

static HalDevice *
firewire_add_unit (const gchar *sysfs_path, int unit_id, HalDevice *parent_dev)
{
	int specifier_id;
	int version;
	HalDevice *d;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.subsystem", "ieee1394_unit");
	hal_device_property_set_string (d, "info.parent",
					hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "ieee1394_unit.originating_device", 
					hal_device_get_udi (parent_dev));
	hal_device_property_set_int (d, "ieee1394_unit.unit_index", unit_id);
	hal_device_add_capability (d, "ieee1394_unit");
	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_int_from_file  (d, "ieee1394.vendor_id", sysfs_path, "../vendor_id", 16);
	hal_util_get_int_from_file  (sysfs_path, "specifier_id", &specifier_id, 16);
	hal_device_property_set_int (d, "ieee1394_unit.specifier_id", specifier_id);
	hal_util_get_int_from_file  (sysfs_path, "version", &version, 16);
	hal_device_property_set_int (d, "ieee1394_unit.version", version);

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

	return d;
}

static HalDevice *
firewire_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	int device_id;
	int unit_id;
	const char *bus_id;

	if (parent_dev == NULL)
		return NULL;

	bus_id = hal_util_get_last_element (sysfs_path);

	if (sscanf (bus_id, "fw%d.%d", &device_id, &unit_id) == 2 ) {
		return firewire_add_unit (sysfs_path, unit_id, parent_dev);
	} else if (sscanf (bus_id, "fw%d", &device_id) == 1) {
		return firewire_add_device (sysfs_path, device_file, parent_dev);
	} else {
		return NULL;
	}
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
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/ieee1394_guid%0llx",
				  hal_device_property_get_uint64 (d, "ieee1394.guid"));
	} else {
		hald_compute_udi (udi, sizeof (udi),
				  "%s_unit%d",
				  hal_device_property_get_string (d, "ieee1394_unit.originating_device"),
				  hal_device_property_get_int (d, "ieee1394_unit.unit_index"));
	}

	hal_device_set_udi (d, udi);
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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_ide_%d_%d",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_int (d, "ide.host"),
			  hal_device_property_get_int (d, "ide.channel"));
	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/ieee1394_guid_%0llx",
			  hal_device_property_get_uint64 (d, "ieee1394.guid"));
	hal_device_set_udi (d, udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static int
input_str_to_bitmask (const char *s, long *bitmask, size_t max_size)
{
	int i, j;
	gchar **v;
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
	g_strfreev(v);

	return num_bits_set;
}

static void
input_test_rel (HalDevice *d, const char *sysfs_path)
{
	char *s;
	long bitmask[NBITS(REL_MAX)];
	int num_bits;

	s = hal_util_get_string_from_file (sysfs_path, "capabilities/rel");
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

static gboolean
input_test_keyboard (long *bitmask)
{
	int i;

	for (i = KEY_Q; i <= KEY_P; i++) {
		if (!test_bit (i, bitmask))
			return FALSE;
	}

	return TRUE;
}

static gboolean
input_test_keypad (long *bitmask)
{
	int i;

	for (i = KEY_KP7; i <= KEY_KPDOT; i++) {
		if (!test_bit (i, bitmask))
			return FALSE;
	}

	return TRUE;
}

static gboolean
input_test_keys (long *bitmask)
{
	int i;

	/* All keys that are not buttons are less than BTN_MISC */
	for (i = KEY_RESERVED + 1; i < BTN_MISC; i++) {
		if (test_bit (i, bitmask))
			return TRUE;
	}

	return FALSE;
}

static void
input_test_key (HalDevice *d, const char *sysfs_path)
{
	char *s;
	long bitmask[NBITS(KEY_MAX)];
	int num_bits;

	s = hal_util_get_string_from_file (sysfs_path, "capabilities/key");
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
		gboolean is_keyboard = input_test_keyboard (bitmask);
		gboolean is_keypad = input_test_keypad (bitmask);

		if (is_keyboard)
			hal_device_add_capability (d, "input.keyboard");
		if (is_keypad)
			hal_device_add_capability (d, "input.keypad");

		if (is_keyboard || is_keypad || input_test_keys (bitmask))
			hal_device_add_capability (d, "input.keys");
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

	s = hal_util_get_string_from_file (sysfs_path, "capabilities/sw");
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
#ifdef SW_RADIO
		} else if (test_bit (SW_RADIO, bitmask)) {
			hal_device_property_set_string (d, "button.type", "radio");
#endif
		}
	}

out:
	;
}

static void
input_test_abs (HalDevice *d, const char *sysfs_path)
{
	char *s;
	long bitmask_abs[NBITS(ABS_MAX)];
	long bitmask_key[NBITS(KEY_MAX)];
	int num_bits_abs;
	int num_bits_key;

	s = hal_util_get_string_from_file (sysfs_path, "capabilities/abs");
	if (s == NULL)
		goto out;
	num_bits_abs = input_str_to_bitmask (s, bitmask_abs, sizeof (bitmask_abs));

	if (test_bit (ABS_X, bitmask_abs) && test_bit (ABS_Y, bitmask_abs)) {

		s = hal_util_get_string_from_file (sysfs_path, "capabilities/key");
		if (s != NULL)
		{
			num_bits_key = input_str_to_bitmask (s, bitmask_key, sizeof (bitmask_key));

			if (test_bit (BTN_STYLUS, bitmask_key)) {
				hal_device_add_capability (d, "input.tablet");
				goto out;
			}

			if (test_bit (BTN_TOUCH, bitmask_key)) {
				hal_device_add_capability (d, "input.touchpad");
				goto out;
			}

			if (test_bit (BTN_TRIGGER, bitmask_key) || test_bit (BTN_A, bitmask_key) || test_bit (BTN_1, bitmask_key)) {
				hal_device_add_capability (d, "input.joystick");
				goto out;
			}

			if (test_bit (BTN_MOUSE, bitmask_key)) {
				/*
				 * This path is taken by VMware's USB mouse, which has
				 * absolute axes, but no touch/pressure button.
				 */
				hal_device_add_capability (d, "input.mouse");
				goto out;
			}
		}

		if (test_bit (ABS_PRESSURE, bitmask_abs)) {
			hal_device_add_capability (d, "input.touchpad");
			goto out;
		}
	}
out:
	;
}

static HalDevice *
input_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	int eventdev_num;
	HalDevice *d;
        char *attr_sysfs_path;
        
        d = NULL;
        attr_sysfs_path = NULL;

	if (device_file == NULL || device_file[0] == '\0')
		goto out;

	/* only care about evdev input devices */
	if (sscanf (hal_util_get_last_element (sysfs_path), "event%d", &eventdev_num) != 1)
		goto out;
        
        /* Prior to 2.6.23pre event%d was a child of input%d - after that event%d
         * moved to the same level with a device/ symlink... Handle both cases
         */
        attr_sysfs_path = g_strdup_printf ("%s/../capabilities", sysfs_path);
        if (g_file_test (attr_sysfs_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
                g_free (attr_sysfs_path);
                attr_sysfs_path = g_strdup_printf ("%s/../", sysfs_path);
        } else {
                g_free (attr_sysfs_path);
                attr_sysfs_path = g_strdup_printf ("%s/device/capabilities", sysfs_path);
                if (g_file_test (attr_sysfs_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
                        g_free (attr_sysfs_path);
                        attr_sysfs_path = g_strdup_printf ("%s/device/", sysfs_path);
                } else {
                        goto out;
                }
        }

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "input.originating_device", hal_device_get_udi (parent_dev));
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}
	hal_device_property_set_string (d, "info.category", "input");
	hal_device_add_capability (d, "input");

	hal_device_property_set_string (d, "input.device", device_file);

	hal_util_set_string_from_file (d, "info.product", attr_sysfs_path, "name");
	hal_util_set_string_from_file (d, "input.product", attr_sysfs_path, "name");

	/* check for keys */
	input_test_key (d, attr_sysfs_path);

	/* check for mice etc. */
	input_test_rel (d, attr_sysfs_path);

	/* check for joysticks etc. */
	input_test_abs (d, attr_sysfs_path);

	/* check for switches */
        input_test_switch (d, attr_sysfs_path);

out:
        g_free (attr_sysfs_path);
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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_logicaldev_input",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/iucv_%s",
			  hal_device_property_get_string
			  (d, "iucv.bus_id"));
	hal_device_set_udi (d, udi);
	return TRUE;

}
/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
leds_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *dev_name;
        gchar **attributes;

	d = hal_device_new ();

	if (parent_dev != NULL)
                hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
        else
                hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");

	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_device_property_set_string (d, "info.category", "leds");
	hal_device_add_capability (d, "leds");

	dev_name = hal_util_get_last_element (sysfs_path);
	if (dev_name) {
	        attributes = g_strsplit_set (dev_name, ":", 0);
	
		if (attributes != NULL) {
			if (attributes[0] != NULL) {
				if (attributes[0][0] != '\0')
					hal_device_property_set_string (d, "leds.device_name", attributes[0]);
				if (attributes[1] != NULL ) {
					if (attributes[1][0] != '\0')
						hal_device_property_set_string (d, "leds.colour", attributes[1]);
					if (attributes[2] != NULL && attributes[2][0] != '\0')
						hal_device_property_set_string (d, "leds.function", attributes[2]);
				}
			}
		}
		g_strfreev (attributes);
	}
	
	return d;
}

static gboolean
leds_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const char *name;
	const char *colour;
	const char *function;

        name = hal_device_property_get_string (d, "leds.device_name");
        colour = hal_device_property_get_string (d, "leds.colour");
        function = hal_device_property_get_string (d, "leds.function");

	if (name && function && colour) {
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/leds_%s_%s_%s", name, function, colour);
	} else if (name && function) {
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/leds_%s_%s", name, function);
	} else if (name) {
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/leds_%s", name);
	} else {
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/leds_unknown");
	}
	
	hal_device_set_udi (d, udi);
	return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
memstick_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	if (parent_dev == NULL) {
		d = NULL;
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_string_from_file (d, "info.product", sysfs_path, "attr_modelname");
	
out:
	return d;
}

static gboolean
memstick_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_memstick_card",
			      hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	hal_device_property_set_string (d, "info.udi", udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
memstick_host_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
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

	hal_device_property_set_string (d, "info.category", "memstick_host");
	hal_device_add_capability (d, "memstick_host");

	hal_device_property_set_string (d, "info.product", "Memory Stick Host Adapter");

	last_elem = hal_util_get_last_element (sysfs_path);
	sscanf (last_elem, "memstick%d", &host_num);
	hal_device_property_set_int (d, "memstick_host.host", host_num);

out:
	return d;
}

static gboolean
memstick_host_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hal_util_compute_udi (hald_get_gdl (), udi, sizeof (udi),
			      "%s_memstick_host",
			      hal_device_property_get_string (d, "info.parent"));
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
	gchar *scr, *type;

	if (parent_dev == NULL) {
		d = NULL;
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);
	sscanf (bus_id, "mmc%d:%x", &host_num, &rca);
	hal_device_property_set_int (d, "mmc.rca", rca);
	
	hal_util_set_string_from_file (d, "mmc.cid", sysfs_path, "cid");
	hal_util_set_string_from_file (d, "mmc.csd", sysfs_path, "csd");
	
	type = hal_util_get_string_from_file (sysfs_path, "type");
	if (type != NULL)
		/* Possible MMC/SD/SDIO */
		hal_device_property_set_string (d, "mmc.type", type);

	scr = hal_util_get_string_from_file (sysfs_path, "scr");
	if (scr != NULL) {
		if (strcmp (scr, "0000000000000000") == 0)
			scr = NULL;
		else
			hal_device_property_set_string (d, "mmc.scr", scr);
	}

	if (!hal_util_set_string_from_file (d, "info.product", sysfs_path, "name")) {
		gchar buf[64];
		if (type != NULL) {
			g_snprintf(buf, sizeof(buf), "%s Card", type);
			hal_device_property_set_string (d, "info.product", buf);
		} else if (scr != NULL) {
			g_snprintf(buf, sizeof(buf), "SD Card");
			hal_device_property_set_string (d, "info.product", buf);
		} else {
			g_snprintf(buf, sizeof(buf), "MMC Card");
                }
		hal_device_property_set_string (d, "mmc.product", buf);
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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_mmc_card_rca%d",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_int (d, "mmc.rca"));
	hal_device_set_udi (d, udi);
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

	hal_util_set_string_from_file (d, "mmc_host.slot_name", sysfs_path, "slot_name");

out:
	return d;
}

static gboolean
mmc_host_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "%s_mmc_host",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
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
	gint addr_len;

	d = NULL;
	d = hal_device_new ();

	if (parent_dev == NULL) {
	        parent_dev = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer");
		if (parent_dev == NULL) {
                	parent_dev = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer");
			if (parent_dev == NULL) {
				HAL_ERROR (("Device '%s' has no parent and couldn't find computer root object."));
				goto error;
			}
		}
        }

	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "net.originating_device", hal_device_get_udi (parent_dev));

	hal_device_property_set_string (d, "info.category", "net");
	hal_device_add_capability (d, "net");

	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	ifname = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "net.interface", ifname);

	hal_util_get_int_from_file(sysfs_path, "addr_len", &addr_len, 0);

	if (!addr_len || !hal_util_set_string_from_file (d, "net.address", sysfs_path, "address")) {
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
		const char *parent_subsys;
		char bridge_path[HAL_PATH_MAX];
		char phy80211_path[HAL_PATH_MAX];
		struct stat s;
		dbus_uint64_t mac_address = 0;
		int ioctl_fd;
		struct iwreq iwr;

		ioctl_fd = socket (PF_INET, SOCK_DGRAM, 0);
		strncpy (iwr.ifr_ifrn.ifrn_name, ifname, IFNAMSIZ);

		addr = hal_device_property_get_string (d, "net.address");
		if (addr != NULL) {
			unsigned int a5, a4, a3, a2, a1, a0;

			if (sscanf (addr, "%x:%x:%x:%x:%x:%x",
				    &a5, &a4, &a3, &a2, &a1, &a0) == 6) {
				mac_address =
					((dbus_uint64_t)a5<<40) |
					((dbus_uint64_t)a4<<32) |
					((dbus_uint64_t)a3<<24) |
					((dbus_uint64_t)a2<<16) |
					((dbus_uint64_t)a1<< 8) |
					((dbus_uint64_t)a0<< 0);
			}
		}

		snprintf (bridge_path, HAL_PATH_MAX, "%s/bridge", sysfs_path);
		/* cfg80211 */
		snprintf (phy80211_path, HAL_PATH_MAX, "%s/phy80211", sysfs_path);
		parent_subsys = hal_device_property_get_string (parent_dev, "info.subsystem");

		if (parent_subsys && strcmp(parent_subsys, "bluetooth") == 0) {
			hal_device_property_set_string (d, "info.product", "Bluetooth Interface");
			hal_device_property_set_string (d, "info.category", "net.bluetooth");
			hal_device_add_capability (d, "net.bluetooth");
			hal_device_property_set_uint64 (d, "net.bluetooth.mac_address", mac_address);
		} else if ((ioctl (ioctl_fd, SIOCGIWNAME, &iwr) == 0) ||
			(stat (phy80211_path, &s) == 0 && (s.st_mode & S_IFDIR))) {
			hal_device_property_set_string (d, "info.product", "WLAN Interface");
			hal_device_property_set_string (d, "info.category", "net.80211");
			hal_device_add_capability (d, "net.80211");
			hal_device_property_set_uint64 (d, "net.80211.mac_address", mac_address);
		} else if (stat (bridge_path, &s) == 0 && (s.st_mode & S_IFDIR)) {
			hal_device_property_set_string (d, "info.product", "Bridge Interface");
			hal_device_property_set_string (d, "info.category", "net.bridge");
			hal_device_add_capability (d, "net.bridge");
			hal_device_property_set_uint64 (d, "net.bridge.mac_address", mac_address);
		} else {
			hal_device_property_set_string (d, "info.product", "Networking Interface");
			hal_device_property_set_string (d, "info.category", "net.80203");
			hal_device_add_capability (d, "net.80203");
			hal_device_property_set_uint64 (d, "net.80203.mac_address", mac_address);
		}

		close (ioctl_fd);
	} else if (media_type == ARPHRD_IRDA) {
		hal_device_property_set_string (d, "info.product", "Networking Interface");
		hal_device_property_set_string (d, "info.category", "net.irda");
		hal_device_add_capability (d, "net.irda");
	} else if (media_type == ARPHRD_LOOPBACK) {
		hal_device_property_set_string (d, "info.product", "Loopback device Interface");
		hal_device_property_set_string (d, "info.category", "net.loopback");
		hal_device_add_capability (d, "net.loopback");
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
net_refresh (HalDevice *d)
{
	const gchar *path, *ifname;
	
	path = hal_device_property_get_string (d, "linux.sysfs_path");
	ifname = hal_util_get_last_element (path);
	hal_device_property_set_string (d, "net.interface", ifname);
	return TRUE;
}

static const char *
net_get_prober (HalDevice *d)
{
	const char *prober = NULL;

	/* run prober only for bluetooth devices */
	if (hal_device_has_capability (d, "net.bluetooth")) {
		prober = "hald-probe-net-bluetooth";
	}

	return prober;
}

static gboolean
net_post_probing (HalDevice *d)
{
	return TRUE;
}

static gboolean
net_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const gchar *id;
	gboolean id_only = TRUE;

	id = hal_device_property_get_string (d, "net.address");

	if (id == NULL || (strcmp (id, "00:00:00:00:00:00") == 0)) {
		/* Need to fall back to something else if mac not available. */
		id = hal_util_get_last_element(hal_device_property_get_string(d, "net.originating_device"));
		if (!strcmp(id, "computer")) {
			const gchar *cat;
			char type[32];

			/* virtual devices or devices without a parent for some reason */
			if ((cat = hal_device_property_get_string(d, "info.category")) &&
			    (sscanf (cat, "net.%s", type) == 1)) {
				hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/net_%s_%s", id, type);
				id_only = FALSE;
			} 
		} 
	} 

	if (id_only)
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/net_%s", id);

	hal_device_set_udi (d, udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
of_platform_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev,
		 const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);

	hal_device_property_set_string (d, "of_platform.id", dev_id);

	g_snprintf (buf, sizeof (buf), "OpenFirmware Platform Device (%s)", hal_device_property_get_string (d, "of_platform.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
of_platform_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/of_platform_%s",
			 hal_device_property_get_string (d, "of_platform.id"));
	hal_device_set_udi (d, udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
pci_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	gint device_class;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_device_property_set_string (d, "pci.linux.sysfs_path", sysfs_path);

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	if(!hal_util_set_int_from_file (d, "pci.product_id", sysfs_path, "device", 16) ||
	   !hal_util_set_int_from_file (d, "pci.vendor_id", sysfs_path, "vendor", 16)) {
		HAL_ERROR(("Could not get PCI product or vendor ID, don't add device, this info is mandatory!"));
		return NULL;
	}

	hal_util_set_int_from_file (d, "pci.subsys_product_id", sysfs_path, "subsystem_device", 16);
	hal_util_set_int_from_file (d, "pci.subsys_vendor_id", sysfs_path, "subsystem_vendor", 16);

	if (hal_util_get_int_from_file (sysfs_path, "class", &device_class, 16)) {
		hal_device_property_set_int (d, "pci.device_class", ((device_class >> 16) & 0xff));
		hal_device_property_set_int (d, "pci.device_subclass", ((device_class >> 8) & 0xff));
		hal_device_property_set_int (d, "pci.device_protocol", (device_class & 0xff));
	}

	{
		gchar buf[64];
		char *vendor_name = NULL;
		char *product_name = NULL;
		char *subsys_vendor_name = NULL;
		char *subsys_product_name = NULL;

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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pci_%x_%x",
			  hal_device_property_get_int (d, "pci.vendor_id"),
			  hal_device_property_get_int (d, "pci.product_id"));
	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pcmcia_%d_%d",
			  hal_device_property_get_int (d, "pcmcia.manfid1"),
			  hal_device_property_get_int (d, "pcmcia.manfid2"));
	hal_device_set_udi (d, udi);
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

	if (strncmp (dev_id, "dock", 4) == 0) {
		int docked;

		hal_util_get_int_from_file (sysfs_path, "docked", &docked, 0);
		hal_device_property_set_bool (d, "info.docked", docked);
	}

	return d;
}

static gboolean
platform_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/platform_%s",
			  hal_device_property_get_string (d, "platform.id"));
	hal_device_set_udi (d, udi);

	return TRUE;
}

static gboolean
platform_refresh_undock (gpointer data)
{
	HalDevice *d;
	gint flags, docked;
	const gchar *sysfs_path;

	if (data == NULL)
		return FALSE;
	d = (HalDevice *) data;

	sysfs_path = hal_device_property_get_string(d, "linux.sysfs_path");
	hal_util_get_int_from_file (sysfs_path, "flags", &flags, 0);

	/* check for != 0, maybe the user did an immediate dock */
	if (flags != 0)
		return TRUE;

	hal_util_get_int_from_file (sysfs_path, "docked", &docked, 0);
	hal_device_property_set_bool (d, "info.docked", docked);

	return FALSE;
}

static gboolean
platform_refresh (HalDevice *d)
{
	const gchar *id, *sysfs_path;
	gint docked, flags;

	id = hal_device_property_get_string (d, "platform.id");
	if (strncmp (id, "dock", 4) != 0)
		return TRUE;

	sysfs_path = hal_device_property_get_string(d, "linux.sysfs_path");
	hal_util_get_int_from_file (sysfs_path, "docked", &docked, 0);

	if (docked == 1) {
		/* undock still in progress? */
		hal_util_get_int_from_file (sysfs_path, "flags", &flags, 0);
		if (flags == 2) {
			g_timeout_add (DOCK_STATION_UNDOCK_POLL_INTERVAL,
				       platform_refresh_undock, d);
			return TRUE;
		}
	}

	hal_device_property_set_bool (d, "info.docked", docked);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
pnp_set_serial_info (const gchar *sysfs_path, HalDevice *d) {

	hal_util_set_int_elem_from_file (d, "pnp.serial.irq", sysfs_path, "resources", "irq", 0, 10, TRUE);

	if (hal_util_set_string_elem_from_file (d, "pnp.serial.port", sysfs_path, "resources", "io", 0, TRUE)) {
		const char* port;
		const char* _port;
		_port = hal_device_property_get_string (d, "pnp.serial.port");
		if(_port == NULL)
			return;

		port = strtok((char*) _port, "-");
		if(port == NULL)
			return;

		hal_device_property_set_string (d, "pnp.serial.port", port);
	}
}


static HalDevice *
pnp_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	HalDevice *computer;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	hal_util_set_string_from_file (d, "pnp.id", sysfs_path, "id");
	if (hal_device_has_property (d, "pnp.id")) {
		gchar *pnp_description;
		const char *pnp_id;
		ids_find_pnp (hal_device_property_get_string (d, "pnp.id"), &pnp_description);
		if (pnp_description != NULL) {
			hal_device_property_set_string (d, "pnp.description", pnp_description);
			hal_device_property_set_string (d, "info.product", pnp_description);
		}
		pnp_id = hal_device_property_get_string (d, "pnp.id");
		if( !strncmp(pnp_id, "WACf00", 6) || !strcmp(pnp_id, "FUJ02e5") ||
		    !strcmp(pnp_id, "FUJ02e6") || !strcmp(pnp_id, "FPI2004")) {
			/* a internal serial tablet --> this should be a tablet pc */
			if ((computer = hal_device_store_find (hald_get_gdl (), "/org/freedesktop/Hal/devices/computer")) != NULL ||
			    (computer = hal_device_store_find (hald_get_tdl (), "/org/freedesktop/Hal/devices/computer")) != NULL) {

				hal_device_property_set_string (computer, "system.formfactor", "laptop");
				hal_device_property_set_string (computer, "system.formfactor.subtype", "tabletpc");
				/* collect info about serial port and irq etc. */
				pnp_set_serial_info (sysfs_path, d);
			}
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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pnp_%s",
			  hal_device_property_get_string (d, "pnp.id"));
	hal_device_set_udi (d, udi);

	return TRUE;

}


/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
ppdev_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;

	d = hal_device_new ();

	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_util_set_driver (d, "info.linux.driver", sysfs_path);
	hal_device_add_capability (d, "ppdev");
	hal_device_property_set_string (d, "info.category", "ppdev");
	hal_device_property_set_string (d, "info.product", "Parallel Port Device");
	
	return d;
}

static gboolean
ppdev_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const char *name;

	name = hal_util_get_last_element( hal_device_property_get_string(d, "linux.device_file"));

	if (name) {
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/ppdev_%s", name);
	} else {
		hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/ppdev");
	}

	hal_device_set_udi (d, udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
refresh_ac_adapter (HalDevice *d)
{
	const char *path;
	gboolean present = FALSE;

	path = hal_device_property_get_string (d, "linux.sysfs_path");
	if (path == NULL)
		return;

	if (hal_util_get_bool_from_file (path, "online", &present, "1") == FALSE) {
		/* if we can't read the property, assume on ac */
		present = TRUE;
	}

	hal_device_property_set_bool (d, "ac_adapter.present", present);
}

static void
refresh_battery_fast (HalDevice *d)
{
	gint percentage = 0;
	gint voltage_now = 0;
	gint current = 0;
	gint time = 0;
	gint value_now = 0;
	gint value_last_full = 0;
	gboolean present = FALSE;
	gboolean unknown_unit = TRUE;
	gboolean is_mah = FALSE;
	gboolean is_mwh = FALSE;
	gboolean is_charging = FALSE;
	gboolean is_discharging = FALSE;
	gboolean got_time = FALSE;
	gboolean got_percentage = FALSE;
	const gchar *path;
	const gchar *reporting_unit;
	gchar *status;

	path = hal_device_property_get_string (d, "linux.sysfs_path");
	if (path == NULL)
		return;

	/* PRESENT */
	if (hal_util_get_bool_from_file (path, "present", &present, "1")) {
		hal_device_property_set_bool (d, "battery.present", present);
	}
	if (present == FALSE) {
		/* remove all the optional keys associated with the cell */
		device_pm_remove_optional_props (d);
		return;
	}

	/* CAPACITY */
	if (hal_util_get_int_from_file (path, "capacity", &percentage, 10)) {
		/* sanity check */
		if (percentage >= 0 && percentage <= 100)
			got_percentage = TRUE;
	}

	/* VOLTAGE: we prefer the average if it exists, although present is still pretty good */
	if (hal_util_get_int_from_file (path, "voltage_avg", &voltage_now, 10)) {
		hal_device_property_set_int (d, "battery.voltage.current", voltage_now / 1000);
	} else if (hal_util_get_int_from_file (path, "voltage_now", &voltage_now, 10)) {
		hal_device_property_set_int (d, "battery.voltage.current", voltage_now / 1000);
	}

	/* CURRENT: we prefer the average if it exists, although present is still pretty good */
	if (hal_util_get_int_from_file (path, "current_avg", &current, 10)) {
		hal_device_property_set_int (d, "battery.reporting.rate", current / 1000);
	} else if (hal_util_get_int_from_file (path, "current_now", &current, 10)) {
		hal_device_property_set_int (d, "battery.reporting.rate", current / 1000);
	}

	/* STATUS: Convert to charging/discharging state */
	status = hal_util_get_string_from_file (path, "status");
	if (status != NULL) {
		if (strcasecmp (status, "charging") == 0) {
			is_charging = TRUE;
		} else if (strcasecmp (status, "discharging") == 0) {
			is_discharging = TRUE;
		}
		hal_device_property_set_bool (d, "battery.is_rechargeable", TRUE);
		hal_device_property_set_bool (d, "battery.rechargeable.is_charging", is_charging);
		hal_device_property_set_bool (d, "battery.rechargeable.is_discharging", is_discharging);
	}

	/* TIME: Some batteries only provide time to discharge */
	if (is_charging == TRUE) {
		if (hal_util_get_int_from_file (path, "time_to_full_avg", &time, 10) ||
		    hal_util_get_int_from_file (path, "time_to_full_now", &time, 10)) {
			got_time = TRUE;
		}
	} else if (is_discharging == TRUE) {
		if (hal_util_get_int_from_file (path, "time_to_empty_avg", &time, 10) ||
		    hal_util_get_int_from_file (path, "time_to_empty_now", &time, 10)) {
			got_time = TRUE;
		}
	}

	/* Have we already got information about the reporting unit?
	 * If we have, we can save a lots of file reads */
	reporting_unit = hal_device_property_get_string (d, "battery.reporting.unit");
	if (reporting_unit != NULL) {
		if (strcasecmp (reporting_unit, "mah") == 0) {
			is_mah = TRUE;
			unknown_unit = FALSE;
		} else if (strcasecmp (reporting_unit, "mwh") == 0) {
			is_mwh = TRUE;
			unknown_unit = FALSE;
		}
	}

	/* ENERGY (reported in uWh, so need to convert to mWh) */
	if (unknown_unit || is_mwh) {
		if (hal_util_get_int_from_file (path, "energy_avg", &value_now, 10)) {
			hal_device_property_set_int (d, "battery.reporting.current", value_now / 1000);
			is_mwh = TRUE;
		} else if (hal_util_get_int_from_file (path, "energy_now", &value_now, 10)) {
			hal_device_property_set_int (d, "battery.reporting.current", value_now / 1000);
			is_mwh = TRUE;
		}
		if (hal_util_get_int_from_file (path, "energy_full", &value_last_full, 10)) {
			hal_device_property_set_int (d, "battery.reporting.last_full", value_last_full / 1000);
			is_mwh = TRUE;
		}
	}

	/* CHARGE (reported in uAh, so need to convert to mAh) */
	if ((unknown_unit && !is_mwh) || is_mah) {
		if (hal_util_get_int_from_file (path, "charge_avg", &value_now, 10)) {
			hal_device_property_set_int (d, "battery.reporting.current", value_now / 1000);
			is_mah = TRUE;
		} else if (hal_util_get_int_from_file (path, "charge_now", &value_now, 10)) {
			hal_device_property_set_int (d, "battery.reporting.current", value_now / 1000);
			is_mah = TRUE;
		}
		if (hal_util_get_int_from_file (path, "charge_full", &value_last_full, 10)) {
			hal_device_property_set_int (d, "battery.reporting.last_full", value_last_full / 1000);
			is_mah = TRUE;
		}
	}

	/* record these for future savings */
	if (unknown_unit) {
		if (is_mwh == TRUE) {
			hal_device_property_set_string (d, "battery.reporting.unit", "mWh");
		} else if (is_mah == TRUE) {
			hal_device_property_set_string (d, "battery.reporting.unit", "mAh");
		}
	}

	/* we've now got the 'reporting' keys, now we need to populate the
	 * processed 'charge_level' keys so stuff like desktop power managers
	 * do not have to deal with odd quirks */
	device_pm_abstract_props (d);

	/* if we have not read from the hardware, then calculate */
	if (got_percentage) {
		hal_device_property_set_int (d, "battery.charge_level.percentage", percentage);
	} else {
		device_pm_calculate_percentage (d);
	}

	/* if we havn't got time from the hardware, then try to calculate it */
	if (got_time) {
		/* zero time isn't displayed */
		if (time > 0)
			hal_device_property_set_int (d, "battery.remaining_time", time);
		else
			hal_device_property_remove (d, "battery.remaining_time");
	} else {
		device_pm_calculate_time (d);
	}
}

static void
refresh_battery_slow (HalDevice *d)
{
	gint voltage_design = 0;
	gint value_full_design = 0;
	char *technology_raw;
	char *model_name;
	char *manufacturer;
	char *serial;
	const gchar *path;

	path = hal_device_property_get_string (d, "linux.sysfs_path");
	if (path == NULL)
		return;

	/* get battery technology */
	technology_raw = hal_util_get_string_from_file (path, "technology");
	if (technology_raw != NULL) {
		hal_device_property_set_string (d, "battery.reporting.technology", technology_raw);
	}
	hal_device_property_set_string (d, "battery.technology", util_get_battery_technology (technology_raw));

	/* get product name */
	model_name = hal_util_get_string_from_file (path, "model_name");
	if (model_name != NULL) {
		hal_device_property_set_string (d, "battery.model", model_name);
		hal_device_property_set_string (d, "info.product", model_name);
	} else {
		hal_device_property_set_string (d, "info.product", "Generic Battery Device");
	}

	/* get manufacturer */
	manufacturer = hal_util_get_string_from_file (path, "manufacturer");
	if (manufacturer != NULL) {
		hal_device_property_set_string (d, "battery.vendor", manufacturer);
	}

	/* get stuff that never changes */
	if (hal_util_get_int_from_file (path, "voltage_max_design", &voltage_design, 10)) {
		hal_device_property_set_int (d, "battery.voltage.design", voltage_design / 1000);
		hal_device_property_set_string (d, "battery.voltage.unit", "mV");
	} else if (hal_util_get_int_from_file (path, "voltage_min_design", &voltage_design, 10)) {
		hal_device_property_set_int (d, "battery.voltage.design", voltage_design / 1000);
		hal_device_property_set_string (d, "battery.voltage.unit", "mV");
	}

	/* try to get the design info and set the units */
	if (hal_util_get_int_from_file (path, "energy_full_design", &value_full_design, 10)) {
		hal_device_property_set_int (d, "battery.reporting.design", value_full_design / 1000);
		hal_device_property_set_string (d, "battery.reporting.unit", "mWh");	
	} else if (hal_util_get_int_from_file (path, "charge_full_design", &value_full_design, 10)) {
		hal_device_property_set_int (d, "battery.reporting.design", value_full_design / 1000);
		hal_device_property_set_string (d, "battery.reporting.unit", "mAh");
	}

	/* get serial */
	serial = hal_util_get_string_from_file (path, "serial_number");
	if (serial != NULL) {
		hal_device_property_set_string (d, "battery.serial", serial);
	}

	/* now do stuff that happens quickly */
	refresh_battery_fast (d);
}

static gboolean
power_supply_refresh (HalDevice *d)
{
	const gchar *type;
	type = hal_device_property_get_string (d, "info.category");
	if (type == NULL) {
		return FALSE;
	}
	if (strcmp (type, "ac_adapter") == 0) {
		device_property_atomic_update_begin ();
		refresh_ac_adapter (d);
		device_property_atomic_update_end ();
	} else if (strcmp (type, "battery") == 0) {
		device_property_atomic_update_begin ();
		refresh_battery_fast (d);
		device_property_atomic_update_end ();
	} else {
		HAL_WARNING (("Could not recognise power_supply type!"));
		return FALSE;
	}
	return TRUE;
}


static gboolean 
power_supply_battery_poll (gpointer data) {

	GSList *i;
	GSList *battery_devices;
	HalDevice *d;
	gboolean battery_polled = FALSE;

	/* for now do it only for primary batteries and extend if neede for the other types */
	battery_devices = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
                                                                    	    "battery.type",
 	                                                                    "primary");

	if (battery_devices) {
		for (i = battery_devices; i != NULL; i = g_slist_next (i)) {
			const char *subsys;

			d = HAL_DEVICE (i->data);

			/* don't poll batteries if quirk is in place */
			if (hal_device_property_get_bool (d, "battery.quirk.do_not_poll"))
				continue;

			subsys = hal_device_property_get_string (d, "info.subsystem");
			if (subsys && (strcmp(subsys, "power_supply") == 0)) {
				hal_util_grep_discard_existing_data();
				device_property_atomic_update_begin ();
				refresh_battery_fast(d);
				device_property_atomic_update_end ();
				battery_polled = TRUE;
			}
		}		
	}

	g_slist_free (battery_devices);

	battery_poll_running = battery_polled;
	return battery_polled;
}

static HalDevice *
power_supply_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *physdev,
		  const gchar *sysfs_path_in_devices)
{
	HalDevice *d = NULL;
	gboolean is_battery = FALSE;
	gboolean is_ac_adapter = FALSE;
	char *type = NULL;
	const char *battery_type = NULL;

	/* power_supply devices are very odd, they might be batteries or ac-adapters */
	type = hal_util_get_string_from_file (sysfs_path, "type");
	if (type == NULL) {
		/* we don't need to free */
		goto finish;
	}
	if (strcasecmp (type, "battery") == 0) {
		is_battery = TRUE;
		battery_type = "primary";
	} else if (strcasecmp (type, "ups") == 0) {
		is_battery = TRUE;
		battery_type = "ups";
	} else if (strcasecmp (type, "usb") == 0) {
		is_battery = TRUE;
		battery_type = "usb";
	} else if (strcasecmp (type, "mains") == 0) {
		is_ac_adapter = TRUE;
	} else {
		HAL_WARNING (("Power supply is neither ac_adapter or battery!"));
		goto finish;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");

	if (is_battery == TRUE) {
		hal_device_property_set_string (d, "info.category", "battery");
		if (battery_type != NULL)
			hal_device_property_set_string (d, "battery.type", battery_type);
		refresh_battery_slow (d);
		hal_device_add_capability (d, "battery");

		/* setup timer for things that we need to poll */
		if (!battery_poll_running) {
#ifdef HAVE_GLIB_2_14
			g_timeout_add_seconds (POWER_SUPPLY_BATTERY_POLL_INTERVAL,
                                               power_supply_battery_poll,
                                               NULL);
#else
			g_timeout_add (1000 * POWER_SUPPLY_BATTERY_POLL_INTERVAL,
                                       power_supply_battery_poll,
                                       NULL);
#endif
			battery_poll_running = TRUE;
		}
	}

	if (is_ac_adapter == TRUE) {
		hal_device_property_set_string (d, "info.category", "ac_adapter");
		hal_device_property_set_string (d, "info.product", "Generic AC Adapter Device");
		refresh_ac_adapter (d);
		hal_device_add_capability (d, "ac_adapter");
	}

	_have_sysfs_power_supply = TRUE;
finish:
	return d;
}

static gboolean
power_supply_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const char *dir;
	const char *name;

	dir = hal_device_property_get_string (d, "linux.sysfs_path");

	name = hal_util_get_last_element(dir);
	if (name) 
		hald_compute_udi (udi, sizeof (udi),
				  "%s_power_supply_%s_%s",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_string (d, "info.category"),
				  name);
	else
		hald_compute_udi (udi, sizeof (udi),
				  "%s_power_supply_%s",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_string (d, "info.category"));

	hal_device_set_udi (d, udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
ps3_system_bus_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev,
		    const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);

	hal_device_property_set_string (d, "ps3_system_bus.id", dev_id);

	g_snprintf (buf, sizeof (buf), "PS3 Device (%s)", hal_device_property_get_string (d, "ps3_system_bus.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
ps3_system_bus_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/ps3_system_bus_%s",
			  hal_device_property_get_string (d, "ps3_system_bus.id"));
	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/pseudo",
			  hal_device_property_get_string (d, "platform.id"));
	hal_device_set_udi (d, udi);

	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
rfkill_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
        gchar buf[64];
	const gchar *type;

	d = hal_device_new ();
	hal_device_add_capability (d, "killswitch");
	hal_device_property_set_string (d, "info.category", "killswitch");
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
		hal_device_copy_property( parent_dev, "info.vendor", d, "info.vendor");
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	type = hal_util_get_string_from_file (sysfs_path, "type");
	if (type == NULL)
		type = "unknown";

	if (strcasecmp (type, "wimax") == 0) {
		hal_device_property_set_string (d, "killswitch.type", "wwan");
	} else { 
		hal_device_property_set_string (d, "killswitch.type", type);
	}

	hal_util_set_int_from_file (d, "killswitch.state", sysfs_path, "state", 10);

	hal_device_property_set_string (d, "killswitch.access_method", "rfkill");

	hal_util_set_string_from_file (d, "killswitch.name", sysfs_path, "name");

        g_snprintf(buf, sizeof(buf), "%s %s Killswitch", hal_device_property_get_string (d, "killswitch.name"),
							 hal_device_property_get_string (d, "killswitch.type"));
        hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
rfkill_refresh (HalDevice *d)
{
	const char *sysfs_path;

	if ((sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path")) != NULL) {
		/* refresh the killswitch state */
		hal_util_set_int_from_file (d, "killswitch.state", sysfs_path, "state", 10);
	}

	return TRUE;
}

static gboolean
rfkill_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "%s_rfkill_%s_%s",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_string (d, "killswitch.name"),
			  hal_device_property_get_string (d, "killswitch.type"));
	hal_device_set_udi (d, udi);
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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_scsi_device_lun%d",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_int (d, "scsi.lun"));
	hal_device_set_udi (d, udi);
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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_scsi_generic",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
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
	const gchar *last_elem;
	gint host_num, bus_num, target_num, lun_num;
	int max;
	gint num = -1;
	int rc = FALSE;

	/* catch only scsi-devices */
	last_elem = hal_util_get_last_element (sysfs_path);
	if (sscanf (last_elem, "%d:%d:%d:%d", &host_num, &bus_num, &target_num, &lun_num) != 4)
		goto out;

	/* avoid loops */
	if (device_event->reposted)
		goto out;

	/* search devpath for missing host */
	g_strlcpy(path, sysfs_path, sizeof(path));
	max = 100;
	while (max--) {
		if (!hal_util_path_ascend (path))
			goto out;

		last_elem = hal_util_get_last_element (path);
		if (sscanf (last_elem, "host%d", &num) == 1)
			break;
	}

	/* the device must belong to this host */
	if (host_num != num)
		goto out;

	/* look if host is present */
	d = hal_device_store_match_key_value_string (hald_get_gdl (),
						     "linux.sysfs_path",
						     path);

	/* skip "add" if host is already created */
	if (action == HOTPLUG_ACTION_ADD && d != NULL)
		goto out;

	/* skip "remove" if host does not exist */
	if (action == HOTPLUG_ACTION_REMOVE && d == NULL)
		goto out;

	/* fake host event */
	rc = TRUE;
	host_event = g_slice_new0 (HotplugEvent);
	host_event->action = action;
	host_event->type = HOTPLUG_EVENT_SYSFS_DEVICE;
	g_strlcpy (host_event->sysfs.subsystem, "scsi_host", sizeof (host_event->sysfs.subsystem));
	g_strlcpy (host_event->sysfs.sysfs_path, path, sizeof (host_event->sysfs.sysfs_path));
	host_event->sysfs.net_ifindex = -1;

	/* insert host before our event, so we can see it as parent */
	if (action == HOTPLUG_ACTION_ADD) {
		hotplug_event_enqueue_at_front (device_event);
		hotplug_event_enqueue_at_front (host_event);
		hotplug_event_reposted (device_event);
		goto out;
	}

	/* remove host */
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

	/* ignore useless class device */
	if (strstr(sysfs_path, "class/scsi_host") != NULL)
		goto out;

	last_elem = hal_util_get_last_element (sysfs_path);
	if (sscanf (last_elem, "host%d", &host_num) != 1)
		goto out;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_int (d, "scsi_host.host", host_num);
        if (parent_dev != NULL)
                hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
        else
                hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_scsi_host",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);
	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
sdio_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	gchar buf[256];
	gint host_num, rca, card_id;

	if (parent_dev == NULL)
		return NULL;

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	bus_id = hal_util_get_last_element (sysfs_path);
	sscanf (bus_id, "mmc%d:%x:%d", &host_num, &rca, &card_id);
	hal_device_property_set_int (d, "sdio.rca", rca);
	hal_device_property_set_int (d, "sdio.card_id", card_id);

	hal_util_set_int_from_file (d, "sdio.vendor_id", sysfs_path, "vendor", 16);
	hal_util_set_int_from_file (d, "sdio.product_id", sysfs_path, "device", 16);
	hal_util_set_int_from_file (d, "sdio.class_id", sysfs_path, "class", 16);

	/* TODO: Here we should have a mapping to a name */
	g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", hal_device_property_get_int (d, "sdio.vendor_id"));
	hal_device_property_set_string (d, "info.vendor", buf);
	hal_device_property_set_string (d, "sdio.vendor", buf);

	/* TODO: Here we should have a mapping to a name */
	g_snprintf (buf, sizeof (buf), "Unknown (0x%04x)", hal_device_property_get_int (d, "sdio.product_id"));
	hal_device_property_set_string (d, "info.product", buf);
	hal_device_property_set_string (d, "sdio.product", buf);

	return d;
}

static gboolean
sdio_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "%s_sdio%d",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_int (d, "sdio.card_id"));
	hal_device_set_udi (d, udi);
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

	if (parent_dev == NULL || parent_path == NULL || device_file == NULL || device_file[0] == '\0') {
		goto out;
	}

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "info.category", "serial");
	hal_device_add_capability (d, "serial");
	hal_device_property_set_string (d, "serial.originating_device", hal_device_get_udi (parent_dev));
	hal_device_property_set_string (d, "serial.device", device_file);

	last_elem = hal_util_get_last_element(sysfs_path);
	if (sscanf (last_elem, "ttyS%d", &portnum) == 1) {
		hal_device_property_set_int (d, "serial.port", portnum);
		hal_device_property_set_string (d, "serial.type", "platform");
		hal_device_property_set_string (d, "info.product",
						hal_device_property_get_string (parent_dev, "info.product"));
	} else if (sscanf (last_elem, "ttyUSB%d", &portnum) == 1) {
		HalDevice *usbdev;
		int port_number;

		/* try to get the port number of the device and not of the whole USB subsystem */
		if (hal_util_get_int_from_file (sysfs_path, "device/port_number", &port_number, 10)) {
			hal_device_property_set_int (d, "serial.port", port_number);
		} else {
			hal_device_property_set_int (d, "serial.port", portnum);
		}
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
	const gchar *dev;

	/* FIXME TODO: check if there is an other way, to call the porber only
		       on ttyS* devices, than check the name of the device file */
	dev  = hal_device_property_get_string (d, "linux.device_file");
	if (dev && !strncmp(dev, "/dev/ttyS", 9))
		return "hald-probe-serial";
	else 
		return NULL;
}

static gboolean
serial_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "%s_serial_%s_%d",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_string (d, "serial.type"),
			  hal_device_property_get_int (d, "serial.port"));
	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi),
			  "%s_%s",
			  hal_device_property_get_string (d, "info.parent"),
			  hal_device_property_get_string (d, "serio.description"));
	hal_device_set_udi (d, udi);
	return TRUE;

}

/*--------------------------------------------------------------------------------------------------------------*/

static void
asound_card_id_set (int cardnum, HalDevice *d, const char *propertyname)
{
	char linestart[5];
	gchar *alsaname;

	snprintf (linestart, sizeof (linestart), "%2d [", cardnum);
	alsaname = hal_util_grep_file_next_line ("/proc/asound", "cards", linestart, FALSE);
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

	HAL_INFO (("sound_add: sysfs_path=%s device_file=%s parent_dev=0x%08x parent_path=%s", sysfs_path, device_file, parent_dev, parent_path));

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	device = hal_util_get_last_element(sysfs_path);

	if (device_file[0] == '\0' && parent_dev == NULL && parent_path == NULL) {
		goto out;
	} else if (device_file[0] == '\0' && parent_dev != NULL && parent_path != NULL) {
		HAL_INFO(("sound_add: handle sound card %s", sysfs_path));
		/* handle card devices */
		hal_device_property_set_string (d, "info.category", "sound");
		hal_device_add_capability (d, "sound");
		hal_device_property_set_string (d, "sound.originating_device", hal_device_get_udi (parent_dev));
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));

		if (sscanf (device, "card%d", &cardnum) == 1) {
			hal_device_property_set_int (d, "sound.card", cardnum);
			asound_card_id_set (cardnum, d, "sound.card_id");
			snprintf (buf, sizeof (buf), "%s Sound Card", hal_device_property_get_string (d, "sound.card_id"));
			hal_device_property_set_string (d, "info.product", buf);
		}
	} else if (parent_dev == NULL || parent_path == NULL) {
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
			hal_device_property_set_int (d, "alsa.card", cardnum);
			hal_device_property_set_int (d, "alsa.device", devicenum);
	
			asound_card_id_set (cardnum, d, "alsa.card_id");

			if (!hal_util_set_string_from_file (d, "alsa.pcm_class", sysfs_path, "pcm_class"))
				 hal_device_property_set_string (d, "alsa.pcm_class", "unknown");
	
			snprintf (aprocdir, sizeof (aprocdir), "/proc/asound/card%d/pcm%d%c", 
				  cardnum, devicenum, type);
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
			hal_device_property_set_int (d, "oss.card", cardnum);
	
			asound_card_id_set (cardnum, d, "oss.card_id");
	
			snprintf (aprocdir, sizeof (aprocdir), "/proc/asound/card%d/pcm0p", cardnum);
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

	if (hal_device_has_property(d, "sound.card")) {
		/* don't include card number as it may not be persistent across reboots */
		hald_compute_udi (udi, sizeof (udi),
				  "%s_sound_card_%i",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_int (d, "sound.card"));
	} else if (hal_device_has_property(d, "alsa.card")) {
		/* don't include card number as it may not be persistent across reboots */
		hald_compute_udi (udi, sizeof (udi),
				  "%s_alsa_%s_%i",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_string (d, "alsa.type"),
				  hal_device_property_get_int (d, "alsa.device"));
	} else if (hal_device_has_property(d, "oss.card")) {
		/* don't include card number as it may not be persistent across reboots */
		hald_compute_udi (udi, sizeof (udi),
				  "%s_oss_%s_%i",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_string (d, "oss.type"),
				  hal_device_property_get_int (d, "oss.device"));
	} else if (hal_device_has_property(d, "alsa.type")) {
		/* handle global ALSA devices */
		hald_compute_udi (udi, sizeof (udi),
				  "%s_alsa_%s",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_string (d, "alsa.type"));
	} else if (hal_device_has_property(d, "oss.type")) {
		/* handle global OSS devices */
		hald_compute_udi (udi, sizeof (udi),
				  "%s_oss_%s",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_string (d, "oss.type"));
	} else {
		/* fallback */
		hald_compute_udi (udi, sizeof (udi), "%s_sound_unknown",
				  hal_device_property_get_string (d, "info.parent"));
	}
	hal_device_set_udi (d, udi);

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
	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/tape_%s",
			  sysfs_name);
	hal_device_set_udi (d, udi);

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

		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		hal_device_property_set_string (d, "usb_device.linux.sysfs_path", sysfs_path);

		hal_util_set_string_from_file(d, "usb_device.configuration", sysfs_path, "configuration");
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
		hal_util_set_double_from_file (d, "usb_device.speed", sysfs_path, "speed");
		hal_util_set_double_from_file (d, "usb_device.version", sysfs_path, "version");

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

		/* take all usb_device.* properties from parent and make them usb.* on this object */
		if (parent_dev != NULL)
			hal_device_merge_with_rewrite (d, parent_dev, "usb.", "usb_device.");

		hal_util_set_driver (d, "info.linux.driver", sysfs_path);

		hal_device_property_set_string (d, "usb.linux.sysfs_path", sysfs_path);

		hal_util_set_int_from_file (d, "usb.interface.number", sysfs_path, "bInterfaceNumber", 10);

		hal_util_set_int_from_file (d, "usb.interface.class", sysfs_path, "bInterfaceClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.subclass", sysfs_path, "bInterfaceSubClass", 16);
		hal_util_set_int_from_file (d, "usb.interface.protocol", sysfs_path, "bInterfaceProtocol", 16);
		hal_util_set_string_from_file(d, "usb.interface.description", sysfs_path, "interface");

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
		hald_compute_udi (udi, sizeof (udi),
				  "%s_if%d",
				  hal_device_property_get_string (d, "info.parent"),
				  hal_device_property_get_int (d, "usb.interface.number"));
		hal_device_set_udi (d, udi);
	} else {
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/usb_device_%x_%x_%s",
				  hal_device_property_get_int (d, "usb_device.vendor_id"),
				  hal_device_property_get_int (d, "usb_device.product_id"),
				  hal_device_has_property (d, "usb_device.serial") ?
				  hal_device_property_get_string (d, "usb_device.serial") :
				  "noserial");
		hal_device_set_udi (d, udi);
	}

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

	if (parent_dev == NULL || parent_path == NULL || device_file == NULL || device_file[0] == '\0') {
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
		hald_compute_udi (udi, sizeof (udi),
				  "%s_hiddev",
				  hal_device_property_get_string (d, "info.parent"));
		hal_device_set_udi (d, udi);
	} else if (hal_device_has_capability (d, "printer")) {
		const char *serial;

		serial = hal_device_property_get_string (d, "printer.serial");
		hald_compute_udi (udi, sizeof (udi),
				  "%s_printer_%s",
				  hal_device_property_get_string (d, "info.parent"),
				  serial != NULL ? serial : "noserial");
		hal_device_set_udi (d, udi);
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

	hald_compute_udi (udi, sizeof (udi), "%s_usbraw",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);

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
	hal_device_property_set_string (d, "info.product", "Multimedia Device");
	hal_device_property_set_string (d, "video4linux.device", device_file);

out:
	return d;
}

static const gchar *
video4linux_get_prober (HalDevice *d)
{
	const char *prober = NULL;

	/* run prober only for video4linux devices */
	if (hal_device_has_capability (d, "video4linux")) {
		prober = "hald-probe-video4linux";
	}

	return prober;
}

static gboolean
video4linux_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi), "%s_video4linux",
			  hal_device_property_get_string (d, "info.parent"));
	hal_device_set_udi (d, udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
vio_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev,
		    const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	const gchar *dev_id;
	const gchar *dev_type;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "vio.id", dev_id);

	dev_type = hal_util_get_string_from_file (sysfs_path, "name");

	if (dev_type) {
		hal_device_property_set_string (d, "vio.type", dev_type);
		g_snprintf (buf, sizeof (buf), "Vio %s Device (%s)", dev_type, dev_id);
		hal_device_property_set_string (d, "info.product", buf);
	} else {
		hal_device_property_set_string (d, "info.product", "Vio Device (unknown)");
	}

	return d;
}

static gboolean
vio_compute_udi (HalDevice *d)
{
	gchar udi[256];
	const char *type;

	type = hal_device_property_get_string (d, "vio.type");

	if (type) {
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/vio_%s_%s",
				  type,
				  hal_device_property_get_string (d, "vio.id"));
	} else {
		hald_compute_udi (udi, sizeof (udi),
				  "/org/freedesktop/Hal/devices/vio_%s",
				  hal_device_property_get_string (d, "vio.id"));
	}

	hal_device_set_udi (d, udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
virtio_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev,
		    const gchar *sysfs_path_in_devices)
{
	HalDevice *d;
	const gchar *dev_id;
	gchar buf[64];

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	hal_util_set_driver (d, "info.linux.driver", sysfs_path);

	dev_id = hal_util_get_last_element (sysfs_path);

	hal_device_property_set_string (d, "virtio.id", dev_id);

	g_snprintf (buf, sizeof (buf), "VirtIO Device (%s)", hal_device_property_get_string (d, "virtio.id"));
	hal_device_property_set_string (d, "info.product", buf);

	return d;
}

static gboolean
virtio_compute_udi (HalDevice *d)
{
	gchar udi[256];

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/virtio_%s",
			  hal_device_property_get_string (d, "virtio.id"));
	hal_device_set_udi (d, udi);

	return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static HalDevice *
vmbus_add (const gchar *sysfs_path, const gchar *device_file, HalDevice *parent_dev, const gchar *parent_path)
{
	HalDevice *d;
	const gchar *bus_id;
	const gchar *class_id;
	const gchar *device_id;
	int busnum, devicenum;

	HAL_INFO (("vmbus_add: sysfs_path=%s device_file=%s parent_dev=0x%08x parent_path=%s", sysfs_path, device_file, parent_dev, parent_path));

	d = hal_device_new ();
	hal_device_property_set_string (d, "linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "info.vendor", "Microsoft/Citrix");
	hal_util_set_driver (d, "info.linux.driver", sysfs_path);
	
	if (parent_dev != NULL) {
		hal_device_property_set_string (d, "info.parent", hal_device_get_udi (parent_dev));
	} else {
		hal_device_property_set_string (d, "info.parent", "/org/freedesktop/Hal/devices/computer");
	}

	bus_id = hal_util_get_last_element (sysfs_path);
	hal_device_property_set_string (d, "vmbus.bus_id", bus_id);
	if (sscanf (bus_id, "vmbus_%d_%d", &busnum, &devicenum) == 2) {
		hal_device_property_set_int (d, "vmbus.bus_number", busnum);
		hal_device_property_set_int (d, "vmbus.device_number", devicenum);
	}

	device_id = hal_util_get_string_from_file (sysfs_path, "device_id");
	hal_device_property_set_string (d, "vmbus.device_id", device_id);
	class_id = hal_util_get_string_from_file (sysfs_path, "class_id");
	hal_device_property_set_string (d, "vmbus.class_id", class_id);

	if (class_id != NULL) {
		if (strcmp (class_id, "{f8615163-df3e-46c5-913ff2d2f965ed0e}") == 0) {
			hal_device_property_set_string (d, "info.product", "Network Virtualization Service Client Device");
		} else if (strcmp (class_id, "{ba6163d9-04a1-4d29-b60572e2ffb1dc7f}") == 0) {
			hal_device_property_set_string (d, "info.product", "Storage Virtualization Service Client Device");
		} else if (strcmp (class_id, "{c5295816-f63a-4d5f-8d1a4daf999ca185}") == 0) {
			// root device of the bus
			hal_device_property_set_string (d, "info.product", "Vmbus Device");
		}
	} 

	if (!hal_device_has_property(d, "info.product")) {
		char buf[64];
		g_snprintf (buf, sizeof (buf), "Virtualization Service Client Device (%s)", bus_id);
		hal_device_property_set_string (d, "info.product", buf);
	}

	return d;
}

static gboolean
vmbus_compute_udi (HalDevice *d)
{
	gchar udi[256];
	
	hald_compute_udi (udi, sizeof (udi), "/org/freedesktop/Hal/devices/_%s",
			 hal_device_property_get_string (d, "vmbus.bus_id"));
	hal_device_set_udi (d, udi);

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

	hald_compute_udi (udi, sizeof (udi),
			  "/org/freedesktop/Hal/devices/xen_%s",
			  hal_device_property_get_string (d, "xen.bus_id"));
	hal_device_set_udi (d, udi);
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
	gboolean (*refresh) (HalDevice *d);
	gboolean (*remove) (HalDevice *d);
};

/*--------------------------------------------------------------------------------------------------------------*/
/* 		 	PLEASE KEEP THE SUBSYSTEMS IN ALPHABETICAL ORDER !!!					*/
/*--------------------------------------------------------------------------------------------------------------*/

static DevHandler dev_handler_backlight =
{
       .subsystem    = "backlight",
       .add          = backlight_add,
       .compute_udi  = backlight_compute_udi,
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

static DevHandler dev_handler_drm =
{
       .subsystem    = "drm",
       .add          = drm_add,
       .compute_udi  = drm_compute_udi,
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

/* krh's new firewire stack */
static DevHandler dev_handler_firewire = { 
	.subsystem    = "firewire",
	.add          = firewire_add,
	.get_prober   = firewire_get_prober,
	.post_probing = firewire_post_probing,
	.compute_udi  = firewire_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_ide = { 
	.subsystem   = "ide",
	.add         = ide_add,
	.compute_udi = ide_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_ieee1394 = { 
	.subsystem   = "ieee1394",
	.add         = ieee1394_add,
	.compute_udi = ieee1394_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_input = 
{ 
	.subsystem    = "input",
	.add          = input_add,
	.get_prober   = input_get_prober,
	.post_probing = input_post_probing,
	.compute_udi  = input_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_iucv = {
	.subsystem   = "iucv",
	.add         = iucv_add,
	.compute_udi = iucv_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_leds = {
	.subsystem   = "leds",
	.add         = leds_add,
	.compute_udi = leds_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_memstick = { 
	.subsystem   = "memstick",
	.add         = memstick_add,
	.compute_udi = memstick_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_memstick_host =
{
	.subsystem    = "memstick_host",
	.add          = memstick_host_add,
	.get_prober   = NULL,
	.post_probing = NULL,
	.compute_udi  = memstick_host_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_mmc = { 
	.subsystem   = "mmc",
	.add         = mmc_add,
	.compute_udi = mmc_compute_udi,
	.remove      = dev_remove
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

static DevHandler dev_handler_net = 
{ 
	.subsystem    = "net",
	.add          = net_add,
	.refresh      = net_refresh,
	.get_prober   = net_get_prober,
	.post_probing = net_post_probing,
	.compute_udi  = net_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_of_platform =
{
	.subsystem   = "of_platform",
	.add         = of_platform_add,
	.compute_udi = of_platform_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_pci = { 
	.subsystem   = "pci",
	.add         = pci_add,
	.compute_udi = pci_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_pcmcia = { 
	.subsystem   = "pcmcia",
	.add         = pcmcia_add,
	.compute_udi = pcmcia_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_platform = {
	.subsystem   = "platform",
	.add         = platform_add,
	.refresh     = platform_refresh,
	.compute_udi = platform_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_pnp = { 
	.subsystem   = "pnp",
	.add         = pnp_add,
	.compute_udi = pnp_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_ppdev = { 
	.subsystem   = "ppdev",
	.add         = ppdev_add,
	.compute_udi = ppdev_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_ps3_system_bus =
{
	.subsystem   = "ps3_system_bus",
	.add         = ps3_system_bus_add,
	.compute_udi = ps3_system_bus_compute_udi,
	.remove      = dev_remove
};

/* SCSI debug, to test thousends of fake devices */
static DevHandler dev_handler_pseudo = {
	.subsystem   = "pseudo",
	.add         = pseudo_add,
	.compute_udi = pseudo_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_power_supply =
{
       .subsystem    = "power_supply",
       .add          = power_supply_add,
       .refresh      = power_supply_refresh,
       .compute_udi  = power_supply_compute_udi,
       .remove       = dev_remove
};

static DevHandler dev_handler_rfkill =
{
       .subsystem    = "rfkill",
       .add          = rfkill_add,
       .compute_udi  = rfkill_compute_udi,
       .refresh      = rfkill_refresh,
       .remove       = dev_remove
};

static DevHandler dev_handler_scsi = { 
	.subsystem   = "scsi",
	.add         = scsi_add,
	.compute_udi = scsi_compute_udi,
	.remove      = dev_remove
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

static DevHandler dev_handler_sdio = { 
	.subsystem   = "sdio",
	.add         = sdio_add,
	.compute_udi = sdio_compute_udi,
	.remove      = dev_remove
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

static DevHandler dev_handler_serio = { 
	.subsystem   = "serio",
	.add         = serio_add,
	.compute_udi = serio_compute_udi,
	.remove      = dev_remove
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

static DevHandler dev_handler_usb = { 
	.subsystem   = "usb",
	.add         = usb_add,
	.compute_udi = usb_compute_udi,
	.remove      = dev_remove
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
	.get_prober   = video4linux_get_prober,
	.post_probing = NULL,
	.compute_udi  = video4linux_compute_udi,
	.remove       = dev_remove
};

static DevHandler dev_handler_vio =
{
	.subsystem   = "vio",
	.add         = vio_add,
	.compute_udi = vio_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_virtio =
{
	.subsystem   = "virtio",
	.add         = virtio_add,
	.compute_udi = virtio_compute_udi,
	.remove      = dev_remove
};

static DevHandler dev_handler_vmbus =
{
	.subsystem   = "vmbus",
	.add         = vmbus_add,
	.compute_udi = vmbus_compute_udi,
	.remove      = dev_remove
};


static DevHandler dev_handler_xen = {
	.subsystem   = "xen",
	.add         = xen_add,
	.compute_udi = xen_compute_udi,
	.remove      = dev_remove
};

/*--------------------------------------------------------------------------------------------------------------*/
/* 		 	PLEASE KEEP THE SUBSYSTEMS IN ALPHABETICAL ORDER !!!					*/
/*--------------------------------------------------------------------------------------------------------------*/

static DevHandler *dev_handlers[] = {
	&dev_handler_backlight,
	&dev_handler_bluetooth,
	&dev_handler_ccw,
	&dev_handler_ccwgroup,
	&dev_handler_drm,
	&dev_handler_dvb,
	&dev_handler_firewire,
	&dev_handler_ide,
	&dev_handler_ieee1394,
	&dev_handler_input,
	&dev_handler_iucv,
	&dev_handler_leds,
	&dev_handler_mmc,
	&dev_handler_memstick,
	&dev_handler_memstick_host,
	&dev_handler_mmc_host,
	&dev_handler_net,
	&dev_handler_of_platform,
	&dev_handler_pci,
	&dev_handler_pcmcia,
	&dev_handler_platform,
	&dev_handler_pnp,
	&dev_handler_power_supply,
	&dev_handler_ppdev,
	&dev_handler_ps3_system_bus,
	&dev_handler_pseudo,
	&dev_handler_rfkill,
	&dev_handler_scsi,
	&dev_handler_scsi_generic,
	&dev_handler_scsi_host,
	&dev_handler_sdio,
	&dev_handler_serial,
	&dev_handler_serio,
	&dev_handler_sound,
	&dev_handler_tape,
	&dev_handler_tape390,
	/* Don't change order of usbclass and usb */
	&dev_handler_usbclass,
	&dev_handler_usb,
	/* -------------------------------------- */
	&dev_handler_usbraw,
	&dev_handler_video4linux,
	&dev_handler_vio,
	&dev_handler_virtio,
	&dev_handler_vmbus,
	&dev_handler_xen,
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
dev_callouts_remove_child_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Remove callouts completed udi=%s", hal_device_get_udi (d)));

	if (!hal_device_store_remove (hald_get_gdl (), d)) {
		HAL_WARNING (("Error removing device"));
	}

	g_object_unref (d);
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
		HAL_INFO (("device removed due to prober fail"));
		hal_device_print (d);
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
			HalDevice *check;

			if (strcmp (subsystem, "scsi") == 0)
				if (missing_scsi_host (sysfs_path, (HotplugEvent *)end_token, HOTPLUG_ACTION_ADD))
					goto out;

			/* check if there is already a device with this sysfs_path in the system */
			if ((check = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path)) != NULL ||
			    (check = hal_device_store_match_key_value_string (hald_get_tdl (), "linux.sysfs_path", sysfs_path)) != NULL) {
				HAL_WARNING(("Have already a device with sysfs_path='%s' and udi='%s'. Ignore new add event for now.", 
					     sysfs_path, hal_device_get_udi(check)));
				/* maybe we should do a refresh on the found device ??? */
				hotplug_event_end (end_token);
				goto out; 
			}

			/* attempt to add the device */
			d = handler->add (sysfs_path, device_file, parent_dev, parent_path);
			if (d == NULL) {
				/* didn't match - there may be a later handler for the device though */
				continue;
			}

			hal_device_property_set_int (d, "linux.hotplug_type", HOTPLUG_EVENT_SYSFS_DEVICE);
			hal_device_property_set_string (d, "linux.subsystem", subsystem);
			
			/* only set info.subsystem if it's not set already to prevent trouble with usb/usb_device and other */
			if (!hal_device_has_property(d, "info.subsystem")) {
				hal_device_property_set_string (d, "info.subsystem", subsystem);
			}

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
	HalDevice *child;
	GSList *children;
	GSList *tmp;

	HAL_INFO (("remove_dev: subsys=%s sysfs_path=%s", subsystem, sysfs_path));

	d = hal_device_store_match_key_value_string (hald_get_gdl (), "linux.sysfs_path", sysfs_path);
	if (d == NULL) {
		HAL_WARNING (("Error removing device"));
	} else {
		for (i = 0; dev_handlers [i] != NULL; i++) {
			DevHandler *handler;

			handler = dev_handlers[i];
			if (strcmp (handler->subsystem, subsystem) == 0) {
				if (strcmp (subsystem, "scsi") == 0) {
					missing_scsi_host(sysfs_path, (HotplugEvent *)end_token, HOTPLUG_ACTION_REMOVE);
				}

				/* check if there are children left before remove the device */
				children = hal_device_store_match_multiple_key_value_string (hald_get_gdl (), 
                        								     "info.parent",
											     hal_device_get_udi(d));

				for (tmp = children; tmp != NULL; tmp = g_slist_next (tmp)) {
			                child = HAL_DEVICE (tmp->data);
					/* find childs without sysfs path as e.g. spawned devices*/
					if (hal_device_property_get_string(child, "linux.sysfs_path") == NULL) {
						HAL_INFO(("Remove now: %s as child of: %s", hal_device_get_udi(child), hal_device_get_udi(d)));
						hal_util_callout_device_remove (child, dev_callouts_remove_child_done, NULL, NULL);
					}
                		}
				g_slist_free (children);

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

void
hotplug_event_refresh_dev (const gchar *subsystem, const gchar *sysfs_path, HalDevice *d, void *end_token)
{
	guint i;
	DevHandler *handler;

	HAL_INFO (("refresh_dev: subsys=%s", subsystem));

	for (i = 0; dev_handlers [i] != NULL; i++) {
		handler = dev_handlers[i];
		if (strcmp (handler->subsystem, subsystem) == 0) {
			if (handler->refresh != NULL) {
				handler->refresh (d);
			}
			goto out;
		}
	}

out:
	/* done with change event */
	hotplug_event_end (end_token);
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

	subsystem = hal_device_property_get_string (d, "info.subsystem");
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");
	device_file = hal_device_property_get_string (d, "linux.device_file");

	hotplug_event = g_slice_new0 (HotplugEvent);
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

	/* be sure that we use linux.subsystem here because info.subsystem can differ see e.g. usb/usb_device */
	subsystem = hal_device_property_get_string (d, "linux.subsystem");
	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	hotplug_event = g_slice_new0 (HotplugEvent);
	hotplug_event->action = HOTPLUG_ACTION_REMOVE;
	hotplug_event->type = HOTPLUG_EVENT_SYSFS;
	g_strlcpy (hotplug_event->sysfs.subsystem, subsystem, sizeof (hotplug_event->sysfs.subsystem));
	g_strlcpy (hotplug_event->sysfs.sysfs_path, sysfs_path, sizeof (hotplug_event->sysfs.sysfs_path));
	hotplug_event->sysfs.device_file[0] = '\0';
	hotplug_event->sysfs.net_ifindex = -1;

	return hotplug_event;
}
