/***************************************************************************
 * CVSID: $Id$
 *
 * ids.h : Lookup names from hardware identifiers
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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

#ifndef IDS_H
#define IDS_H

#include <glib.h>

#ifdef USE_PCI_IDS

void pci_ids_init (void);

void
ids_find_pci (int vendor_id, int product_id,
	      int subsys_vendor_id, int subsys_product_id,
	      char **vendor_name, char **product_name,
	      char **subsys_vendor_name, char **subsys_product_name);

#else /*USE_PCI_IDS*/
static inline void pci_ids_init (void) {return;};

static inline void
ids_find_pci (int vendor_id, int product_id,
	      int subsys_vendor_id, int subsys_product_id,
	      char **vendor_name, char **product_name,
	      char **subsys_vendor_name, char **subsys_product_name) {
	*vendor_name = NULL;
	*product_name = NULL;
	*subsys_vendor_name = NULL;
	*subsys_product_name = NULL;
	return;
}
#endif /*USE_PCI_IDS*/

#ifdef USE_PNP_IDS

void
ids_find_pnp (const char *pnp_id, char **pnp_description);

#else /*USE_PNP_IDS*/
static inline void
ids_find_pnp (const char *pnp_id, char **pnp_description) {
	*pnp_description = NULL;
	return;
}
#endif /*USE_PNP_IDS*/

#ifdef USE_USB_IDS

void usb_ids_init (void);

void
ids_find_usb (int vendor_id, int product_id,
	      char **vendor_name, char **product_name);

#else /*USE_USB_IDS*/
static inline void usb_ids_init (void) {return;}
static inline void
ids_find_usb (int vendor_id, int product_id,
	      char **vendor_name, char **product_name) {
	*vendor_name = NULL;
	*product_name = NULL;
	return;
}
#endif /*USE_USB_IDS*/

void ids_init (void);

#endif /* IDS_H */
