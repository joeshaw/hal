/***************************************************************************
 * CVSID: $Id$
 *
 * PCI bus devices
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
#include "bus_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxPci PCI
 * @ingroup HalDaemonLinux
 * @brief PCI
 * @{
 */


/** Pointer to where the pci.ids file is loaded */
static char *pci_ids = NULL;

/** Length of data store at at pci_ids */
static unsigned int pci_ids_len;

/** Iterator position into pci_ids */
static unsigned int pci_ids_iter_pos;

/** Initialize the pci.ids line iterator to the beginning of the file */
static void
pci_ids_line_iter_init ()
{
	pci_ids_iter_pos = 0;
}

/** Maximum length of lines in pci.ids */
#define PCI_IDS_MAX_LINE_LEN 512

/** Get the next line from pci.ids
 *
 *  @param  line_len            Pointer to where number of bytes in line will
 *                              be stored
 *  @return                     Pointer to the line; only valid until the
 *                              next invocation of this function
 */
static char *
pci_ids_line_iter_get_line (unsigned int *line_len)
{
	unsigned int i;
	static char line[PCI_IDS_MAX_LINE_LEN];

	for (i = 0;
	     pci_ids_iter_pos < pci_ids_len &&
	     i < PCI_IDS_MAX_LINE_LEN - 1 &&
	     pci_ids[pci_ids_iter_pos] != '\n'; i++, pci_ids_iter_pos++) {
		line[i] = pci_ids[pci_ids_iter_pos];
	}

	line[i] = '\0';
	if (line_len != NULL)
		*line_len = i;

	pci_ids_iter_pos++;

	return line;
}

/** See if there are more lines to process in pci.ids
 *
 *  @return                     #TRUE iff there are more lines to process
 */
static dbus_bool_t
pci_ids_line_iter_has_more ()
{
	return pci_ids_iter_pos < pci_ids_len;
}


/** Find the names for a PCI device.
 *
 *  The pointers returned are only valid until the next invocation of this
 *  function.
 *
 *  @param  vendor_id           PCI vendor id or 0 if unknown
 *  @param  product_id          PCI product id or 0 if unknown
 *  @param  subsys_vendor_id    PCI subsystem vendor id or 0 if unknown
 *  @param  subsys_product_id   PCI subsystem product id or 0 if unknown
 *  @param  vendor_name         Set to pointer of result or #NULL
 *  @param  product_name        Set to pointer of result or #NULL
 *  @param  subsys_vendor_name  Set to pointer of result or #NULL
 *  @param  subsys_product_name Set to pointer of result or #NULL
 */
static void
pci_ids_find (int vendor_id, int product_id,
	      int subsys_vendor_id, int subsys_product_id,
	      char **vendor_name, char **product_name,
	      char **subsys_vendor_name, char **subsys_product_name)
{
	char *line;
	unsigned int i;
	unsigned int line_len;
	unsigned int num_tabs;
	char rep_vi[8];
	char rep_pi[8];
	char rep_svi[8];
	char rep_spi[8];
	static char store_vn[PCI_IDS_MAX_LINE_LEN];
	static char store_pn[PCI_IDS_MAX_LINE_LEN];
	static char store_svn[PCI_IDS_MAX_LINE_LEN];
	static char store_spn[PCI_IDS_MAX_LINE_LEN];
	dbus_bool_t vendor_matched = FALSE;
	dbus_bool_t product_matched = FALSE;

	snprintf (rep_vi, 8, "%04x", vendor_id);
	snprintf (rep_pi, 8, "%04x", product_id);
	snprintf (rep_svi, 8, "%04x", subsys_vendor_id);
	snprintf (rep_spi, 8, "%04x", subsys_product_id);

	*vendor_name = NULL;
	*product_name = NULL;
	*subsys_vendor_name = NULL;
	*subsys_product_name = NULL;

	for (pci_ids_line_iter_init (); pci_ids_line_iter_has_more ();) {
		line = pci_ids_line_iter_get_line (&line_len);

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

			/* first check subsys_vendor_id, if haven't done 
			 * already */
			if (*subsys_vendor_name == NULL
			    && subsys_vendor_id != 0) {
				if ((*((dbus_uint32_t *) line)) ==
				    (*((dbus_uint32_t *) rep_svi))) {
					/* found it */
					for (i = 4; i < line_len; i++) {
						if (!isspace (line[i]))
							break;
					}
					strncpy (store_svn, line + i,
						 PCI_IDS_MAX_LINE_LEN);
					*subsys_vendor_name = store_svn;
				}
			}

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
						 PCI_IDS_MAX_LINE_LEN);
					*vendor_name = store_vn;
				}
			}

			break;

		case 1:
			product_matched = FALSE;

			/* product names */
			if (!vendor_matched)
				continue;

			/* check product_id */
			if (product_id != 0) {
				if (memcmp (line + 1, rep_pi, 4) == 0) {
					/* found it */

					product_matched = TRUE;

					for (i = 5; i < line_len; i++) {
						if (!isspace (line[i]))
							break;
					}
					strncpy (store_pn, line + i,
						 PCI_IDS_MAX_LINE_LEN);
					*product_name = store_pn;
				}
			}
			break;

		case 2:
			/* subsystem_vendor subsystem_product */
			if (!vendor_matched || !product_matched)
				continue;

			/* check product_id */
			if (subsys_vendor_id != 0
			    && subsys_product_id != 0) {
				if (memcmp (line + 2, rep_svi, 4) == 0
				    && memcmp (line + 7, rep_spi,
					       4) == 0) {
					/* found it */
					for (i = 11; i < line_len; i++) {
						if (!isspace (line[i]))
							break;
					}
					strncpy (store_spn, line + i,
						 PCI_IDS_MAX_LINE_LEN);
					*subsys_product_name = store_spn;
				}
			}

			break;

		default:
			break;
		}

	}
}

/** Load the PCI database used for mapping vendor, product, subsys_vendor
 *  and subsys_product numbers into names.
 *
 *  @param  path                Path of the pci.ids file, e.g. 
 *                              /usr/share/hwdata/pci.ids
 *  @return                     #TRUE if the file was succesfully loaded
 */
static dbus_bool_t
pci_ids_load (const char *path)
{
	FILE *fp;
	unsigned int num_read;

	fp = fopen (path, "r");
	if (fp == NULL) {
		HAL_ERROR (("couldn't open PCI database at %s,", path));
		return FALSE;
	}

	fseek (fp, 0, SEEK_END);
	pci_ids_len = ftell (fp);
	fseek (fp, 0, SEEK_SET);

	pci_ids = malloc (pci_ids_len);
	if (pci_ids == NULL) {
		DIE (("Couldn't allocate %d bytes for PCI database file\n",
		      pci_ids_len));
	}

	num_read = fread (pci_ids, sizeof (char), pci_ids_len, fp);
	if (pci_ids_len != num_read) {
		HAL_ERROR (("Error loading PCI database file\n"));
		free (pci_ids);
		pci_ids = NULL;
		return FALSE;
	}

	return TRUE;
}

/** Free resources used by to store the PCI database
 *
 *  @param                      #FALSE if the PCI database wasn't loaded
 */
static dbus_bool_t
pci_ids_free ()
{
	if (pci_ids != NULL) {
		free (pci_ids);
		pci_ids = NULL;
		return TRUE;
	}
	return FALSE;
}


/** This function will compute the device uid based on other properties
 *  of the device. Specifically, the following properties are required:
 *
 *   - pci.idVendor, pci.idProduct
 *
 *  Other properties may also be used.
 *
 *  Requirements for uid:
 *   - do not rely on bus, port etc.; we want this id to be as unique for
 *     the device as we can
 *   - make sure it doesn't rely on properties that cannot be obtained
 *     from the minimal information we can obtain on an unplug event
 *
 *  @param  d                   HalDevice object
 *  @param  append_num          Number to append to name if not -1
 *  @return                     New unique device id; only good until the
 *                              next invocation of this function
 */
static char *
pci_device_compute_udi (HalDevice *d, int append_num)
{
	static char buf[256];

	if (append_num == -1)
		sprintf (buf, "/org/freedesktop/Hal/devices/pci_%x_%x",
			 hal_device_property_get_int (d, "pci.vendor_id"),
			 hal_device_property_get_int (d, "pci.product_id"));
	else
		sprintf (buf, "/org/freedesktop/Hal/devices/pci_%x_%x/%d",
			 hal_device_property_get_int (d, "pci.vendor_id"),
			 hal_device_property_get_int (d, "pci.product_id"),
			 append_num);

	return buf;
}


/** Set capabilities from PCI class. This is a function from hell,
 *  maybe some searchable data-structure would be better...
 *
 *  @param  d                   HalDevice to set caps on
 *  @param  dev_class           device class
 *  @param  dev_sub_class       device sub class
 *  @param  dev_proto           device protocol
 */
static void
pci_add_caps_from_class (HalDevice * d,
			 int dev_class, int dev_sub_class, int dev_proto)
{
	char *cat = NULL;

	switch (dev_class) {
	case 0x01:
		cat = "storage_controller";
		hal_device_add_capability (d, "storage_controller");
		switch (dev_sub_class) {
		case 0x00:
			hal_device_add_capability (d, "storage_controller.scsi");
			break;
		case 0x01:
			hal_device_add_capability (d, "storage_controller.ide");
			break;
		case 0x02:
			hal_device_add_capability (d, "storage_controller.floppy");
			break;
		case 0x03:
			hal_device_add_capability (d, "storage_controller.ipi");
			break;
		case 0x04:
			hal_device_add_capability (d, "storage_controller.raid");
			break;
		}
		break;
	case 0x02:
		cat = "net";
		hal_device_add_capability (d, "net");
		switch (dev_sub_class) {
		case 0x00:
			hal_device_add_capability (d, "net.ethernet");
			break;
		case 0x01:
			hal_device_add_capability (d, "net.tokenring");
			break;
		case 0x02:
			hal_device_add_capability (d, "net.fddi");
			break;
		case 0x03:
			hal_device_add_capability (d, "net.atm");
			break;
		case 0x04:
			hal_device_add_capability (d, "net.isdn");
			break;
		}
		break;
	case 0x03:
		cat = "video";
		hal_device_add_capability (d, "video");
		if (dev_sub_class == 0x00 && dev_proto == 0x00)
			hal_device_add_capability (d, "video.vga");
		if (dev_sub_class == 0x00 && dev_proto == 0x01)
			hal_device_add_capability (d, "video.8514");
		else if (dev_sub_class == 0x01)
			hal_device_add_capability (d, "video.xga");
		else if (dev_sub_class == 0x02)
			hal_device_add_capability (d, "video.3d");
		break;
	case 0x04:
		cat = "multimedia";
		hal_device_add_capability (d, "multimedia");
		switch (dev_sub_class) {
		case 0x00:
			hal_device_add_capability (d, "multimedia.video");
			cat = "multimedia.video";
			break;
		case 0x01:
			hal_device_add_capability (d, "multimedia.audio");
			cat = "multimedia.audio";
			break;
		case 0x02:
			hal_device_add_capability (d, "multimedia.telephony");
			cat = "multimedia.telephony";
			break;
		}
		break;
	case 0x06:
		cat = "bridge";
		hal_device_add_capability (d, "bridge");
		switch (dev_sub_class) {
		case 0x00:
			hal_device_add_capability (d, "bridge.host");
			break;
		case 0x01:
			hal_device_add_capability (d, "bridge.isa");
			break;
		case 0x02:
			hal_device_add_capability (d, "bridge.eisa");
			break;
		case 0x03:
			hal_device_add_capability (d, "bridge.micro_channel");
			break;
		case 0x04:
			hal_device_add_capability (d, "bridge.pci");
			break;
		case 0x05:
			hal_device_add_capability (d, "bridge.pcmcia");
			break;
		case 0x06:
			hal_device_add_capability (d, "bridge.nubus");
			break;
		case 0x07:
			cat = "bridge.cardbus";
			hal_device_add_capability (d, "bridge.cardbus");
			break;
		case 0x08:
			hal_device_add_capability (d, "bridge.raceway");
			break;
		case 0x09:
			hal_device_add_capability (d, "bridge.semi_transparent");
			break;
		case 0x0a:
			hal_device_add_capability (d, "bridge.infiniband");
			break;
		}
		break;
	case 0x07:
		cat = "comm";
		hal_device_add_capability (d, "comm");
		if (dev_sub_class == 0x00) {
			cat = "comm.serial";
			hal_device_add_capability (d, "comm.serial");
			switch (dev_proto) {
			case 0x00:
				hal_device_add_capability (d, "comm.serial.8250");
				break;
			case 0x01:
				hal_device_add_capability (d, "comm.serial.16450");
				break;
			case 0x02:
				hal_device_add_capability (d, "comm.serial.16550");
				break;
			case 0x03:
				hal_device_add_capability (d, "comm.serial.16650");
				break;
			case 0x04:
				hal_device_add_capability (d, "comm.serial.16750");
				break;
			case 0x05:
				hal_device_add_capability (d, "comm.serial.16850");
				break;
			case 0x06:
				hal_device_add_capability (d, "comm.serial.16950");
				break;
			}
		} else if (dev_sub_class == 0x01) {
			cat = "comm.parallel";
			hal_device_add_capability (d, "comm.parallel");
			switch (dev_proto) {
			case 0x00:
				hal_device_add_capability (d, "comm.parallel.spp");
				break;
			case 0x01:
				hal_device_add_capability (d,
						   "comm.parallel.bidir");
				break;
			case 0x02:
				hal_device_add_capability (d, "comm.parallel.ecp");
				break;
			case 0x03:
				hal_device_add_capability (d,
						   "comm.parallel.ieee1284");
				break;
			case 0xfe:
				hal_device_add_capability (d,
						   "comm.parallel.ieee1284_target");
				break;
			}
		} else if (dev_sub_class == 0x02) {
			cat = "comm.serial";
			hal_device_add_capability (d, "comm.serial");
			hal_device_add_capability (d, "comm.serial.multiport");
		} else if (dev_sub_class == 0x03) {
			cat = "modem";
			hal_device_add_capability (d, "modem");
			if (dev_proto >= 0x01 && dev_proto <= 0x04)
				hal_device_add_capability (d, "modem.hayes");
		}
		break;
	case 0x0c:
		cat = "serial_controller";
		hal_device_add_capability (d, "serial_controller");
		switch (dev_sub_class) {
		case 0x00:
			cat = "serial_controller.ieee1394";
			hal_device_add_capability (d,
					   "serial_controller.ieee1394");
			if (dev_proto == 0x10)
				hal_device_add_capability (d,
						   "serial_controller.ieee1394.ohci");
			break;
		case 0x01:
			hal_device_add_capability (d, "serial_controller.access");
			break;
		case 0x02:
			hal_device_add_capability (d, "serial_controller.ssa");
			break;
		case 0x03:
			cat = "serial_controller.usb";
			hal_device_add_capability (d, "serial_controller.usb");
			switch (dev_proto) {
			case 0x00:
				hal_device_add_capability (d,
						   "serial_controller.usb.uhci");
				break;
			case 0x01:
				hal_device_add_capability (d,
						   "serial_controller.usb.ohci");
				break;
			case 0x02:
				hal_device_add_capability (d,
						   "serial_controller.usb.ehci");
				break;
			case 0xfe:
				hal_device_add_capability (d,
						   "serial_controller.usb.device");
				break;
			}
			break;
		}
		break;
	case 0x0d:
		cat = "wireless";
		hal_device_add_capability (d, "wireless");
		switch (dev_sub_class) {
		case 0x00:
			hal_device_add_capability (d, "wireless.irda");
			break;
		case 0x01:
			hal_device_add_capability (d,
					   "wireless.consumer_controller");
			break;
		case 0x02:
			hal_device_add_capability (d, "wireless.rf_controller");
			break;
		}
		break;
	case 0x0f:
		cat = "satellite_controller";
		hal_device_add_capability (d, "satellite_controller");
		switch (dev_sub_class) {
		case 0x00:
			hal_device_add_capability (d, "satellite_controller.tv");
			break;
		case 0x01:
			hal_device_add_capability (d,
					   "satellite_controller.audio");
			break;
		case 0x02:
			hal_device_add_capability (d,
					   "satellite_controller.video");
			break;
		case 0x03:
			hal_device_add_capability (d, "satellite_controller.data");
			break;
		}
		break;
	}

	if (cat != NULL)
		hal_device_property_set_string (d, "info.category", cat);
}




/** Init function for PCI handling
 *
 */
static void
pci_device_init (BusDeviceHandler *self)
{
	/* get all drivers under /sys/bus/pci/drivers */
	drivers_collect ("pci");

	/* Load /usr/share/hwdata/pci.ids */
	pci_ids_load (HWDATA_DIR "/pci.ids");
}

/** Shutdown function for PCI handling
 *
 */
static void
pci_device_shutdown (BusDeviceHandler *self)
{
	pci_ids_free ();
}


static void 
pci_device_post_process (BusDeviceHandler *self,
			 HalDevice *d,
			 const char *sysfs_path,
			 struct sysfs_device *device)
{
	int i;
	int len;
	int vendor_id = 0;
	int product_id = 0;
	int subsys_vendor_id = 0;
	int subsys_product_id = 0;
	char *vendor_name;
	char *product_name;
	char *subsys_vendor_name;
	char *subsys_product_name;
	struct sysfs_attribute *cur;
	char attr_name[SYSFS_NAME_LEN];
	dbus_int32_t cls = 0x00ffffff;
	char namebuf[512];

	dlist_for_each_data (sysfs_get_device_attributes (device), cur,
			     struct sysfs_attribute) {

		if (sysfs_get_name_from_path (cur->path,
					      attr_name,
					      SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		/*printf("attr_name=%s -> '%s'\n", attr_name, cur->value); */

		if (strcmp (attr_name, "device") == 0)
			product_id = parse_hex (cur->value);
		else if (strcmp (attr_name, "vendor") == 0)
			vendor_id = parse_hex (cur->value);
		else if (strcmp (attr_name, "subsystem_device") == 0)
			subsys_product_id = parse_hex (cur->value);
		else if (strcmp (attr_name, "subsystem_vendor") == 0)
			subsys_vendor_id = parse_hex (cur->value);
		else if (strcmp (attr_name, "class") == 0) {
			cls = parse_hex (cur->value);
		}
	}

	hal_device_property_set_int (d, "pci.vendor_id", vendor_id);
	hal_device_property_set_int (d, "pci.product_id", product_id);
	hal_device_property_set_int (d, "pci.subsys_vendor_id", subsys_vendor_id);
	hal_device_property_set_int (d, "pci.subsys_product_id",
			     subsys_product_id);

	/* Lookup names in pci.ids */
	pci_ids_find (vendor_id, product_id, subsys_vendor_id,
		      subsys_product_id, &vendor_name, &product_name,
		      &subsys_vendor_name, &subsys_product_name);
	if (vendor_name != NULL)
		hal_device_property_set_string (d, "pci.vendor", vendor_name);
	if (product_name != NULL)
		hal_device_property_set_string (d, "pci.product", product_name);
	if (subsys_vendor_name != NULL)
		hal_device_property_set_string (d, "pci.subsys_vendor",
					subsys_vendor_name);
	if (subsys_product_name != NULL)
		hal_device_property_set_string (d, "pci.subsys_product",
					subsys_product_name);

	/* Provide best-guess of name, goes in Product property; 
	 * .fdi files can override this */
	if (product_name != NULL) {
		hal_device_property_set_string (d, "info.product", product_name);
	} else {
		snprintf (namebuf, sizeof(namebuf), "Unknown (0x%04x)", product_id);
		hal_device_property_set_string (d, "info.product", namebuf);
	}

	/* Provide best-guess of vendor, goes in Vendor property; 
	 * .fdi files can override this */
	if (vendor_name != NULL) {
		hal_device_property_set_string (d, "info.vendor", vendor_name);
	} else {
		snprintf (namebuf, sizeof(namebuf), "Unknown (0x%04x)", vendor_id);
		hal_device_property_set_string (d, "info.vendor", namebuf);
	}


	hal_device_property_set_int (d, "pci.device_class", (cls >> 16) & 0xff);
	hal_device_property_set_int (d, "pci.device_subclass", (cls >> 8) & 0xff);
	hal_device_property_set_int (d, "pci.device_protocol", cls & 0xff);
	pci_add_caps_from_class (d, (cls >> 16) & 0xff, (cls >> 8) & 0xff,
				 cls & 0xff);
}


/** Method specialisations for bustype pci */
BusDeviceHandler pci_bus_handler = {
	pci_device_init,           /**< init function */
	bus_device_detection_done, /**< detection is done */
	pci_device_shutdown,       /**< shutdown function */
	bus_device_tick,           /**< timer function */
	bus_device_accept,         /**< accept function */
 	bus_device_visit,          /**< visitor function */
	bus_device_removed,        /**< device is removed */
	pci_device_compute_udi,    /**< UDI computing function */
	pci_device_post_process,   /**< add more properties */
	"pci",                     /**< sysfs bus name */
	"pci"                      /**< namespace */
};


/** @} */
