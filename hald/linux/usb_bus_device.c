/***************************************************************************
 * CVSID: $Id$
 *
 * USB bus device
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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "bus_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxUsb USB
 * @ingroup HalDaemonLinux
 * @brief USB
 * @{
 */

/** Pointer to where the usb.ids file is loaded */
static char *usb_ids = NULL;

/** Length of data store at at usb_ids */
static unsigned int usb_ids_len;

/** Iterator position into usb_ids */
static unsigned int usb_ids_iter_pos;

/** Initialize the usb.ids line iterator to the beginning of the file */
static void
usb_ids_line_iter_init ()
{
	usb_ids_iter_pos = 0;
}

/** Maximum length of lines in usb.ids */
#define USB_IDS_MAX_LINE_LEN 512

/** Get the next line from usb.ids
 *
 *  @param  line_len            Pointer to where number of bytes in line will
 *                              be stored
 *  @return                     Pointer to the line; only valid until the
 *                              next invocation of this function
 */
static char *
usb_ids_line_iter_get_line (unsigned int *line_len)
{
	unsigned int i;
	static char line[USB_IDS_MAX_LINE_LEN];

	for (i = 0;
	     usb_ids_iter_pos < usb_ids_len &&
	     i < USB_IDS_MAX_LINE_LEN - 1 &&
	     usb_ids[usb_ids_iter_pos] != '\n'; i++, usb_ids_iter_pos++) {
		line[i] = usb_ids[usb_ids_iter_pos];
	}

	line[i] = '\0';
	if (line_len != NULL)
		*line_len = i;

	usb_ids_iter_pos++;

	return line;
}

/** See if there are more lines to process in usb.ids
 *
 *  @return                     #TRUE iff there are more lines to process
 */
static dbus_bool_t
usb_ids_line_iter_has_more ()
{
	return usb_ids_iter_pos < usb_ids_len;
}

/** Find the names for a USB device.
 *
 *  The pointers returned are only valid until the next invocation of this
 *  function.
 *
 *  @param  vendor_id           USB vendor id or 0 if unknown
 *  @param  product_id          USB product id or 0 if unknown
 *  @param  vendor_name         Set to pointer of result or #NULL
 *  @param  product_name        Set to pointer of result or #NULL
 */
static void
usb_ids_find (int vendor_id, int product_id,
	      char **vendor_name, char **product_name)
{
	char *line;
	unsigned int i;
	unsigned int line_len;
	unsigned int num_tabs;
	char rep_vi[8];
	char rep_pi[8];
	static char store_vn[USB_IDS_MAX_LINE_LEN];
	static char store_pn[USB_IDS_MAX_LINE_LEN];
	dbus_bool_t vendor_matched = FALSE;

	snprintf (rep_vi, 8, "%04x", vendor_id);
	snprintf (rep_pi, 8, "%04x", product_id);

	*vendor_name = NULL;
	*product_name = NULL;

	for (usb_ids_line_iter_init (); usb_ids_line_iter_has_more ();) {
		line = usb_ids_line_iter_get_line (&line_len);

		/* skip lines with no content */
		if (line_len < 4)
			continue;

		/* skip comments */
		if (line[0] == '#')
			continue;

		/* count number of tabs */
		num_tabs = 0;
		for (i = 0; i < line_len; i++) {
			if (line[i] != '\t')
				break;
			num_tabs++;
		}

		switch (num_tabs) {
		case 0:
			/* vendor names */
			vendor_matched = FALSE;

			/* check vendor_id */
			if (vendor_id != 0) {
				if (memcmp (line, rep_vi, 4) == 0) {
					/* found it */
					vendor_matched = TRUE;

					for (i = 4; i < line_len; i++) {
						if (!isspace (line[i]))
							break;
					}
					strncpy (store_vn, line + i,
						 USB_IDS_MAX_LINE_LEN);
					*vendor_name = store_vn;
				}
			}
			break;

		case 1:
			/* product names */
			if (!vendor_matched)
				continue;

			/* check product_id */
			if (product_id != 0) {
				if (memcmp (line + 1, rep_pi, 4) == 0) {
					/* found it */
					for (i = 5; i < line_len; i++) {
						if (!isspace (line[i]))
							break;
					}
					strncpy (store_pn, line + i,
						 USB_IDS_MAX_LINE_LEN);
					*product_name = store_pn;

					/* no need to continue the search */
					return;
				}
			}
			break;

		default:
			break;
		}

	}
}

/** Load the USB database used for mapping vendor, product, subsys_vendor
 *  and subsys_product numbers into names.
 *
 *  @param  path                Path of the usb.ids file, e.g. 
 *                              /usr/share/hwdata/usb.ids
 *  @return                     #TRUE if the file was succesfully loaded
 */
static dbus_bool_t
usb_ids_load (const char *path)
{
	FILE *fp;
	unsigned int num_read;

	fp = fopen (path, "r");
	if (fp == NULL) {
		printf ("couldn't open USB database at %s,", path);
		return FALSE;
	}

	fseek (fp, 0, SEEK_END);
	usb_ids_len = ftell (fp);
	fseek (fp, 0, SEEK_SET);

	usb_ids = malloc (usb_ids_len);
	if (usb_ids == NULL) {
		printf
		    ("Couldn't allocate %d bytes for USB database file\n",
		     usb_ids_len);
		return FALSE;
	}

	num_read = fread (usb_ids, sizeof (char), usb_ids_len, fp);
	if (usb_ids_len != num_read) {
		printf ("Error loading USB database file\n");
		free (usb_ids);
		usb_ids = NULL;
		return FALSE;
	}

	return TRUE;
}

/** Free resources used by to store the USB database
 *
 *  @param                      #FALSE if the USB database wasn't loaded
 */
static dbus_bool_t
usb_ids_free ()
{
	if (usb_ids != NULL) {
		free (usb_ids);
		usb_ids = NULL;
		return TRUE;
	}
	return FALSE;
}


/** Key information about USB devices from /proc that is not available 
 *  in sysfs
 */
typedef struct usb_proc_info_s {
	int t_bus;	     /**< Bus number */
	int t_level;	     /**< Level in topology (depth) */
	int t_parent;	     /**< Parent DeviceNumber */
	int t_port;	     /**< Port on Parent for this device */
	int t_count;	     /**< Count of devices at this level */
	int t_device;	     /**< DeviceNumber */
	int t_speed_bcd;     /**< Device Speed in Mbps, encoded as BCD */
	int t_max_children;  /**< Maximum number of children */
	int d_version_bcd;   /**< USB version, encoded in BCD */

	struct usb_proc_info_s *next;
				  /**< next element or #NULL if last */
} usb_proc_info;

/** First element in usb proc linked list */
static usb_proc_info *usb_proc_head = NULL;

/** Unique device id of the device we are working on */
static usb_proc_info *usb_proc_cur_info = NULL;

/** Find the USB virtual root hub device for a USB bus.
 *
 *  @param  bus_number          USB bus number
 *  @return                     The #usb_proc_info structure with information
 *                              retrieved from /proc or #NULL if not found
 */
static usb_proc_info *
usb_proc_find_virtual_hub (int bus_number)
{
	usb_proc_info *i;
	for (i = usb_proc_head; i != NULL; i = i->next) {
		if (i->t_bus == bus_number && i->t_level == 0)
			return i;
	}

	return NULL;
}


/** Find a child of a USB virtual root hub device for a USB bus.
 *
 *  @param  bus_number          USB bus number
 *  @param  port_number         The port number, starting from 1
 *  @return                     The #usb_proc_info structure with information
 *                              retrieved from /proc or #NULL if not found
 */
static usb_proc_info *
usb_proc_find_virtual_hub_child (int bus_number, int port_number)
{
	usb_proc_info *i;
	for (i = usb_proc_head; i != NULL; i = i->next) {
		/* Note that /proc counts port starting from zero */
		if (i->t_bus == bus_number && i->t_level == 1 &&
		    i->t_port == port_number - 1)
			return i;
	}

	return NULL;
}

/** Find a child of a given hub device given a bus and port number
 *
 *  @param  bus_number           USB bus number
 *  @param  port_number          The port number, starting from 1
 *  @param  parent_device_number The Linux device number
 *  @return                      The #usb_proc_info structure with information
 *                               retrieved from /proc or #NULL if not found
 */
static usb_proc_info *
usb_proc_find_on_hub (int bus_number, int port_number,
		      int parent_device_number)
{
	usb_proc_info *i;
	for (i = usb_proc_head; i != NULL; i = i->next) {
		/* Note that /proc counts port starting from zero */
		if (i->t_bus == bus_number && i->t_port == port_number - 1
		    && i->t_parent == parent_device_number)
			return i;
	}

	return NULL;
}


/** Parse the topology field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/usb/devices starting
 *                              with "T:"
 */
static void
usb_proc_handle_topology (usb_proc_info * info, char *s)
{
	info->t_bus = find_num ("Bus=", s, 10);
	info->t_level = find_num ("Lev=", s, 10);
	info->t_parent = find_num ("Prnt=", s, 10);
	info->t_port = find_num ("Port=", s, 10);
	info->t_count = find_num ("Cnt=", s, 10);
	info->t_device = find_num ("Dev#=", s, 10);
	info->t_speed_bcd = find_bcd2 ("Spd=", s);
	info->t_max_children = find_num ("MxCh=", s, 10);
}

/** Parse the device descriptor field
 *
 *  @param  info                Structure to put information into
 *  @param  s                   Line from /proc/bus/usb/devices starting
 *                              with "D:"
 */
static void
usb_proc_handle_device_info (usb_proc_info * info, char *s)
{
	info->d_version_bcd = find_bcd2 ("Ver=", s);
}


/** Called when an entry from /proc/bus/usb/devices have been parsed.
 *
 *  @param  info                Structure representing the entry
 */
static void
usb_proc_device_done (usb_proc_info * info)
{
	info->next = usb_proc_head;
	usb_proc_head = info;
}



/** Parse a line from /proc/bus/usb/devices
 *
 *  @param  s                   Line from /proc/bus/usb/devices
 */
static void
usb_proc_parse_line (char *s)
{
	switch (s[0]) {
	case 'T':  /* topology; always present, indicates a new device */
		if (usb_proc_cur_info != NULL) {
			// beginning of a new device, done with current
			usb_proc_device_done (usb_proc_cur_info);
		}

		usb_proc_cur_info = malloc (sizeof (usb_proc_info));

		if (usb_proc_cur_info == NULL)
			DIE (("Cannot allocated memory"));

		usb_proc_handle_topology (usb_proc_cur_info, s);
		break;

	case 'B':		/* bandwidth */
		break;

	case 'D':		/* device information */
		usb_proc_handle_device_info (usb_proc_cur_info, s);
		break;

	case 'P':		/* more device information */
		break;

	case 'S':		/* device string information */
		break;

	case 'C':		/* config descriptor info */
		break;

	case 'I':		/* interface descriptor info */
		break;

	case 'E':		/* endpoint descriptor info */
		break;

	default:
		break;
	}
}

/** Parse /proc/bus/usb/devices
 */
static void
usb_proc_parse ()
{
	FILE *f;
	char buf[256];

	/* We may be called multiple times; in fact we are called on every
	 * hotplug.. so clean up old info
	 */
	if (usb_proc_head != NULL) {
		usb_proc_info *i;
		usb_proc_info *next;

		for (i = usb_proc_head; i != NULL; i = next) {
			next = i->next;
			free (i);
		}
		usb_proc_head = NULL;
	}

	usb_proc_cur_info = NULL;

	f = fopen ("/proc/bus/usb/devices", "r");
	if (f == NULL)
		f = fopen
		    ("/proc/bus/usb/devices_please-use-sysfs-instead",
		     "r");
	if (f == NULL) {
		/*DIE (("Couldn't open /proc/bus/usb/devices"));*/
		return;
	}

	while (!feof (f)) {
		fgets (buf, 256, f);
		usb_proc_parse_line (buf);
	}
	usb_proc_device_done (usb_proc_cur_info);

	{
		usb_proc_info *i;
		for (i = usb_proc_head; i != NULL; i = i->next) {
			printf ("/p/b/u/d entry\n");
			printf ("  bus               %d\n", i->t_bus);
			printf ("  level             %d\n", i->t_level);
			printf ("  parent            %d\n", i->t_parent);
			printf ("  port              %d\n", i->t_port);
			printf ("  count             %d\n", i->t_count);
			printf ("  device            %d\n", i->t_device);
			printf ("  speed_bcd         %x.%x (0x%06x)\n",
				i->t_speed_bcd >> 8, i->t_speed_bcd & 0xff,
				i->t_speed_bcd);
			printf ("  max_children      %d\n",
				i->t_max_children);
			printf ("  version_bcd       %x.%x (0x%06x)\n",
				i->d_version_bcd >> 8,
				i->d_version_bcd & 0xff, i->d_version_bcd);
			printf ("\n");
		}
	}
}

/** Set capabilities from interface and/or device class.  This is a
 *  function from hell, maybe some searchable data-structure would be
 *  better...
 *
 *  @param  d                   The HalDevice to set the caps
 *  @param  if_class            Interface class
 *  @param  if_sub_class        Interface sub class
 *  @param if_proto Interface protocol */
static void
usb_add_caps_from_class (HalDevice * d,
			 int if_class, int if_sub_class, int if_proto)
{
	char *cat = NULL;

	switch (if_class) {
	case 0x01:
		cat = "multimedia.audio";
		hal_device_add_capability (d, "multimedia.audio");
		break;
	case 0x02:
		if (if_sub_class == 0x06) {
			cat = "net";
			hal_device_add_capability (d, "net");
			hal_device_add_capability (d, "net.ethernet");
		} else if (if_sub_class == 0x02 && if_proto == 0x01) {
			cat = "modem";
			hal_device_add_capability (d, "modem");
		}
		break;
	case 0x03:
		cat = "input";
		hal_device_add_capability (d, "input");
		if (if_sub_class == 0x00 || if_sub_class == 0x01) {
			if (if_proto == 0x01) {
				cat = "input.keyboard";
				hal_device_add_capability (d, "input.keyboard");
			} else if (if_proto == 0x02) {
				cat = "input.mouse";
				hal_device_add_capability (d, "input.mouse");
			}
		}
		break;
	case 0x04:
		break;
	case 0x05:
		break;
	case 0x06:
		break;
	case 0x07:
		cat = "printer";
		hal_device_add_capability (d, "printer");
		break;
	case 0x08:
		cat = "storage_controller";
		hal_device_add_capability (d, "storage_controller");
		break;
	case 0x09:
		cat = "hub";
		hal_device_add_capability (d, "hub");
		break;
	case 0x0a:
		break;
	case 0xe0:
		if (if_sub_class == 0x01 && if_proto == 0x01) {
			cat = "bluetooth_adaptor";
			hal_device_add_capability (d, "bluetooth_adaptor");
		}
		break;
	}

	if (cat != NULL)
		hal_device_property_set_string (d, "info.category", cat);
}



/** Init function for USB handling
 *
 */
static void
usb_device_init ()
{

	/* get all drivers under /sys/bus/usb/drivers */
	drivers_collect ("usb");

	/* Load /usr/share/hwdata/usb.ids */
	usb_ids_load (HWDATA_DIR "/usb.ids");

	/* Parse /proc/bus/usb/devices */

	usb_proc_parse ();
}


/** Shutdown function for USB handling
 *
 */
static void
usb_device_shutdown ()
{
	usb_ids_free ();
}

/** Specialised accept function since both USB devices and USB interfaces
 *  share the same bus name
 *
 *  @param  self                Pointer to class members
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 *  @param  is_probing          Set to TRUE only on initial detection
 */
static dbus_bool_t
usb_device_accept (BusDeviceHandler *self, const char *path, 
		   struct sysfs_device *device, dbus_bool_t is_probing)
{
	unsigned int i;

	if (strcmp (device->bus, "usb") != 0)
		return FALSE;

	/* only USB interfaces got a : in the bus_id */
	for (i = 0; device->bus_id[i] != 0; i++) {
		if (device->bus_id[i] == ':') {
			return FALSE;
		}
	}

	return TRUE;
}

static char *
usb_device_compute_udi (HalDevice *d, int append_num)
{
	const char *serial;
	const char *format;
	static char buf[256];

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/usb_%x_%x_%x_%d_%s";
	else
		format =
		    "/org/freedesktop/Hal/devices/usb_%x_%x_%x_%d_%s-%d";

	if (hal_device_has_property (d, "usb.serial"))
		serial = hal_device_property_get_string (d, "usb.serial");
	else
		serial = "noserial";

	snprintf (buf, 256, format,
		  hal_device_property_get_int (d, "usb.vendor_id"),
		  hal_device_property_get_int (d, "usb.product_id"),
		  hal_device_property_get_int (d, "usb.device_revision_bcd"),
		  hal_device_property_get_int (d, "usb.cfg_value"),
		  serial, append_num);

	return buf;
}


static void 
usb_merge_info_from_proc (HalDevice* d)
{
	int bus_number;
	const char *bus_id;
	usb_proc_info *proc_info;
	const char *parent_udi;
	HalDevice* parent;

	parent_udi = hal_device_property_get_string (d, "info.parent");
	parent = hal_device_store_find (hald_get_gdl (), parent_udi);

	/* Merge information from /proc/bus/usb/devices */
	proc_info = NULL;
	usb_proc_parse ();

	bus_id = get_last_element (hal_device_property_get_string (d, "linux.sysfs_path"));

	if (sscanf (bus_id, "usb%d", &bus_number) == 1) {
		/* Is of form "usb%d" which means that this is a USB virtual 
		 * root hub, cf. drivers/usb/hcd.c in kernel 2.6
		 */
		hal_device_property_set_int (d, "usb.bus_number", bus_number);

		proc_info = usb_proc_find_virtual_hub (bus_number);
	} else {
		int i;
		int len;
		int digit;
		int port_number;
		/* Not a root hub; According to the Linux kernel sources,
		 * the name is of the form
		 *
		 *  "%d-%s[.%d]"
		 *
		 * where the first number is the bus-number, the middle string 
		 * is the parent device and the last, optional, number is the 
		 * port number in the event that the USB device is a hub.
		 */

		len = strlen (bus_id);

		/* the first part is easy */
		bus_number = atoi (bus_id);

		hal_device_property_set_int (d, "usb.bus_number", bus_number);

		/* The naming convention also guarantees that
		 *
		 *   device is on a (non-virtual) hub    
		 *
		 *            IF AND ONLY IF  
		 *
		 *   the bus_id contains a "."
		 */
		for (i = 0; i < len; i++) {
			if (bus_id[i] == '.')
				break;
		}

		if (i == len) {
			/* Not on a hub; this means we must be a child of the 
			 * root hub... Thus the name must is of the 
			 * form "%d-%d"
			 */
			if (sscanf (bus_id, "%d-%d",
				    &bus_number, &port_number) == 2) {

				proc_info =
				    usb_proc_find_virtual_hub_child
				    (bus_number, port_number);
				hal_device_property_set_int (d, "usb.port_number",
						     port_number);
			}
		} else {
			int parent_device_number;

			/* On a hub */

			/* This is quite a hack */
			port_number = 0;
			for (i = len - 1; i > 0 && isdigit (bus_id[i]);
			     --i) {
				digit = (int) (bus_id[i] - '0');
				port_number *= 10;
				port_number += digit;
			}

			hal_device_property_set_int (d, "usb.port_number",
					     port_number);

			/* Ok, got the port number and bus number; this is 
			 * not quite enough though.. We take the 
			 * usb.linux.device_number from our parent and then
			 *  we are set.. */
			if (parent == NULL) {
				HAL_WARNING (("USB device is on a hub but "
					      "no parent??"));
				/* have to give up then */
				proc_info = NULL;
			} else {
				parent_device_number =
				    hal_device_property_get_int (
					    parent, "usb.linux.device_number");
				//printf("parent_device_number = %d\n", parent_device_number);
				proc_info =
				    usb_proc_find_on_hub (bus_number,
							  port_number,
							 parent_device_number);
			}

		}
	}


	if (proc_info != NULL) {
		char kernel_path[32 + 1];

		hal_device_property_set_int (d, "usb.level_number",
				     proc_info->t_level);
		hal_device_property_set_int (d, "usb.linux.device_number",
				     proc_info->t_device);
		hal_device_property_set_int (d, "usb.linux.parent_number",
				     proc_info->t_device);
		hal_device_property_set_int (d, "usb.num_ports",
				     proc_info->t_max_children);
		hal_device_property_set_int (d, "usb.speed_bcd",
				     proc_info->t_speed_bcd);
		hal_device_property_set_int (d, "usb.version_bcd",
				     proc_info->d_version_bcd);

		/* Ok, now compute the unique name that the kernel sometimes 
		 * use to refer to the device; it's #usb_make_path() as 
		 * defined in include/linux/usb.h
		 */
		if (proc_info->t_level == 0) {
			snprintf (kernel_path, 32, "usb-%s",
				  hal_device_property_get_string (d,
							  "usb.serial"));
			hal_device_property_set_string (d, "linux.kernel_devname",
						kernel_path);
		} else {
			if (parent != NULL) {
				if (proc_info->t_level == 1) {
					snprintf (kernel_path, 32, "%s-%d",
						  hal_device_property_get_string
						  (parent,
						   "linux.kernel_devname"),
						  hal_device_property_get_int (
							  d,
							  "usb.port_number"));
				} else {
					snprintf (kernel_path, 32, "%s.%d",
						  hal_device_property_get_string
						  (parent,
						   "linux.kernel_devname"),
						  hal_device_property_get_int (
							  d,
							  "usb.port_number"));
				}
				hal_device_property_set_string (d,
							"linux.kernel_devname",
							kernel_path);
			}
		}

	}

}


static void 
usb_device_post_process (BusDeviceHandler *self,
			 HalDevice *d,
			 const char *sysfs_path,
			 struct sysfs_device *device)
{
	int i;
	char attr_name[SYSFS_NAME_LEN];
	int vendor_id = 0;
	int product_id = 0;
	char *vendor_name;
	char *product_name;
	char *vendor_name_kernel = NULL;
	char *product_name_kernel = NULL;
	char numeric_name[32];
	struct sysfs_attribute *cur;
	int len;

	dlist_for_each_data (sysfs_get_device_attributes (device), cur,
			     struct sysfs_attribute) {

		if (sysfs_get_name_from_path (cur->path,
					      attr_name,
					      SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; i >= 0 && isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		/*printf("attr_name=%s -> '%s'\n", attr_name, cur->value); */

		if (strcmp (attr_name, "idProduct") == 0)
			product_id = parse_hex (cur->value);
		else if (strcmp (attr_name, "idVendor") == 0)
			vendor_id = parse_hex (cur->value);
		else if (strcmp (attr_name, "bcdDevice") == 0)
			hal_device_property_set_int (d, "usb.device_revision_bcd",
					     parse_hex (cur->value));
		else if (strcmp (attr_name, "bMaxPower") == 0)
			hal_device_property_set_int (d, "usb.max_power",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "serial") == 0
			 && strlen (cur->value) > 0)
			hal_device_property_set_string (d, "usb.serial",
						cur->value);
		else if (strcmp (attr_name, "bmAttributes") == 0) {
			int bmAttributes = parse_hex (cur->value);

			/* USB_CONFIG_ATT_SELFPOWER */
			hal_device_property_set_bool (d, "usb.is_self_powered",
					      (bmAttributes & 0x40) != 0);
			hal_device_property_set_bool (d, "usb.can_wake_up",
					      (bmAttributes & 0x20) != 0);
		}
/*
        else if( strcmp(attr_name, "speed")==0 )
            hal_device_set_property_double(d, "usb.speed", 
                                           parse_double(cur->value));
*/

		else if (strcmp (attr_name, "manufacturer") == 0)
			vendor_name_kernel = cur->value;
		else if (strcmp (attr_name, "product") == 0)
			product_name_kernel = cur->value;
		else if (strcmp (attr_name, "bDeviceClass") == 0)
			hal_device_property_set_int (d, "usb.device_class",
					     parse_hex (cur->value));
		else if (strcmp (attr_name, "bDeviceSubClass") == 0)
			hal_device_property_set_int (d, "usb.device_subclass",
					     parse_hex (cur->value));
		else if (strcmp (attr_name, "bDeviceProtocol") == 0)
			hal_device_property_set_int (d, "usb.device_protocol",
					     parse_hex (cur->value));

		else if (strcmp (attr_name, "bNumConfigurations") == 0)
			hal_device_property_set_int (d, "usb.num_configurations",
					     parse_dec (cur->value));
		else if (strcmp (attr_name, "bConfigurationValue") == 0)
			hal_device_property_set_int (d, "usb.configuration_value",
					     parse_dec (cur->value));

		else if (strcmp (attr_name, "bNumInterfaces") == 0)
			hal_device_property_set_int (d, "usb.num_interfaces",
					     parse_dec (cur->value));

	}			/* for all attributes */

	hal_device_property_set_int (d, "usb.product_id", product_id);
	hal_device_property_set_int (d, "usb.vendor_id", vendor_id);

	/* Lookup names in usb.ids; these may override what the kernel told
	 * us, but, hey, it's only a name; it's not something we are going
	 * to match a device on... We prefer names from usb.ids as the kernel
	 * name sometimes is just a hexnumber :-/
	 *
	 * Also provide best guess on name, Product and Vendor properties;
	 * these can both be overridden in .fdi files.
	 */
	usb_ids_find (vendor_id, product_id, &vendor_name, &product_name);
	if (vendor_name != NULL) {
		hal_device_property_set_string (d, "usb.vendor", vendor_name);
		hal_device_property_set_string (d, "info.vendor", vendor_name);
	} else if (vendor_name_kernel != NULL) {
		/* fallback on name supplied from kernel */
		hal_device_property_set_string (d, "usb.vendor",
					vendor_name_kernel);
		hal_device_property_set_string (d, "info.vendor",
					vendor_name_kernel);
	} else {
		/* last resort; use numeric name */
		snprintf (numeric_name, sizeof(numeric_name), "Unknown (0x%04x)", vendor_id);
		hal_device_property_set_string (d, "usb.vendor", numeric_name);
		hal_device_property_set_string (d, "info.vendor", numeric_name);
	}

	if (product_name != NULL) {
		hal_device_property_set_string (d, "usb.product", product_name);
		hal_device_property_set_string (d, "info.product", product_name);
	} else if (product_name_kernel != NULL) {
		/* name supplied from kernel (if available) */
		hal_device_property_set_string (d, "usb.product",
					product_name_kernel);
		hal_device_property_set_string (d, "info.product",
					product_name_kernel);
	} else {
		/* last resort; use numeric name */
		snprintf (numeric_name, sizeof(numeric_name), "Unknown (0x%04x)",
			  product_id);
		hal_device_property_set_string (d, "usb.product", numeric_name);
		hal_device_property_set_string (d, "info.product", numeric_name);
	}


	/* Check device class */
	usb_add_caps_from_class (d, hal_device_property_get_int (d, "usb.device_class"),
				 hal_device_property_get_int (d, "usb.device_subclass"),
				 hal_device_property_get_int (d, "usb.device_protocol"));

	usb_merge_info_from_proc (d);
}


/** Method specialisations for bustype usb */
BusDeviceHandler usb_bus_handler = {
	usb_device_init,           /**< init function */
	bus_device_detection_done, /**< detection is done */
	usb_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	usb_device_accept,         /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	usb_device_compute_udi,    /**< UDI computing function */
	usb_device_post_process,   /**< add more properties */
	"usb",                     /**< sysfs bus name */
	"usb"                      /**< namespace */
};


/** @} */
