/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_net.c : Network device functions on Linux 2.6
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 *
 * Some parts of this file is based on mii-diag.c, Copyright 1997-2003 by
 * Donald Becker <becker@scyld.com>
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

#include <glib.h>

#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if_arp.h>		/* for ARPHRD_ETHER etc. */

#include "../hald.h"
#include "../logger.h"
#include "../device_store.h"
#include "linux_class_net.h"

/**
 * @defgroup HalDaemonLinuxNet Networking class
 * @ingroup HalDaemonLinux
 * @brief Networking class
 * @{
 */


/* fwd decl */
static void visit_class_device_net_got_sysdevice (HalDevice * parent,
						  void *data1,
						  void *data2);

/** Visitor function for net device.
 *
 *  This function parses the attributes present and merges more information
 *  into the HAL device this class device points to
 *
 *  @param  path                Sysfs-path for device
 *  @param  device              libsysfs object for device
 */
void
visit_class_device_net (const char *path,
			struct sysfs_class_device *class_device)
{
	int i;
	int len;
	HalDevice *d;
	struct sysfs_attribute *cur;
	char attr_name[SYSFS_NAME_LEN];
	char *addr_store = NULL;
	int media_type = 0;
	char *media;

	if (class_device->sysdevice == NULL) {
		HAL_WARNING (("Net class device at sysfs path %s doesn't "
			      "have sysdevice", path));
		return;
	}

	d = ds_device_new ();
	ds_property_set_string (d, "net.interface", class_device->name);
	ds_property_set_string (d, "net.linux.sysfs_path", path);

	dlist_for_each_data (sysfs_get_classdev_attributes (class_device),
			     cur, struct sysfs_attribute) {
		if (sysfs_get_name_from_path
		    (cur->path, attr_name, SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; i >= 0 && isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		if (strcmp (attr_name, "address") == 0) {
			addr_store = cur->value;
		} else if (strcmp (attr_name, "type") == 0) {
			media_type = parse_dec (cur->value);
		}
	}

	if (addr_store != NULL && media_type == ARPHRD_ETHER) {
		unsigned int a5, a4, a3, a2, a1, a0;

		ds_property_set_string (d, "net.ethernet.mac_addr",
					addr_store);

		if (sscanf (addr_store, "%x:%x:%x:%x:%x:%x",
			    &a5, &a4, &a3, &a2, &a1, &a0) == 6) {
			dbus_uint32_t mac_upper, mac_lower;

			mac_upper = (a5 << 16) | (a4 << 8) | a3;
			mac_lower = (a2 << 16) | (a1 << 8) | a0;

			ds_property_set_int (d,
					     "net.ethernet.mac_addr_upper24",
					     (dbus_int32_t) mac_upper);
			ds_property_set_int (d,
					     "net.ethernet.mac_addr_lower24",
					     (dbus_int32_t) mac_lower);
		}
	}


	/* check for driver */
	if (class_device->driver != NULL) {
		ds_property_set_string (d, "linux.driver",
					class_device->driver->name);
	}


	ds_property_set_int (d, "net.arp_proto_hw_id", media_type);

	/* Always set capabilities as the last thing the addition of a 
	 * capability triggers a signal to other apps using HAL, monitoring
	 * daemons for instance
	 */

	ds_property_set_string (d, "info.category", "net");
	ds_add_capability (d, "net");

	/* type is decimal according to net/core/net-sysfs.c and it
	 * assumes values from /usr/include/net/if_arp.h. Either
	 * way we store both the 
	 */
	switch (media_type) {
	case ARPHRD_NETROM:
		media = "NET/ROM pseudo";
		break;
	case ARPHRD_ETHER:
		media = "Ethernet";
		ds_add_capability (d, "net.ethernet");
		break;
	case ARPHRD_EETHER:
		media = "Experimenal Ethernet";
		break;
	case ARPHRD_AX25:
		media = "AX.25 Level 2";
		break;
	case ARPHRD_PRONET:
		media = "PROnet tokenring";
		ds_add_capability (d, "net.tokenring");
		break;
	case ARPHRD_CHAOS:
		media = "Chaosnet";
		break;
	case ARPHRD_IEEE802:
		media = "IEEE802";
		break;
	case ARPHRD_ARCNET:
		media = "ARCnet";
		break;
	case ARPHRD_APPLETLK:
		media = "APPLEtalk";
		break;
	case ARPHRD_DLCI:
		media = "Frame Relay DLCI";
		break;
	case ARPHRD_ATM:
		media = "ATM";
		ds_add_capability (d, "net.atm");
		break;
	case ARPHRD_METRICOM:
		media = "Metricom STRIP (new IANA id)";
		break;
#ifdef ARPHRD_IEEE1394
	case ARPHRD_IEEE1394:
		media = "IEEE1394 IPv4 - RFC 2734";
		break;
#endif
	default:
		media = "Unknown";
		break;
	}
	ds_property_set_string (d, "net.media", media);

	/* Find the physical; this happens asynchronously as it might
	 * be added later. If we are probing this can't happen so the
	 * timeout is set to zero in that event..
	 */
	ds_device_async_find_by_key_value_string
	    ("linux.sysfs_path_device", class_device->sysdevice->path,
	     FALSE, visit_class_device_net_got_sysdevice, (void *) d, NULL,
	     is_probing ? 0 : HAL_LINUX_HOTPLUG_TIMEOUT);
}

/** Callback when the sysdevice is found or if there is no parent.. This is
 *  where we get added to the GDL..
 *
 *  @param  parent              Async Return value from the find call
 *  @param  data1               User data
 *  @param  data2               User data
 */
static void
visit_class_device_net_got_sysdevice (HalDevice * sysdevice,
				      void *data1, void *data2)
{
	HalDevice *d = (HalDevice *) data1;

	if (sysdevice == NULL) {
		HAL_WARNING (("Sysdevice for a class net device never appeared!"));
	} else {
		/* merge information from temporary device into the physical
		 * device 
		 */
		ds_device_merge (sysdevice, d);
	}

	/* get rid of tempoary device; it was only a placeholder after all */
	ds_device_destroy (d);
}



/** Structure for holding watching information for an ethernet interface */
typedef struct link_detection_if_s {
	HalDevice *device;		/**< HalDevice* object */
	int skfd;			/**< File descriptor for socket */
	struct ifreq ifr;		/**< Structure used in ioctl() */
	int new_ioctl_nums;		/**< Is the new ioctl being used? */
	dbus_uint16_t status_word_baseline;
					/**< Last status word read */
	struct link_detection_if_s *next;/**< Pointer to next element */
} link_detection_if;

/** Head of linked list of ethernet interfaces to watch */
static link_detection_if *link_detection_list_head = NULL;

/** Read a word from the MII transceiver management registers 
 *
 *  @param  iface               Which interface
 *  @param  location            Which register
 *  @return                     Word that is read
 */
static dbus_uint16_t
mdio_read (link_detection_if * iface, int location)
{
	dbus_uint16_t *data = (dbus_uint16_t *) (&(iface->ifr.ifr_data));

	data[1] = location;

	if (ioctl (iface->skfd,
		   iface->new_ioctl_nums ? 0x8948 : SIOCDEVPRIVATE + 1,
		   &(iface->ifr)) < 0) {
		HAL_ERROR (("SIOCGMIIREG on %s failed: %s\n",
			    iface->ifr.ifr_name, strerror (errno)));
		return -1;
	}
	return data[3];
}

/** Check whether status has changed.
 *
 *  @param  iface               Which interface
 */
static void
link_detection_process (link_detection_if * iface)
{
	dbus_bool_t got_link = FALSE;
	dbus_uint16_t status_word;
	dbus_uint16_t link_word;
	dbus_uint16_t status_word_new;

	/*printf("iface = 0x%0x\n", iface); */

	status_word_new = mdio_read (iface, 1);
	if (status_word_new != iface->status_word_baseline) {
		iface->status_word_baseline = status_word_new;

		HAL_INFO (("Ethernet link status change on hal udi %s)",
			   iface->device->udi));

		/* Read status_word again since some bits may be sticky */
		status_word = mdio_read (iface, 1);

		/* Refer to http://www.scyld.com/diag/mii-status.html for
		 * the full explanation of the numbers
		 *
		 * 0x8000  Capable of 100baseT4.
		 * 0x7800  Capable of 10/100 HD/FD (most common).
		 * 0x0040  Preamble suppression permitted.
		 * 0x0020  Autonegotiation complete.
		 * 0x0010  Remote fault.
		 * 0x0008  Capable of Autonegotiation.
		 * 0x0004  Link established ("sticky"* on link failure)
		 * 0x0002  Jabber detected ("sticky"* on transmit jabber)
		 * 0x0001  Extended MII register exist.
		 *
		 */

		property_atomic_update_begin ();

		if ((status_word & 0x0016) == 0x0004) {
			ds_property_set_bool (iface->device,
					      "net.ethernet.link", TRUE);
			got_link = TRUE;
		} else {
			ds_property_set_bool (iface->device,
					      "net.ethernet.link", FALSE);
		}

		/* Read link word
		 *
		 * 0x8000  Link partner can send more info.
		 * 0x4000  Link partner got our advertised abilities.
		 * 0x2000  Fault detected by link partner (uncommon).
		 * 0x0400  Flow control supported (currently uncommon)
		 * 0x0200  100baseT4 supported (uncommon)
		 * 0x0100  100baseTx-FD (full duplex) supported
		 * 0x0080  100baseTx supported
		 * 0x0040  10baseT-FD supported
		 * 0x0020  10baseT supported
		 * 0x001F  Protocol selection bits, always 0x0001 for Ethernet.
		 */
		link_word = mdio_read (iface, 1);


		if (link_word & 0x0300) {
			ds_property_set_int (iface->device,
					     "net.ethernet.rate",
					     100 * 1000 * 1000);
		}
		if (link_word & 0x60) {
			ds_property_set_int (iface->device,
					     "net.ethernet.rate",
					     10 * 1000 * 1000);
		}

		property_atomic_update_end ();

		emit_condition (iface->device, "NetLinkEvent",
				DBUS_TYPE_BOOLEAN, got_link,
				DBUS_TYPE_INVALID);
	}
}

/** Timeout handler for processing status on all watched interfaces
 *
 *  @param  data                User data when setting up timer
 *  @return                     TRUE iff timer should be kept
 */
static gboolean
link_detection_timer_handler (gpointer data)
{
	link_detection_if *iface;

	for (iface = link_detection_list_head; iface != NULL;
	     iface = iface->next)
		link_detection_process (iface);

	return TRUE;
}

/** Add a watch for a HAL device; it must be a net.ethernet capable.
 *
 *  @param  device              HalDevice object
 */
static void
link_detection_add (HalDevice * device)
{
	const char *interface_name;
	link_detection_if *iface;

	iface = malloc (sizeof (link_detection_if));
	if (iface == NULL)
		DIE (("No memory"));

	interface_name = ds_property_get_string (device, "net.interface");
	if (interface_name == NULL) {
		HAL_WARNING (("device '%s' does not have net.interface\n",
			      device->udi));
		free (iface);
		return;
	}

	iface->device = device;

	snprintf (iface->ifr.ifr_name, IFNAMSIZ, interface_name);

	/* Open a basic socket. */
	if ((iface->skfd = socket (AF_INET, SOCK_DGRAM, 0)) < 0) {
		HAL_ERROR (("cannot open socket on interface %s; errno=%d\n",
			    interface_name, errno));
		free (iface);
		return;
	}

	if (ioctl (iface->skfd, 0x8947, &(iface->ifr)) >= 0) {
		iface->new_ioctl_nums = 1;
	} else if (ioctl (iface->skfd, SIOCDEVPRIVATE, &(iface->ifr)) >= 0) {
		iface->new_ioctl_nums = 0;
	} else {
		HAL_ERROR (("SIOCGMIIPHY on %s failed: %s\n",
			    iface->ifr.ifr_name, strerror (errno)));
		(void) close (iface->skfd);
		free (iface);
		return;
	}

	iface->status_word_baseline = 0x5555;


	link_detection_process (iface);

	iface->next = link_detection_list_head;
	link_detection_list_head = iface;
}

/** Remove watch for a HAL device
 *
 */
static void
link_detection_remove (HalDevice * device)
{
	link_detection_if *iface;
	link_detection_if *iface_prev = NULL;

	for (iface = link_detection_list_head; iface != NULL;
	     iface = iface->next) {
		if (iface->device == device) {

			HAL_INFO (("Stopping ethernet link monitoring on "
				   "device %s", device->udi));

			if (iface_prev != NULL) {
				iface_prev->next = iface->next;
			} else {
				link_detection_list_head = iface->next;
			}

			close (iface->skfd);
			free (iface);
		}

		iface_prev = iface;
	}
}


/** Callback for when a new capability is added to a device.
 *
 *  @param  device              Pointer a #HalDevice object
 *  @param  capability          Capability added
 *  @param  in_gdl              True iff the device object in visible in the
 *                              global device list
 */
static void
new_capability (HalDevice * device, const char *capability,
		dbus_bool_t in_gdl)
{
	if (in_gdl) {
		if (strcmp (capability, "net.ethernet") == 0) {
			link_detection_add (device);
		}
	}
}

/** Callback for the global device list has changed
 *
 *  @param  device              Pointer a #HalDevice object
 *  @param  is_added            True iff device was added
 */
static void
gdl_changed (HalDevice * device, dbus_bool_t is_added)
{
	if (is_added) {
		if (ds_query_capability (device, "net.ethernet")) {
			link_detection_add (device);
		}
	} else {
		/* We may not have added this device yet, but the 
		 * callee checks for for this
		 */
		link_detection_remove (device);
	}
}




/** Init function for block device handling
 *
 */
void
linux_class_net_init ()
{
	g_timeout_add (1000, link_detection_timer_handler, NULL);

	/* We want to know when net.ethernet devices appear and disappear */
	ds_add_cb_newcap (new_capability);
	ds_add_cb_gdl_changed (gdl_changed);
}

/** This function is called when all device detection on startup is done
 *  in order to perform optional batch processing on devices
 *
 */
void
linux_class_net_detection_done ()
{
}

/** Shutdown function for block device handling
 *
 */
void
linux_class_net_shutdown ()
{
}

/** @} */
