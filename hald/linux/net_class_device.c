/***************************************************************************
 * CVSID: $Id$
 *
 * Network device class
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2004 Novell, Inc.
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
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <net/if_arp.h> /* for ARPHRD_... */
#include <sys/socket.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"
#include "../hald_dbus.h"

#include "class_device.h"
#include "common.h"

#if PCMCIA_SUPPORT_ENABLE
#include "pcmcia_utils.h"
#endif

/**
 * @defgroup HalDaemonLinuxNet Network class
 * @ingroup HalDaemonLinux
 * @brief Network class
 * @{
 */

static const char *
media_type_to_string (int media_type)
{
	switch (media_type) {
	case ARPHRD_NETROM:
		return "NET/ROM pseudo";

	case ARPHRD_ETHER:
		return "Ethernet";

	case ARPHRD_EETHER:
		return "Experimental Ethernet";

	case ARPHRD_AX25:
		return "AX.25 Level 2";

	case ARPHRD_PRONET:
		return "PROnet token ring";

	case ARPHRD_CHAOS:
		return "Chaosnet";

	case ARPHRD_ARCNET:
		return "ARCnet";

	case ARPHRD_APPLETLK:
		return "Appletalk";

	case ARPHRD_DLCI:
		return "Frame Relay DLCI";

	case ARPHRD_ATM:
		return "ATM";

	case ARPHRD_METRICOM:
		return "Metricom STRIP (new IANA id)";

#ifdef ARPHRD_IEEE1394
	case ARPHRD_IEEE1394:
		return "IEEE1394 IPv4 - RFC 2734";
#endif

	default:
		return "Unknown";
	}
}

/** Read a word from the MII transceiver management registers 
 *
 *  @param  iface               Which interface
 *  @param  location            Which register
 *  @return                     0 on success, -1 on failure
 */
static int
mdio_read (int sockfd, struct ifreq *ifr, int location,
	   gboolean new_ioctl_nums, guint16 *result)
{
	guint16 *data = (guint16 *) &(ifr->ifr_data);

	data[1] = location;

	if (ioctl (sockfd,
		   new_ioctl_nums ? 0x8948 : SIOCDEVPRIVATE + 1,
		   ifr) < 0) {
		HAL_ERROR (("SIOCGMIIREG on %s failed: %s\n",
			    ifr->ifr_name, strerror (errno)));
		return -1;
	}
	*result = data[3];
	
	return 0;
}

static void
mii_get_rate (HalDevice *d)
{
	const char *ifname;
	int sockfd;
	struct ifreq ifr;
	gboolean new_ioctl_nums;
	int res;
	guint16 link_word;

	ifname = hal_device_property_get_string (d, "net.interface");

	sockfd = socket (AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		HAL_ERROR (("cannot open socket on interface %s; errno=%d",
			    ifname, errno));
		return;
	}

	snprintf (ifr.ifr_name, IFNAMSIZ, ifname);

	if (ioctl (sockfd, 0x8947, &ifr) >= 0)
		new_ioctl_nums = TRUE;
	else if (ioctl (sockfd, SIOCDEVPRIVATE, &ifr) >= 0)
		new_ioctl_nums = FALSE;
	else {
		HAL_ERROR (("SIOCGMIIPHY on %s failed: %s",
			    ifr.ifr_name, strerror (errno)));
		close (sockfd);
		return;
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
	res = mdio_read (sockfd, &ifr, 5, new_ioctl_nums, &link_word);

	if (res < 0) {
		HAL_WARNING (("Error reading rate info"));
	} else if (link_word & 0x380) {
		hal_device_property_set_uint64 (d, "net.80203.rate",
						100 * 1000 * 1000);
	} else if (link_word & 0x60) {
		hal_device_property_set_uint64 (d, "net.80203.rate",
						10 * 1000 * 1000);
	}

	close (sockfd);
}

static void
mii_get_link (HalDevice *d)
{
	const char *ifname;
	int sockfd;
	struct ifreq ifr;
	gboolean new_ioctl_nums;
	int res;
	guint16 status_word;

	ifname = hal_device_property_get_string (d, "net.interface");

	sockfd = socket (AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		HAL_ERROR (("cannot open socket on interface %s; errno=%d",
			    ifname, errno));
		return;
	}

	snprintf (ifr.ifr_name, IFNAMSIZ, ifname);

	if (ioctl (sockfd, 0x8947, &ifr) >= 0)
		new_ioctl_nums = TRUE;
	else if (ioctl (sockfd, SIOCDEVPRIVATE, &ifr) >= 0)
		new_ioctl_nums = FALSE;
	else {
		HAL_ERROR (("SIOCGMIIPHY on %s failed: %s",
			    ifr.ifr_name, strerror (errno)));
		close (sockfd);
		return;
	}

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

	/* We have to read it twice to clear any "sticky" bits */
	res = mdio_read (sockfd, &ifr, 1, new_ioctl_nums, &status_word);
	res = mdio_read (sockfd, &ifr, 1, new_ioctl_nums, &status_word);

	if (res < 0)
		HAL_WARNING (("Error reading link info"));
	else if ((status_word & 0x0016) == 0x0004)
		hal_device_property_set_bool (d, "net.80203.link", TRUE);
	else
		hal_device_property_set_bool (d, "net.80203.link", FALSE);

	/* Also get the link rate */
	mii_get_rate (d);

	close (sockfd);
}

static void
link_detection_handle_message (struct nlmsghdr *hdr)
{
	struct ifinfomsg *ifinfo;
	char ifname[1024];
	struct rtattr *attr;
	int attr_len;
	HalDevice *d;
	const char *hal_ifname;

	ifinfo = NLMSG_DATA (hdr);

	if (hdr->nlmsg_len < NLMSG_LENGTH (sizeof (struct ifinfomsg))) {
		HAL_ERROR (("Packet too small or truncated for ifinfomsg"));
		return;
	}

	memset (&ifname, 0, sizeof (ifname));

	attr = (struct rtattr *) ((unsigned char *)ifinfo + NLMSG_ALIGN (sizeof (struct ifinfomsg)));
	attr_len = NLMSG_PAYLOAD (hdr, sizeof (struct ifinfomsg));

	while (RTA_OK (attr, attr_len)) {
		if (attr->rta_type == IFLA_IFNAME) {
			unsigned int l = RTA_PAYLOAD (attr);

			if (l > sizeof (ifname) - 1)
				l = sizeof (ifname) - 1;

			strncpy (ifname, RTA_DATA (attr), l);
		}

		attr = RTA_NEXT (attr, attr_len);
	}

	/* bail out if there is no interface name */
	if (strlen (ifname) == 0)
		goto out;

	HAL_INFO (("type=0x%02x, SEQ=%d, ifi_flags=0x%04x, ifi_change=0x%04x, ifi_index=%d, ifname='%s'", 
		   hdr->nlmsg_type, 
		   hdr->nlmsg_seq,
		   ifinfo->ifi_flags,
		   ifinfo->ifi_change,
		   ifinfo->ifi_index,
		   ifname));

	/* find hal device object this event applies to */
	d = hal_device_store_match_key_value_int (hald_get_gdl (), "net.linux.ifindex", ifinfo->ifi_index);
	if (d == NULL) {
		HAL_WARNING (("No HAL device object corresponding to ifindex=%d, ifname='%s'",
			      ifinfo->ifi_index, ifname));
		goto out;
	}

	device_property_atomic_update_begin ();
	{

		/* handle link changes */
		if (ifinfo->ifi_flags & IFF_RUNNING) {
			if (hal_device_has_capability (d, "net.80203")) {
				if (!hal_device_property_get_bool (d, "net.80203.link")) {
					hal_device_property_set_bool (d, "net.80203.link", TRUE);
					mii_get_rate (d);
				}
			}
		} else {
			if (hal_device_has_capability (d, "net.80203")) {
				if (hal_device_property_get_bool (d, "net.80203.link")) {
					hal_device_property_set_bool (d, "net.80203.link", FALSE);
					/* only have rate when we have a link */
					hal_device_property_remove (d, "net.80203.rate");
				}
			}
		}
		
		/* handle events for renaming */
		hal_ifname = hal_device_property_get_string (d, "net.interface");
		if (hal_ifname != NULL && strcmp (hal_ifname, ifname) != 0) {
			char new_sysfs_path[256];
			const char *sysfs_path;
			char *p;

			HAL_INFO (("Net interface '%s' renamed to '%s'", hal_ifname, ifname));

			hal_device_property_set_string (d, "net.interface", ifname);

			sysfs_path = hal_device_property_get_string (d, "net.linux.sysfs_path");
			strncpy (new_sysfs_path, sysfs_path, sizeof (new_sysfs_path) - 1);
			p = strrchr (new_sysfs_path, '/');
			if (p != NULL) {
				strncpy (p + 1, ifname, sizeof (new_sysfs_path) - 1 - (p + 1 - new_sysfs_path));
				hal_device_property_set_string (d, "net.linux.sysfs_path", new_sysfs_path);
			}
		}

		/* handle up/down status */
		if (ifinfo->ifi_flags & IFF_UP) {
			if (!hal_device_property_get_bool (d, "net.interface_up")) {
				hal_device_property_set_bool (d, "net.interface_up", TRUE);
			}
		} else {
			if (hal_device_property_get_bool (d, "net.interface_up")) {
				hal_device_property_set_bool (d, "net.interface_up", FALSE);
			}
		}
		
	}
	device_property_atomic_update_end ();

out:
	return;
}

#define VALID_NLMSG(h, s) ((NLMSG_OK (h, s) && \
                           s >= sizeof (struct nlmsghdr) && \
                           s >= h->nlmsg_len))

static gboolean
link_detection_data_ready (GIOChannel *channel, GIOCondition cond,
			   gpointer user_data)
{
	int fd;
	int bytes_read;
	guint total_read = 0;
	struct sockaddr_nl nladdr;
	socklen_t nladdrlen = sizeof(nladdr);
	char buf[1024];

	if (cond & ~(G_IO_IN | G_IO_PRI)) {
		HAL_ERROR (("Error occurred on netlink socket"));
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd (channel);

	do {
		errno = 0;
		bytes_read = recvfrom (fd,
				   buf + total_read,
				   sizeof (buf) - total_read,
				   MSG_DONTWAIT,
				   (struct sockaddr*)&nladdr, &nladdrlen);
		if (nladdrlen != sizeof(nladdr)) {
			HAL_ERROR(("Bad address size reading netlink socket"));
			return FALSE;
		}
		if (nladdr.nl_pid) {
			HAL_ERROR(("Spoofed packet received on netlink socket"));
			return FALSE;
		}
		if (bytes_read > 0)
			total_read += bytes_read;
	} while (bytes_read > 0 || errno == EINTR);

	if (bytes_read < 0 && errno != EAGAIN) {
		HAL_ERROR (("Error reading data off netlink socket"));
		return FALSE;
	}

	if (total_read > 0) {
		struct nlmsghdr *hdr = (struct nlmsghdr *) buf;
		guint offset = 0;

		while (offset < total_read &&
		       VALID_NLMSG (hdr, total_read - offset)) {

			if (hdr->nlmsg_type == NLMSG_DONE)
				break;

			if (hdr->nlmsg_type == RTM_NEWLINK ||
			    hdr->nlmsg_type == RTM_DELLINK)
				link_detection_handle_message (hdr);

			offset += hdr->nlmsg_len;
			hdr = (struct nlmsghdr *) (buf + offset);
		}

		if (offset < total_read &&
		    !VALID_NLMSG (hdr, total_read - offset)) {
			HAL_ERROR (("Packet too small or truncated"));
			return FALSE;
		}
	}

	return TRUE;
}

static void
link_detection_init (HalDevice *d)
{
	static int netlink_fd = -1;
	struct sockaddr_nl addr;
	GIOChannel *channel;

	/* Already opened the socket, no need to do it twice */
	if (netlink_fd >= 0)
		return;

	netlink_fd = socket (PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);

	if (netlink_fd < 0) {
		HAL_ERROR (("Unable to create netlink socket"));
		return;
	}

	memset (&addr, 0, sizeof (addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = getpid ();
	addr.nl_groups = RTMGRP_LINK;

	if (bind (netlink_fd, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		HAL_ERROR (("Unable to bind to netlink socket"));
		return;
	}

	channel = g_io_channel_unix_new (netlink_fd);

	g_io_add_watch (channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_NVAL,
			link_detection_data_ready, NULL);
}

/** This method is called just before the device is either merged
 *  onto the sysdevice or added to the GDL (cf. merge_or_add). 
 *  This is useful for extracting more information about the device
 *  through e.g. ioctl's using the device file property and also
 *  for setting info.category|capability.
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  class_device  Libsysfs object representing class device
 *                        instance
 */
static void 
net_class_pre_process (ClassDeviceHandler *self,
		       HalDevice *d,
		       const char *sysfs_path,
		       struct sysfs_class_device *class_device)
{
	struct sysfs_attribute *attr;
	char *address = NULL;
	int media_type = 0;
	const char *media;
	char wireless_path[SYSFS_PATH_MAX];
	char driver_path[SYSFS_PATH_MAX];
	dbus_bool_t is_80211 = FALSE;
	int ifindex;
	int flags;
	struct stat statbuf;
#if PCMCIA_SUPPORT_ENABLE
 	pcmcia_stab_entry *entry;
#endif

	hal_device_property_set_string (d, "net.linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "net.interface",
					class_device->name);

	/* Check to see if this interface supports wireless extensions */
	is_80211 = FALSE;
	snprintf (wireless_path, SYSFS_PATH_MAX, "%s/wireless", sysfs_path);
	if (stat (wireless_path, &statbuf) == 0) {
		hal_device_add_capability (d, "net.80211");
		is_80211 = TRUE;
	}

	/* Check driver link (may be unavailable for PCMCIA devices) */
	snprintf (driver_path, SYSFS_PATH_MAX, "%s/driver", sysfs_path);
	if (stat (driver_path, &statbuf) == 0) {
		char buf[256];
		memset (buf, '\0', sizeof (buf));
		if (readlink (driver_path, buf, sizeof (buf) - 1) > 0) {
			hal_device_property_set_string (d, "net.linux.driver", get_last_element (buf));
		}
	}

	attr = sysfs_get_classdev_attr (class_device, "address");
	if (attr != NULL) {
		address = g_strstrip (g_strdup (attr->value));
		hal_device_property_set_string (d, "net.address", address);
	}

	attr = sysfs_get_classdev_attr (class_device, "type");
	if (attr != NULL) {
		media_type = parse_dec (attr->value);
	}

	attr = sysfs_get_classdev_attr (class_device, "flags");
	if (attr != NULL) {
		flags = parse_hex (attr->value);
		hal_device_property_set_bool (d, "net.interface_up", flags & IFF_UP);
		if (!is_80211) {
			/* TODO: for some reason IFF_RUNNING isn't exported in flags */
			/*hal_device_property_set_bool (d, "net.80203.link", flags & IFF_RUNNING);*/
			mii_get_link (d);
		}
	}

	attr = sysfs_get_classdev_attr (class_device, "ifindex");
	if (attr != NULL) {
		ifindex = parse_dec (attr->value);
		hal_device_property_set_int (d, "net.linux.ifindex", ifindex);
	}

	/* FIXME: Other address types for non-ethernet devices */
	if (address != NULL && media_type == ARPHRD_ETHER) {
		unsigned int a5, a4, a3, a2, a1, a0;

		if (sscanf (address, "%x:%x:%x:%x:%x:%x",
			    &a5, &a4, &a3, &a2, &a1, &a0) == 6) {
			dbus_uint64_t mac_address;

			mac_address = 
				((dbus_uint64_t)a5<<40) |
				((dbus_uint64_t)a4<<32) | 
				((dbus_uint64_t)a3<<24) | 
				((dbus_uint64_t)a2<<16) | 
				((dbus_uint64_t)a1<< 8) | 
				((dbus_uint64_t)a0<< 0);

			/* TODO: comment out when 64-bit python patch is in dbus */
			hal_device_property_set_uint64 (d, is_80211 ? "net.80211.mac_address" : "net.80203.mac_address",
							mac_address);
		}

	}
	g_free (address);

	if (hal_device_has_property (d, "net.80203.link") &&
	    hal_device_property_get_bool (d, "net.80203.link")) {
		mii_get_rate (d);
	}

	hal_device_property_set_int (d, "net.arp_proto_hw_id", media_type);

	media = media_type_to_string (media_type);
	hal_device_property_set_string (d, "net.media", media);

	hal_device_add_capability (d, "net");
	if (is_80211) {
		hal_device_property_set_string (d, "info.category", "net.80211");
		hal_device_add_capability (d, "net.80211");
	} else {
		hal_device_property_set_string (d, "info.category", "net.80203");
		hal_device_add_capability (d, "net.80203");
	}

#if PCMCIA_SUPPORT_ENABLE
	/* Add PCMCIA specific entries for PCMCIA cards */
	if ((entry = pcmcia_get_stab_entry_for_device (class_device->name))) {
		pcmcia_card_info *info = pcmcia_card_info_get (entry->socket);
		if (info && (info->socket >= 0)) {
			const char *type;
			HalDevice *parent;

			hal_device_property_set_string (d, "info.bus", "pcmcia");
			if (entry->driver)
				hal_device_property_set_string (d, "net.linux.driver", entry->driver);

			if (info->productid_1 && strlen (info->productid_1))
				hal_device_property_set_string (d, "pcmcia.productid_1", info->productid_1);
			if (info->productid_2 && strlen (info->productid_2))
				hal_device_property_set_string (d, "pcmcia.productid_2", info->productid_2);
			if (info->productid_3 && strlen (info->productid_3))
				hal_device_property_set_string (d, "pcmcia.productid_3", info->productid_3);
			if (info->productid_4 && strlen (info->productid_4))
				hal_device_property_set_string (d, "pcmcia.productid_4", info->productid_4);

			if ((type = pcmcia_card_type_string_from_type (info->type)))
				hal_device_property_set_string (d, "pcmcia.function", type);

			hal_device_property_set_int (d, "pcmcia.manfid_1", info->manfid_1);
			hal_device_property_set_int (d, "pcmcia.manfid_2", info->manfid_2);
			hal_device_property_set_int (d, "pcmcia.socket_number", info->socket);

			/* Provide best-guess of vendor, goes in Vendor property; 
			 * .fdi files can override this */
			if (info->productid_1 != NULL) {
				hal_device_property_set_string (d, "info.vendor", info->productid_1);
			} else {
				char namebuf[50];
				snprintf (namebuf, sizeof(namebuf), "Unknown (0x%04x)", info->manfid_1);
				hal_device_property_set_string (d, "info.vendor", namebuf);
			}

			/* Provide best-guess of name, goes in Product property; 
			 * .fdi files can override this */
			if (info->productid_2 != NULL) {
				hal_device_property_set_string (d, "info.product", info->productid_2);
			} else {
				char namebuf[50];
				snprintf (namebuf, sizeof(namebuf), "Unknown (0x%04x)", info->manfid_2);
				hal_device_property_set_string (d, "info.product", namebuf);
			}

			/* Reparent PCMCIA devices to be under their socket */
			parent = hal_device_store_match_key_value_int (hald_get_gdl (), 
									      "pcmcia_socket.number", 
									      info->socket);
			if (parent)
				hal_device_property_set_string (d, "info.parent",
						hal_device_property_get_string (parent, "info.udi"));

		}

		pcmcia_card_info_free (info);
		pcmcia_stab_entry_free (entry);
	}
#endif
}

static dbus_bool_t
net_class_accept (ClassDeviceHandler *self, const char *path,
		  struct sysfs_class_device *class_device)
{
	gboolean accept;
	struct sysfs_attribute *attr = NULL;
#if PCMCIA_SUPPORT_ENABLE
	pcmcia_stab_entry *entry = NULL;
#endif

	accept = TRUE;

	/* If the class name isn't 'net', deny it. */
	if (strcmp (class_device->classname, self->sysfs_class_name) != 0) {
		accept = FALSE;
		goto out;
	}

	/* If we have a sysdevice, allow it. */
	if (class_device->sysdevice != NULL) {
		accept = TRUE;
		goto out;
	}

	/*
	 * Get the "type" attribute from sysfs to see if this is a
	 * network device we're interested in.
	 */
	attr = sysfs_get_classdev_attr (class_device, "type");
	if (attr == NULL || attr->value == NULL) {
		accept = FALSE;
		goto out;
	}

	/* 1 is the type for ethernet (incl. wireless) devices */
	if (attr->value[0] != '1') {
		accept = FALSE;
		goto out;
	}

#if PCMCIA_SUPPORT_ENABLE
	/* Allow 'net' devices without a sysdevice only if they
	 * are backed by real hardware (like PCMCIA cards).
	 */
	if ((entry = pcmcia_get_stab_entry_for_device (class_device->name))) {
		pcmcia_card_info *info = pcmcia_card_info_get (entry->socket);
		if (info && (info->socket >= 0)) {
			/* Ok, we're a PCMCIA card */
			accept = TRUE;
		} else {
			accept = FALSE;
		}
		
		pcmcia_card_info_free (info);
		pcmcia_stab_entry_free (entry);
		goto out;
	} else {
		accept = FALSE;
		goto out;
	}
#endif

out:
	return accept;
}

static void
net_class_post_merge (ClassDeviceHandler *self, HalDevice *d)
{
	link_detection_init (d);
}

static char *
net_class_compute_udi (HalDevice *d, int append_num)
{
	const char *format;
	static char buf[256];

	/*hal_device_print (d);*/

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/net-%s";
	else
		format = "/org/freedesktop/Hal/devices/net-%s-%d";

	snprintf (buf, 256, format,
		  hal_device_property_get_string (d, "net.address"),
		  append_num);

	return buf;
}

static void
net_class_udev_event (ClassDeviceHandler *self, HalDevice *d, 
		      char *dev_file)
{
	/* how rude; udev sends us a device event for the networking device;
	 * ignore it */
	HAL_INFO (("Ignoring udev event for %s", dev_file));
}

/** Method specialisations for input device class */
ClassDeviceHandler net_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	class_device_tick,                  /**< timer function */
	net_class_accept,                   /**< accept function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	net_class_udev_event,               /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	net_class_pre_process,              /**< add more properties */
	net_class_post_merge,               /**< post merge function */
	class_device_got_udi,               /**< got UDI */
	net_class_compute_udi,              /**< compute UDI */
	class_device_in_gdl,                /**< in GDL */
	"net",                              /**< sysfs class name */
	"net",                              /**< hal class name */
	FALSE,                              /**< require device file */
	TRUE                                /**< merge onto sysdevice */
};

/** @} */
