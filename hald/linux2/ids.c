/***************************************************************************
 * CVSID: $Id$
 *
 * classdev.c : Handling of functional kernel devices
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "../logger.h"

#include "ids.h"

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
void
ids_find_pci (int vendor_id, int product_id,
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
	dbus_bool_t vendor_matched = FALSE;
	dbus_bool_t product_matched = FALSE;
	static char store_vn[PCI_IDS_MAX_LINE_LEN];
	static char store_pn[PCI_IDS_MAX_LINE_LEN];
	static char store_svn[PCI_IDS_MAX_LINE_LEN];
	static char store_spn[PCI_IDS_MAX_LINE_LEN];

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
		HAL_ERROR (("Error loading PCI database file"));
		free (pci_ids);
		pci_ids = NULL;
		fclose(fp);
		return FALSE;
	}

	fclose(fp);
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





/*==========================================================================*/

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
void
ids_find_usb (int vendor_id, int product_id,
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
		fclose(fp);
		return FALSE;
	}

	num_read = fread (usb_ids, sizeof (char), usb_ids_len, fp);
	if (usb_ids_len != num_read) {
		printf ("Error loading USB database file\n");
		free (usb_ids);
		usb_ids = NULL;
		fclose(fp);
		return FALSE;
	}

	fclose(fp);
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

void 
ids_init (void)
{
	/* Load /usr/share/hwdata/pci.ids */
	pci_ids_load (HWDATA_DIR "/pci.ids");

	/* Load /usr/share/hwdata/usb.ids */
	usb_ids_load (HWDATA_DIR "/usb.ids");
}

