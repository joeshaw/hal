/***************************************************************************
 * CVSID: $Id$
 *
 * linux_common.c : Common functionality used by Linux specific parts
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#define _GNU_SOURCE 1		/* for strndup() */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "../logger.h"
#include "../device_store.h"
#include "../device_info.h"
#include "../hald.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxCommon Utility functions
 * @ingroup HalDaemonLinux
 * @brief Utility functions
 * @{
 */

/** Parse a double represented as a decimal number (base 10) in a string. 
 *
 *  @param  str                 String to parse
 *  @return                     Double; If there was an error parsing the
 *                              result is undefined.
 */
double
parse_double (const char *str)
{
	/** @todo Check error condition */
	return atof (str);
}

/** Parse an integer represented as a decimal number (base 10) in a string. 
 *
 *  @param  str                 String to parse
 *  @return                     Integer; If there was an error parsing the
 *                              result is undefined.
 */
dbus_int32_t
parse_dec (const char *str)
{
	dbus_int32_t value;
	value = strtol (str, NULL, 10);
	/** @todo Check error condition */
	return value;
}

/** Parse an integer represented as a hexa-decimal number (base 16) in
 *  a string.
 *
 *  @param  str                 String to parse
 *  @return                     Integer; If there was an error parsing the
 *                              result is undefined.
 */
dbus_int32_t
parse_hex (const char *str)
{
	dbus_int32_t value;
	value = strtol (str, NULL, 16);
	/** @todo Check error condition */
	return value;
}

/** Find an integer appearing right after a substring in a string.
 *
 *  The result is LONG_MAX if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @param  base                Base, e.g. decimal or hexadecimal, that
 *                              number appears in
 *  @return                     Number
 */
long int
find_num (char *pre, char *s, int base)
{
	char *where;
	int result;

	where = strstr (s, pre);
	if (where == NULL) {
		/*DIE(("Didn't find '%s' in '%s'", pre, s)); */
		return LONG_MAX;
	}
	where += strlen (pre);

	result = strtol (where, NULL, base);
	/** @todo Handle errors gracefully */
/*
    if( result==LONG_MIN || result==LONG_MAX )
        DIE(("Error parsing value for '%s' in '%s'", pre, s));
*/

	return result;
}

/** Find a floating point number appearing right after a substring in a string
 *  and return it as a double precision IEEE754 floating point number.
 *
 *  The result is undefined if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @return                     Number
 */
double
find_double (char *pre, char *s)
{
	char *where;
	double result;

	where = strstr (s, pre);
	/** @todo Handle errors gracefully */
	if (where == NULL)
		DIE (("Didn't find '%s' in '%s'", pre, s));
	where += strlen (pre);

	result = atof (where);

	return result;
}

/** Find a floating point number appearing right after a substring in a string
 *  and return it as a BCD encoded number with 2 digits of precision.
 *
 *  The result is undefined if the number isn't properly formatted or
 *  the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @return                     Number
 */
int
find_bcd2 (char *pre, char *s)
{
	int i;
	char c;
	int digit;
	int left, right, result;
	int len;
	char *str;
	dbus_bool_t passed_white_space;
	int num_prec;

	str = find_string (pre, s);
	if (str == NULL || strlen (str) == 0)
		return 0xffff;


	left = 0;
	len = strlen (str);
	passed_white_space = FALSE;
	for (i = 0; i < len && str[i] != '.'; i++) {
		if (isspace (str[i])) {
			if (passed_white_space)
				break;
			else
				continue;
		}
		passed_white_space = TRUE;
		left *= 16;
		c = str[i];
		digit = (int) (c - '0');
		left += digit;
	}
	i++;
	right = 0;
	num_prec = 0;
	for (; i < len; i++) {
		if (isspace (str[i]))
			break;
		if (num_prec == 2)	/* Only care about two digits 
					 * of precision */
			break;
		right *= 16;
		c = str[i];
		digit = (int) (c - '0');
		right += digit;
		num_prec++;
	}

	for (; num_prec < 2; num_prec++)
		right *= 16;

	result = left * 256 + (right & 255);
	return result;
}

/** Find a string appearing right after a substring in a string
 *  and return it. The string return is statically allocated and is 
 *  only valid until the next invocation of this function.
 *
 *  The result is undefined if the substring didn't exist in the given string.
 *
 *  @param  pre                 Substring preceding the value to parse
 *  @param  s                   String to analyze
 *  @return                     Number
 */
char *
find_string (char *pre, char *s)
{
	char *where;
	static char buf[256];
	char *p;

	where = strstr (s, pre);
	if (where == NULL)
		return NULL;
	where += strlen (pre);

	p = buf;
	while (*where != '\n' && *where != '\r') {
		char c = *where;

		/* ignoring char 63 fixes a problem with info from the 
		 * Lexar CF Reader
		 */
		if ((isalnum (c) || isspace (c) || ispunct (c)) && c != 63) {
			*p = c;
			++p;
		}

		++where;
	}
	*p = '\0';

	/* remove trailing white space */
	--p;
	while (isspace (*p)) {
		*p = '\0';
		--p;
	}

	return buf;
}

/** Read the first line of a file and return it.
 *
 *  @param  filename_format     Name of file, printf-style formatted
 *  @return                     Pointer to string or #NULL if the file could
 *                              not be opened. The result is only valid until
 *                              the next invocation of this function.
 */
char *
read_single_line (char *filename_format, ...)
{
	FILE *f;
	int i;
	int len;
	char filename[512];
	static char buf[512];
	va_list args;

	va_start (args, filename_format);
	vsnprintf (filename, 512, filename_format, args);
	va_end (args);

	f = fopen (filename, "rb");
	if (f == NULL)
		return NULL;

	if (fgets (buf, 512, f) == NULL) {
		fclose (f);
		return NULL;
	}

	len = strlen (buf);
	for (i = len - 1; i > 0; --i) {
		if (buf[i] == '\n' || buf[i] == '\r')
			buf[i] = '\0';
		else
			break;
	}

	fclose (f);
	return buf;
}

/** Given a path, /foo/bar/bat/foobar, return the last element, e.g.
 *  foobar.
 *
 *  @param  path                Path
 *  @return                     Pointer into given string
 */
const char *
get_last_element (const char *s)
{
	int len;
	const char *p;

	len = strlen (s);
	for (p = s + len - 1; p > s; --p) {
		if ((*p) == '/')
			return p + 1;
	}

	return s;
}

/* returns the path of the udevinfo program 
 *
 * @return                      path or NULL if udevinfo program is not found
 */
const char *
udevinfo_path (void)
{
	char *possible_paths[] = { "/sbin/udevinfo",
		"/usr/bin/udevinfo",
		"/usr/sbin/udevinfo",
		"/usr/local/sbin/udevinfo"
	};
	char *path = NULL;
	unsigned int i;
	struct stat s;
	for (i = 0; i < sizeof (possible_paths) / sizeof (char *); i++) {
		if (stat (possible_paths[i], &s) == 0
		    && S_ISREG (s.st_mode)) {
			path = possible_paths[i];
			break;
		}
	}
	return path;
}

/** This function takes a temporary device and renames it to a proper
 *  UDI using the supplied bus-specific #naming_func. After renaming
 *  the HAL daemon will locate a .fdi file and possibly merge information
 *  into the object.
 *
 *  This function handles the fact that identical devices (for
 *  instance two completely identical USB mice) gets their own unique
 *  device id by appending a trailing number after it.
 *
 *  You cannot rely on the HalDevice object you gave this function, since
 *  information may have been merged into an existing HalDevice object. Use
 *  ds_device_find() to get the corresponding HalDevice object.
 *
 *  The device _is not_ added to the GDL. You need to call ds_gdl_add()
 *  explicitly to do this.
 *
 *  @param  d                   HalDevice object
 *  @param  naming_func         Function to compute bus-specific UDI
 *  @param  namespace           Namespace of properties that must match,
 *                              e.g. "usb", "pci", in order to have matched
 *                              a device
 *  @return                     New UDI for the device
 *                              or #NULL if the device already existed.
 *                              In the event that the device already existed
 *                              the given HalDevice object is destroyed.
 */
char *
rename_and_merge (HalDevice * d,
		  ComputeFDI naming_func, const char *namespace)
{
	int append_num;
	char *computed_udi;
	HalDevice *computed_d;

	/* udi is a temporary udi */

	append_num = -1;
tryagain:
	/* compute the udi for the device */
	computed_udi = (*naming_func) (d, append_num);

	/* See if a device with the computed udi already exist. It can exist
	 * because the device-list is (can be) persistent across invocations 
	 * of hald.
	 *
	 * If it does exist, note that it's udi is computed from only the same 
	 * information as our just computed udi.. So if we match, and it's
	 * unplugged, it's the same device!
	 *
	 * (of course assuming that our udi computing algorithm actually works!
	 *  Which it might not, see discussions - but we can get close enough
	 *  for it to be practical)
	 */
	computed_d = hal_device_store_find (hald_get_gdl (), computed_udi);
	if (computed_d != NULL) {

		if ((!hal_device_has_property
		     (computed_d, "info.not_available"))
		    &&
		    (!hal_device_property_get_bool
		     (computed_d, "info.not_available"))) {
			/* Danger, Will Robinson! Danger!
			 *
			 * Ok, this means that either
			 *
			 * a) The user plugged in two instances of the kind of
			 *    of device; or
			 *
			 * b) The agent is invoked with --probe for the second
			 *    time during the life of the HAL daemon
			 *
			 * We want to support b) otherwise we end up adding a
			 * lot of devices which is a nuisance.. We also want to
			 * be able to do b) when developing HAL agents.
			 *
			 * So, therefore we check if the non-unplugged device 
			 * has the same bus information as our newly hotplugged
			 * one.
			 */
			if (hal_device_matches (computed_d, d, namespace)) {
				HAL_ERROR (("Found device already present "
					    "as '%s'!\n", computed_d->udi));
				hal_device_print (d);
				hal_device_print (computed_d);
				/* indeed a match, must be b) ;ignore device */
				hal_device_store_remove (hald_get_tdl (), d);
				g_object_unref (d);
				/* and return */
				return NULL;
			}
			
			/** Not a match, must be case a). Choose next 
			 *  computed_udi and try again! */
			append_num++;
			goto tryagain;
		} else {
			/* must be another instance of this type of device */
			append_num++;
			goto tryagain;
		}

		/* It did exist! Merge our properties from the probed device
		 * since some of the bus-specific properties are likely to be
		 * different 
		 *
		 * (user may have changed port, #Dev is different etc.)
		 *
		 * Note that the probed device only contain bus-specific
		 * properties - the other properties will remain..
		 */
		hal_device_merge (computed_d, d);

		/* Set that we are back in business! */
		hal_device_property_set_bool (computed_d, "info.not_available",
					      FALSE);

		HAL_INFO (("Device %s is plugged in again",
			   computed_d->udi));

	} else {
		/* Device is not in list... */

		/* assign the computed device name */
		HAL_INFO ((" ##### computed_udi=%s", computed_udi));
		hal_device_set_udi (d, computed_udi);
		hal_device_property_set_string (d, "info.udi", computed_udi);

		/* Search for device information file and attempt merge */
		if (di_search_and_merge (d)) {
			HAL_INFO (("Found a .fdi file for %s", d->udi));
		}

	}

	return computed_udi;
}

/** Given a sysfs-path for a device, this functions finds the sysfs
 *  path representing the parent of the given device by truncation.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @return                     Path for parent; must be freed by caller
 */
char *
get_parent_sysfs_path (const char *path)
{
	int i;
	int len;
	char *parent_path;

	/* Find parent device by truncating our own path */
	parent_path = strndup (path, SYSFS_PATH_MAX);
	if (parent_path == NULL)
		DIE (("No memory"));
	len = strlen (parent_path);
	for (i = len - 1; parent_path[i] != '/'; --i) {
		parent_path[i] = '\0';
	}
	parent_path[i] = '\0';

	return parent_path;
}

/** Set the physical device for a device.
 *
 *  This function visits all parent devices and sets the property
 *  info.physical_device to the first parent device that doesn't have the
 *  info.physical_device property set.
 *
 *  @param  device              HalDevice to process
 */
void
find_and_set_physical_device (HalDevice * device)
{
	HalDevice *d;
	HalDevice *parent;
	const char *parent_udi;

	d = device;
	do {
		parent_udi = hal_device_property_get_string (d,
							      "info.parent");
		if (parent_udi == NULL) {
			HAL_ERROR (("Error finding parent for %s\n",
				    d->udi));
			return;
		}

		parent = hal_device_store_find (hald_get_gdl (), parent_udi);
		if (parent == NULL) {
			HAL_ERROR (("Error resolving UDI %s\n",
				    parent_udi));
			return;
		}

		if (!hal_device_has_property (parent,
					       "info.physical_device")) {
			hal_device_property_set_string (device,
							 "info.physical_device",
							 parent_udi);
			return;
		}

		d = parent;
	}
	while (TRUE);
}

/** Utility function to retrieve major and minor number for a class device.
 *
 *  The class device must have a dev file in the #sysfs_path directory and
 *  it must be of the form %d:%d.
 *  
 *  @param  sysfs_path          Path to directory in sysfs filesystem of
 *                              class device
 *  @param  major               Major device number will be stored here
 *  @param  minor               Minor device number will be stored here
 *  @return                     TRUE on success, otherwise FALSE
 */
dbus_bool_t
class_device_get_major_minor (const char *sysfs_path, int *major, int *minor)
{
	struct sysfs_attribute *attr;
	char attr_path[SYSFS_PATH_MAX];

	snprintf (attr_path, SYSFS_PATH_MAX, "%s/dev", sysfs_path);
	attr = sysfs_open_attribute (attr_path);
	if (sysfs_read_attribute (attr) >= 0) {
		if (sscanf (attr->value, "%d:%d", major, minor) != 2) {
			HAL_WARNING (("Could not parse '%s'", attr->value));
			sysfs_close_attribute (attr);
			return TRUE;
		}
		sysfs_close_attribute (attr);
		return TRUE;
	}

	return FALSE;			
}

/** Get the name of the special device file given the sysfs path for a class
 *  device.
 *
 *  @param  sysfs_path          Path to class device in sysfs
 *  @param  dev_file            Where the special device file name should be
 *                              stored
 *  @param  dev_file_length     Size of dev_file character array
 */
dbus_bool_t
class_device_get_device_file (const char *sysfs_path, 
			      char *dev_file, int dev_file_length)
{
	int i;
	unsigned int sysfs_mount_path_len;
	char sysfs_path_trunc[SYSFS_NAME_LEN];
	char *udev_argv[7] = { (char *) udevinfo_path (), 
			       "-r", "-q", "name", "-p",
			       sysfs_path_trunc, NULL };
	char *udev_stdout;
	char *udev_stderr;
	int udev_exitcode;

	/* compute truncated sysfs path - udevinfo doesn't want the
	 * the sysfs_mount_path (e.g. /udev or /dev) prefix */
	sysfs_mount_path_len = strlen (sysfs_mount_path);
	if (strlen (sysfs_path) > sysfs_mount_path_len) {
		strncpy (sysfs_path_trunc, sysfs_path + sysfs_mount_path_len,
			 SYSFS_NAME_LEN);
	}

	HAL_INFO (("*** sysfs_path_trunc = '%s'", sysfs_path_trunc));

	/* Now invoke udevinfo */
	if (udev_argv[0] == NULL || g_spawn_sync ("/",
						  udev_argv,
						  NULL,
						  0,
						  NULL,
						  NULL,
						  &udev_stdout,
						  &udev_stderr,
						  &udev_exitcode,
						  NULL) != TRUE) {
		HAL_ERROR (("Couldn't invoke %s", udevinfo_path ()));
		return FALSE;
	}

	if (udev_exitcode != 0) {
		HAL_ERROR (("%s returned %d", udevinfo_path (),
			    udev_exitcode));
		return FALSE;
	}

	/* sanitize string returned by udev */
	for (i = 0; udev_stdout[i] != 0; i++) {
		if (udev_stdout[i] == '\r' || udev_stdout[i] == '\n') {
			udev_stdout[i] = 0;
			break;
		}
	}

	HAL_INFO (("got device file %s for %s", udev_stdout, sysfs_path));

	strncpy (dev_file, udev_stdout, dev_file_length);
	return TRUE;
}


/* Entry in bandaid driver database */
struct driver_entry_s {
	char driver_name[SYSFS_NAME_LEN];
				       /**< Name of driver, e.g. 8139too */
	char device_path[SYSFS_PATH_MAX];
				       /**< Sysfs path */
	struct driver_entry_s *next;   /**< Pointer to next element or #NULL
                                        *   if the last element */
};

/** Head of linked list of #driver_entry_s structs */
static struct driver_entry_s *drivers_table_head = NULL;

/** Add an entry to the bandaid driver database.
 *
 *  @param  driver_name         Name of the driver
 *  @param  device_path         Path to device, must start with /sys/devices
 */
static void
drivers_add_entry (const char *driver_name, const char *device_path)
{
	struct driver_entry_s *entry;

	entry = malloc (sizeof (struct driver_entry_s));
	if (entry == NULL)
		DIE (("Out of memory"));
	strncpy (entry->driver_name, driver_name, SYSFS_NAME_LEN);
	strncpy (entry->device_path, device_path, SYSFS_PATH_MAX);
	entry->next = drivers_table_head;
	drivers_table_head = entry;
}

/** Given a device path under /sys/devices, lookup the driver. You need
 *  to have called #drivers_collect() on the bus-type before hand.
 *
 *  @param  device_path         sysfs path to device
 *  @return                     Driver name or #NULL if no driver is bound
 *                              to that sysfs device
 */
const char *
drivers_lookup (const char *device_path)
{
	struct driver_entry_s *i;

	for (i = drivers_table_head; i != NULL; i = i->next) {
		if (strcmp (device_path, i->device_path) == 0)
			return i->driver_name;
	}
	return NULL;
}

/** Collect all drivers being used on a bus. This is only bandaid until
 *  sysutils fill in the driver_name in sysfs_device.
 *
 *  @param  bus_name            Name of bus, e.g. pci, usb
 */
void
drivers_collect (const char *bus_name)
{
	char path[SYSFS_PATH_MAX];
	struct sysfs_directory *current;
	struct sysfs_link *current2;
	struct sysfs_directory *dir;
	struct sysfs_directory *dir2;

	/* traverse /sys/bus/<bus>/drivers */
	snprintf (path, SYSFS_PATH_MAX, "%s/bus/%s/drivers",
		  sysfs_mount_path, bus_name);
	dir = sysfs_open_directory (path);
	if (dir == NULL) {
		HAL_WARNING (("Error opening sysfs directory at %s\n",
			      path));
		goto out;
	}
	if (sysfs_read_directory (dir) != 0) {
		HAL_WARNING (("Error reading sysfs directory at %s\n",
			      path));
		goto out;
	}
	if (dir->subdirs != NULL) {
		dlist_for_each_data (dir->subdirs, current,
				     struct sysfs_directory) {
			/*printf("name=%s\n", current->name); */

			dir2 = sysfs_open_directory (current->path);
			if (dir2 == NULL)
				DIE (("Error opening sysfs directory "
				      "at %s\n", current->path));
			if (sysfs_read_directory (dir2) != 0)
				DIE (("Error reading sysfs directory "
				      "at %s\n", current->path));

			if (dir2->links != NULL) {
				dlist_for_each_data (dir2->links, current2,
						     struct sysfs_link) {
					/*printf("link=%s\n",current2->target);
					 */
					drivers_add_entry (current->name,
							   current2->
							   target);
				}
				sysfs_close_directory (dir2);
			}
		}
	}
out:
	if (dir != NULL)
		sysfs_close_directory (dir);
}

/** @} */
