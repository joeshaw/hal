/***************************************************************************
 * CVSID: $Id$
 *
 * addon-hid-ups.c : Detect UPS'es using the USB HID interface and
 *                   add and maintain battery.* properties
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
 *
 * Based on hid-ups.c: Copyright (c) 2001 Vojtech Pavlik
 *                     <vojtech@ucw.cz>, Copyright (c) 2001 Paul
 *                     Stewart <hiddev@wetlogic.net>, Tweaked by Kern
 *                     Sibbald <kern@sibbald.com> to learn about USB
 *                     UPSes.  hid-ups.c is GPLv2 and available from
 *                     examples directory of version 3.10.16 of the
 *                     acpupsd project; see http://www.apcupsd.com.
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

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <asm/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/hiddev.h>

#include "libhal/libhal.h"

#define UPS_USAGE		0x840000
#define UPS_SERIAL		0x8400fe
#define BAT_CHEMISTRY		0x850089
#define UPS_CAPACITY_MODE	0x85002c

#define UPS_SHUTDOWN_IMMINENT	0x840069
#define UPS_BATTERY_VOLTAGE	0x840030
#define UPS_BELOW_RCL		0x840042
#define UPS_CHARING		0x840044
#define UPS_DISCHARGING 	0x850045
#define UPS_REMAINING_CAPACITY	0x850066
#define UPS_RUNTIME_TO_EMPTY	0x850068
#define UPS_AC_PRESENT		0x8500d0

#define STATE_NORMAL 0		      /* unit powered */
#define STATE_DEBOUNCE 1	      /* power failure */
#define STATE_BATTERY 2 	      /* power failure confirmed */

static dbus_bool_t
is_ups (int fd)
{
	unsigned int i;
	dbus_bool_t ret;
	struct hiddev_devinfo device_info;

	ret = FALSE;

	if (ioctl (fd, HIDIOCGDEVINFO, &device_info) < 0)
		goto out;

	for (i = 0; i < device_info.num_applications; i++) {
		if ((ioctl(fd, HIDIOCAPPLICATION, i) & 0xff0000) == UPS_USAGE) {
			ret = TRUE;
			goto out;
		}			
	}

out:
	return ret;
}

static char *
ups_get_string (int fd, int sindex)
{
	static struct hiddev_string_descriptor sdesc;
	
	if (sindex == 0) {
		return "";
	}
	sdesc.index = sindex;
	if (ioctl (fd, HIDIOCGSTRING, &sdesc) < 0) {
		return "";
	}
	fprintf (stderr, "foo: '%s'\n", sdesc.value);
	return sdesc.value;
}


static dbus_bool_t
ups_get_static (LibHalContext *ctx, const char *udi, int fd)
{
	int ret;
	struct hiddev_report_info rinfo;
	struct hiddev_field_info finfo;
	struct hiddev_usage_ref uref;
	int rtype;
	unsigned int i, j;
	DBusError error;

	/* set to failure */
	ret = FALSE;

	/* first check that we are an UPS */
	if (!is_ups (fd))
		goto out;

	for (rtype = HID_REPORT_TYPE_MIN; rtype <= HID_REPORT_TYPE_MAX; rtype++) {
		rinfo.report_type = rtype;
		rinfo.report_id = HID_REPORT_ID_FIRST;
		while (ioctl (fd, HIDIOCGREPORTINFO, &rinfo) >= 0) {
			for (i = 0; i < rinfo.num_fields; i++) { 
				memset (&finfo, 0, sizeof (finfo));
				finfo.report_type = rinfo.report_type;
				finfo.report_id = rinfo.report_id;
				finfo.field_index = i;
				ioctl (fd, HIDIOCGFIELDINFO, &finfo);
				
				memset (&uref, 0, sizeof (uref));
				for (j = 0; j < finfo.maxusage; j++) {
					uref.report_type = finfo.report_type;
					uref.report_id = finfo.report_id;
					uref.field_index = i;
					uref.usage_index = j;
					ioctl (fd, HIDIOCGUCODE, &uref);
					ioctl (fd, HIDIOCGUSAGE, &uref);

					dbus_error_init (&error);

					switch (uref.usage_code) {

					case 0x850066: /* RemainingCapacity */
						libhal_device_set_property_int (
							ctx, udi, "battery.charge_level.current", uref.value, &error);
						libhal_device_set_property_string (
							ctx, udi, "battery.charge_level.unit", "percent", &error);
						break;

					case 0x850068: /* RunTimeToEmpty */
						libhal_device_set_property_int (
							ctx, udi, "battery.remaining_time", uref.value, &error);
						break;

					case 0x850044: /* Charging */
						libhal_device_set_property_bool (
							ctx, udi, "battery.rechargeable.is_charging", uref.value != 0, &error);
						break;

					case 0x850045: /* Discharging */
						libhal_device_set_property_bool (
							ctx, udi, "battery.rechargeable.is_discharging", uref.value != 0, &error);
						break;

					case 0x8500d1: /* BatteryPresent */
						libhal_device_set_property_bool (
							ctx, udi, "battery.present", uref.value != 0, &error);
						break;

					case 0x850088: /* iDeviceName */
						libhal_device_set_property_string (
							ctx, udi, "foo", 
							ups_get_string (fd, uref.value), &error);
						break;

					case 0x850089: /* iDeviceChemistry */
						libhal_device_set_property_string (
							ctx, udi, "battery.technology", 
							ups_get_string (fd, uref.value), &error);
						break;

					case 0x85008b: /* Rechargeable */
						libhal_device_set_property_bool (
							ctx, udi, "battery.is_rechargeable", uref.value != 0, &error);
						break;

					case 0x85008f: /* iOEMInformation */
						libhal_device_set_property_string (
							ctx, udi, "battery.vendor", 
							ups_get_string (fd, uref.value), &error);
						break;

					case 0x8400fe: /* iProduct */
						libhal_device_set_property_string (
							ctx, udi, "battery.model", 
							ups_get_string (fd, uref.value), &error);
						break;

					case 0x8400ff: /* iSerialNumber */
						libhal_device_set_property_string (
							ctx, udi, "battery.serial", 
							ups_get_string (fd, uref.value), &error);
						break;

					case 0x850083: /* DesignCapacity */
						libhal_device_set_property_int (
							ctx, udi, "battery.charge_level.design", uref.value, &error);
						libhal_device_set_property_int (
							ctx, udi, "battery.charge_level.last_full", uref.value, &error);
						break;

					default:
						break;
					}
				}
			}
			rinfo.report_id |= HID_REPORT_ID_NEXT;
		}
	}

	libhal_device_set_property_string (ctx, udi, "battery.type", "ups", &error);
	libhal_device_add_capability (ctx, udi, "battery", &error);

	ret = TRUE;

out:
	return ret;
}

int
main (int argc, char *argv[])
{
	int fd;
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	unsigned int i;
	fd_set fdset;
	struct hiddev_event ev[64];
	int rd;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);
	if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
		goto out;

	if ((ctx = libhal_ctx_new ()) == NULL)
		goto out;
	if (!libhal_ctx_set_dbus_connection (ctx, conn))
		goto out;
	if (!libhal_ctx_init (ctx, &error))
		goto out;

	device_file = getenv ("HAL_PROP_HIDDEV_DEVICE");
	if (device_file == NULL)
		goto out;

	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	if (!ups_get_static (ctx, udi, fd))
		goto out;

	FD_ZERO(&fdset);
	while (1) {
		FD_SET(fd, &fdset);
		rd = select(fd+1, &fdset, NULL, NULL, NULL);
		
		if (rd > 0) {
			rd = read(fd, ev, sizeof(ev));
			if (rd < (int) sizeof(ev[0])) {
				close(fd);
				goto out;
			}

			for (i = 0; i < rd / sizeof(ev[0]); i++) {
				DBusError error;
				
				dbus_error_init (&error);
				switch (ev[i].hid) {
				case 0x850066: /* RemainingCapacity */
					libhal_device_set_property_int (
						ctx, udi, "battery.charge_level.current", ev[i].value, &error);
					break;
					
				case 0x850068: /* RunTimeToEmpty */
					libhal_device_set_property_int (
						ctx, udi, "battery.remaining_time", ev[i].value, &error);
					break;
					
				case 0x850044: /* Charging */
					libhal_device_set_property_bool (
						ctx, udi, "battery.rechargeable.is_charging", ev[i].value != 0, &error);
					break;
					
				case 0x850045: /* Discharging */
					libhal_device_set_property_bool (
						ctx, udi, "battery.rechargeable.is_discharging", ev[i].value != 0, &error);
					break;
					
				default:
					break;
				}
			}
		}
	}

out:
	return 0;
}
