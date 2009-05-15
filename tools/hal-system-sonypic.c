/*! @file	hal-system-sonypic.c
 *  @brief	Issue ioctl's from methods invoked by HAL.
 *  @author	Bastien Nocera <bnocera@redhat.com>
 *  @author	Richard Hughes <richard@hughsie.com>
 *  @date	Thursday 08 February 2007
 */
/*
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <glib.h>

#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/sonypi.h>
#define SONYPI_DEV	"/dev/sonypi"
#define __u8		u_int8_t
#endif /* __linux__ */

#include <libhal/libhal.h>

/**
 * @defgroup HalSystemSonypi  Use sonypi specific ioctls
 * @ingroup HalMisc
 *
 * @brief A commandline tool for running sonypi specific ioctl's. Uses libhal
 *
 * @{
 */

/** Gets the LCD brightness
 *
 *  @param	val		The returned brightness value, 0..255
 *  @return			Success, TRUE or FALSE
 */
static int
sonypi_get_lcd_brightness (__u8 *val)
{
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
	return FALSE;			/* FIXME implement */
#elif sun
	return FALSE;			/* FIXME implement */
#else
	int fd;
	int ret;

	fd = open (SONYPI_DEV, O_RDONLY);
	if (fd < 0) {
		fprintf (stderr, "sonyi : Failed to open " SONYPI_DEV "\n");
		return FALSE;
	}
	ret = ioctl (fd, SONYPI_IOCGBRT, val);
	close (fd);

	if (ret < 0) {
		fprintf (stderr, "sonypi : SONYPI_IOCGBRT failed\n");
		return FALSE;
	}
	return TRUE;
#endif /* ! __FreeBSD__ */
}

/** Gets whether the builtin Bluetooth adapter is turned on
 *
 * @param      val              The returned power status
 * @return			Success, TRUE or FALSE
 */
static int
sonypi_get_bluetooth_power (int *found, int usb_vendor_id, int usb_product_id)
{
	LibHalContext *ctx;
	DBusError err;
	char **devices;
	int num_devices, i;

	*found = FALSE;

	dbus_error_init (&err);
	ctx = libhal_ctx_init_direct (&err);
	if (ctx == NULL) {
		fprintf (stderr, "Cannot contact HAL: %s\n", dbus_error_is_set (&err) ?
				err.message : "No reason");
		if (dbus_error_is_set (&err))
			LIBHAL_FREE_DBUS_ERROR (&err);
		return FALSE;
	}
	devices = libhal_get_all_devices (ctx, &num_devices, &err);
	if (dbus_error_is_set (&err))
		goto error_set;

	for(i = 0; i < num_devices; i++) {
		int device_vid, device_pid;
		if (libhal_device_property_exists (ctx, devices[i], "usb.vendor_id", &err) == FALSE) {
			if (dbus_error_is_set (&err))
				goto error_set;
			continue;
		}
		device_vid = libhal_device_get_property_int (ctx, devices[i], "usb.vendor_id", &err);
		if (dbus_error_is_set (&err))
			goto error_set;
		if (device_vid != usb_vendor_id)
			continue;

		if (libhal_device_property_exists (ctx, devices[i], "usb.product_id", &err) == FALSE) {
			if (dbus_error_is_set (&err))
				goto error_set;
			continue;
		}
		device_pid = libhal_device_get_property_int (ctx, devices[i], "usb.product_id", &err);
		if (dbus_error_is_set (&err))
			goto error_set;
		if (device_pid != usb_product_id)
			continue;

		/* Yay! Found the device */
		*found = TRUE;
		break;
	}

	libhal_ctx_shutdown (ctx, NULL);
	return TRUE;

error_set:
	fprintf (stderr, "Error looking for the Bluetooth device: %s\n", err.message);
	LIBHAL_FREE_DBUS_ERROR (&err);
	libhal_ctx_shutdown (ctx, &err);

	return FALSE;
}

/** Sets the LCD brightness
 *
 *  @param	val		The brightness value we want to set, 0..255
 *  @return			Success, TRUE or FALSE
 */
static int
sonypi_set_lcd_brightness (__u8 val)
{
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
	return FALSE;			/* FIXME implement */
#elif sun
	return FALSE;			/* FIXME implement */
#else
	int ret;
	int fd;

	fd = open (SONYPI_DEV, O_RDWR);
	if (fd < 0) {
		fprintf (stderr, "sonypi : Failed to open " SONYPI_DEV "\n");
		return FALSE;
	}
	ret = ioctl (fd, SONYPI_IOCSBRT, &val);
	close (fd);

	if (ret < 0) {
		fprintf (stderr, "sonypi : SONYPI_IOCSBRT failed\n");
		return FALSE;
	}
	return TRUE;
#endif /* ! __FreeBSD__ */
}

/** Sets the builtin Bluetooth adapter's power
 *
 * @param	val		The power value we want to set
 * @return			Success, TRUE or FALSE
 */
static int
sonypi_set_bluetooth_power (int val)
{
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
	return FALSE;			/* FIXME implement */
#elif sun
	return FALSE;			/* FIXME implement */
#else
	int ret;
	int fd;

	fd = open (SONYPI_DEV, O_RDWR);
	if (fd < 0) {
		fprintf (stderr, "sonypi : Failed to open " SONYPI_DEV "\n");
		return FALSE;
	}
	ret = ioctl (fd, SONYPI_IOCSBLUE, &val);
	close (fd);

	if (ret < 0) {
		fprintf (stderr, "sonypi : SONYPI_IOCSBLUE failed\n");
		return FALSE;
	}
	return TRUE;
#endif /* ! __FreeBSD__ */
}

/** Print out program usage.
 *
 *  @param  argc                Number of arguments given to program
 *  @param  argv                Arguments given to program
 */
static void
usage (int argc, char *argv[])
{
	fprintf (stderr, "\nusage : hal-system-sonypi "
			 "[setlcd x] [getlcd] [setbluetooth x] [getbluetooth]\n");
	fprintf (stderr,
 "\n"
 "        setlcd x       Sets the LCD to a range 0..14 (0 is off)\n"
 "        getlcd         Gets the current LCD brightness value\n"
 "        setbluetooth x Sets the Bluetooth power\n"
 "        getbluetooth   Gets the Bluetooth power\n"
 "        help           Show this information and exit\n"
 "\n"
 "This program calls sonypi specific ioctls from within scripts run by HAL.\n");
}

/** Entry point
 *
 *  @param  argc                Number of arguments given to program
 *  @param  argv                Arguments given to program
 *  @return                     Return code
 */
int
main (int argc, char *argv[])
{
	__u8 brightness;
	int power;
	int ret;

	if (argc == 2) {
		if (strcmp (argv[1], "getlcd") == 0) {
			ret = sonypi_get_lcd_brightness (&brightness);
			if (ret == FALSE)
				return EXIT_FAILURE;
			printf ("%i", brightness);
			return EXIT_SUCCESS;
		}
		if (strcmp (argv[1], "getbluetooth") == 0) {
			const char *usb_vendor_id, *usb_product_id;
			int vid, pid;

			power = 0;
			if ((usb_vendor_id = g_getenv ("HAL_PROP_KILLSWITCH_EXPECTED_USB_VENDOR_ID")) == NULL
			    || (usb_product_id = g_getenv ("HAL_PROP_KILLSWITCH_EXPECTED_USB_PRODUCT_ID")) == NULL) {
				fprintf (stderr, "sonypi : no expected USB IDs supplied\n");
				return EXIT_FAILURE;
			}

			vid = (int) g_ascii_strtoull (usb_vendor_id, NULL, 0);
			pid = (int) g_ascii_strtoull (usb_product_id, NULL, 0);
			ret = sonypi_get_bluetooth_power (&power, vid, pid);

			if (ret == FALSE)
				return EXIT_FAILURE;
			printf ("%i", power);
			return EXIT_SUCCESS;
		}
	} else if (argc == 3) {
		if (strcmp (argv[1], "setlcd") == 0) {
			/* This will clip the brightness to a number
			 * between 0 and 255 */
			brightness = atoi (argv[2]);
			ret = sonypi_set_lcd_brightness (brightness);
			if (ret == FALSE)
				return EXIT_FAILURE;
			return EXIT_SUCCESS;
		}
		if (strcmp (argv[1], "setbluetooth") == 0) {
			power = atoi (argv[2]);
			ret = sonypi_set_bluetooth_power (power);
			if (ret == FALSE)
				return EXIT_FAILURE;
			return EXIT_SUCCESS;
		}
	}
	usage (argc, argv);
	return EXIT_SUCCESS;
}

/**
 * @}
 */
