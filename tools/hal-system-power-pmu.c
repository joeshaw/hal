/*! @file	hal-system-power-pmu.c
 *  @brief	Issue ioctl's from methods invoked by HAL.
 *  @author	Richard Hughes <richard@hughsie.com>
 *  @date	2005-12-11
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

#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/pmu.h>
#endif
#include <libhal/libhal.h>

#define PMUDEV		"/dev/pmu"
#define __u32		unsigned int

/** 
 *  pmac_sleep:
 *
 *  Returns:		Success, TRUE or FALSE
 *
 *  Issues a sleep ioctl 
 */
static int
pmac_sleep (void)
{
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
	return FALSE;			/* FIXME implement */
#elif sun
	return FALSE;			/* FIXME implement */
#elif __GNU__
	return FALSE;			/* FIXME implement */
#else
	int ret;
	int fd;

	fd = open (PMUDEV, O_RDWR);
	if (fd < 0) {
		fprintf (stderr, "power-pmu : Failed to open " PMUDEV "\n");
		return FALSE;
	}
	/* returns when machine wakes up */
	ret = ioctl (fd, PMU_IOC_SLEEP, 0);
	close (fd);
	if (ret < 0) {
		fprintf (stderr, "power-pmu : PMU_IOC_SLEEP failed\n");
		return FALSE;
	}
	return TRUE;
#endif /* ! __FreeBSD__ */
}

/** 
 *  pmac_get_lcd_brightness:
 *  @val:	The returned brightness value, 0..15
 *
 *  Returns:	Success, TRUE or FALSE
 *
 *  Gets the LCD brightness 
 */
static int
pmac_get_lcd_brightness (int *val)
{
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
	return FALSE;			/* FIXME implement */
#elif sun
	return FALSE;			/* FIXME implement */
#elif __GNU__
	return FALSE;			/* FIXME implement */
#else
	int ret;
	int fd;

	fd = open (PMUDEV, O_RDWR);
	if (fd < 0) {
		fprintf (stderr, "power-pmu : Failed to open " PMUDEV "\n");
		return FALSE;
	}
	ret = ioctl (fd, PMU_IOC_GET_BACKLIGHT, val);
	close (fd);

	if (ret < 0) {
		fprintf (stderr, "power-pmu : PMU_IOC_GET_BACKLIGHT failed\n");
		return FALSE;
	}
	return TRUE;
#endif /* ! __FreeBSD__ */
}

/** 
 *  pmac_set_lcd_brightness:
 *  @val:		The brightness value we want to set, 0..15
 *  
 *  Returns:		Success, TRUE or FALSE
 *
 *  Sets the LCD brightness 
 */
static int
pmac_set_lcd_brightness (int val)
{
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
	return FALSE;			/* FIXME implement */
#elif sun
	return FALSE;			/* FIXME implement */
#elif __GNU__
	return FALSE;			/* FIXME implement */
#else
	int ret;
	int fd;

	fd = open (PMUDEV, O_RDWR);
	if (fd < 0) {
		fprintf (stderr, "power-pmu : Failed to open " PMUDEV "\n");
		return FALSE;
	}
	ret = ioctl (fd, PMU_IOC_SET_BACKLIGHT, &val);
	close (fd);

	if (ret < 0) {
		fprintf (stderr, "power-pmu : PMU_IOC_SET_BACKLIGHT failed\n");
		return FALSE;
	}
	return TRUE;
#endif /* ! __FreeBSD__ */
}

/** 
 *  usage:
 *  @argc:	Number of arguments given to program
 *  @argv:	Arguments given to program
 *
 *  Print out program usage. 
 */
static void
usage (int argc, char *argv[])
{
	fprintf (stderr, "\nusage : hal-system-power-pmu "
			 "[setlcd x] [getlcd] [suspend]\n");
	fprintf (stderr,
 "\n"
 "        setlcd x       Sets the LCD to a range 0..14 (0 is off)\n"
 "        getlcd         Gets the current LCD brightness value\n"
 "        backlightoff   Turns off the LCD backlight (setlcd turns it on again)\n"
 "        sleep          Initiate an immediate sleep\n"
 "        help           Show this information and exit\n"
 "\n"
 "This program calls PMU specific ioctls from within scripts run by HAL.\n");
}

/** 
 *  main:
 *  @argc:        Number of arguments given to program
 *  @argv:        Arguments given to program
 *
 *  @return       Return code
 *
 *  Main entry point 
 */
int
main (int argc, char *argv[])
{
	int brightness;
	int ret;

	if (argc == 2) {
		if (strcmp (argv[1], "sleep") == 0) {
			ret = pmac_sleep ();
			if (ret == FALSE)
				return EXIT_FAILURE;
			return EXIT_SUCCESS;
		}
		if (strcmp (argv[1], "getlcd") == 0) {
			ret = pmac_get_lcd_brightness (&brightness);
			if (ret == FALSE)
				return EXIT_FAILURE;
			/* we subtract 1 as 0 is backlight disable */
			printf ("%i", brightness - 1);
			return EXIT_SUCCESS;
		}
		if (strcmp (argv[1], "backlightoff") == 0) {
			ret = pmac_set_lcd_brightness (0);
			if (ret == FALSE)
				return EXIT_FAILURE;
			return EXIT_SUCCESS;
		}
	} else if (argc == 3) {
		if (strcmp (argv[1], "setlcd") == 0) {
			/* we add 1 as 0 is backlight disable */
			brightness = atoi (argv[2]) + 1;
			ret = pmac_set_lcd_brightness (brightness);
			if (ret == FALSE)
				return EXIT_FAILURE;
			return EXIT_SUCCESS;
		}
	}
	usage (argc, argv);
	return EXIT_SUCCESS;
}
