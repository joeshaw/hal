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

#ifdef HAVE_IWLIB
#  include <linux/wireless.h>
#  include <iwlib.h>
#else
#  include <net/if.h>
#endif

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "class_device.h"
#include "common.h"

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
		hal_device_property_set_int (d, "net.ethernet.rate",
					     100 * 1000 * 1000);
	} else if (link_word & 0x60) {
		hal_device_property_set_int (d, "net.ethernet.rate",
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
		hal_device_property_set_bool (d, "net.ethernet.link", TRUE);
	else
		hal_device_property_set_bool (d, "net.ethernet.link", FALSE);

	/* Also get the link rate */
	mii_get_rate (d);

	close (sockfd);
}

static void
link_detection_handle_message (struct nlmsghdr *hdr, HalDevice *d)
{
	struct ifinfomsg *ifinfo;
	char ifname[1024];
	struct rtattr *attr;
	int attr_len;

	ifinfo = NLMSG_DATA (hdr);

	if (hdr->nlmsg_len < NLMSG_LENGTH (sizeof (struct ifinfomsg))) {
		HAL_ERROR (("Packet too small or truncated for ifinfomsg"));
		return;
	}

	memset (&ifname, 0, sizeof (ifname));

	attr = (void *) ifinfo + NLMSG_ALIGN (sizeof (struct ifinfomsg));
	attr_len = NLMSG_PAYLOAD (hdr, sizeof (struct ifinfomsg));

	while (RTA_OK (attr, attr_len)) {
		if (attr->rta_type == IFLA_IFNAME) {
			int l = RTA_PAYLOAD (attr);

			if (l > 1023)
				l = 1023;

			strncpy (ifname, RTA_DATA (attr), l);
		}

		attr = RTA_NEXT (attr, attr_len);
	}

	hal_device_property_set_bool (d, "net.ethernet.link",
				      ifinfo->ifi_flags & IFF_RUNNING ?
				      TRUE : FALSE);

	/*
	 * Check the MII registers to set our link rate if we haven't set
	 * it previously.
	 */
	if (!hal_device_has_property (d, "net.ethernet.rate"))
		mii_get_rate (d);
}

#define VALID_NLMSG(h, s) ((NLMSG_OK (h, s) && \
                           s >= sizeof (struct nlmsghdr) && \
                           s >= h->nlmsg_len))

static gboolean
link_detection_data_ready (GIOChannel *channel, GIOCondition cond,
			   gpointer user_data)
{
	HalDevice *d = HAL_DEVICE (user_data);
	int fd;
	int bytes_read;
	guint total_read = 0;
	char buf[1024];

	if (cond & ~(G_IO_IN | G_IO_PRI)) {
		HAL_ERROR (("Error occurred on netlink socket"));
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd (channel);

	do {
		errno = 0;
		bytes_read = recv (fd,
				   buf + total_read,
				   sizeof (buf) - total_read,
				   MSG_DONTWAIT);

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
				link_detection_handle_message (hdr, d);

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
	int fd;
	struct sockaddr_nl addr;
	GIOChannel *channel;

	fd = socket (PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);

	if (fd < 0) {
		HAL_ERROR (("Unable to create netlink socket"));
		return;
	}

	memset (&addr, 0, sizeof (addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = getpid ();
	addr.nl_groups = RTMGRP_LINK;

	if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		HAL_ERROR (("Unable to bind to netlink socket"));
		return;
	}

	channel = g_io_channel_unix_new (fd);

	g_io_add_watch (channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_NVAL,
			link_detection_data_ready, d);
}

#ifdef HAVE_IWLIB
static void
open_wireless_sysfs_subdir (HalDevice *d, const char *sysfs_path)
{
	char wireless_path[SYSFS_PATH_MAX];
	struct sysfs_directory *dir;
	struct sysfs_attribute *cur;

	snprintf (wireless_path, SYSFS_PATH_MAX, "%s/wireless", sysfs_path);
	dir = sysfs_open_directory (wireless_path);

	/* will fail if the directory doesn't exist */
	if (sysfs_read_directory (dir) < 0)
		return;

	/* dir exists but is empty */
	if (dir->attributes == NULL)
		return;

	dlist_for_each_data (dir->attributes, cur, struct sysfs_attribute) {
		char attr_name[SYSFS_NAME_LEN];
		int len, i;
		int tmp;

		if (sysfs_get_name_from_path (cur->path, attr_name,
					      SYSFS_NAME_LEN) != 0)
			continue;

		/* strip whitespace */
		len = strlen (cur->value);
		for (i = len - 1; i >= 0 && isspace (cur->value[i]); --i)
			cur->value[i] = '\0';

		if (strcmp (attr_name, "level") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_int (d,
						     "net.ethernet.80211.level",
						     tmp);
		} else if (strcmp (attr_name, "link") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_int (d, "net.ethernet.80211.link",
						     tmp);
		} else if (strcmp (attr_name, "noise") == 0) {
			tmp = parse_dec (cur->value);

			hal_device_property_set_int (d, "net.ethernet.80211.noise",
						     tmp);
		} else if (strcmp (attr_name, "status") == 0) {
			tmp = parse_hex (cur->value);

			hal_device_property_set_int (d, "net.ethernet.80211.status",
						     tmp);
		}
	}

	sysfs_close_directory (dir);

	hal_device_add_capability (d, "net.ethernet.80211");
}

typedef struct {
	char address[128];
	float freq;
	char essid[IW_ESSID_MAX_SIZE + 1];
	int link, level, noise;
} APInfo;

static int
ap_compare (gconstpointer a, gconstpointer b)
{
	APInfo *ap_a = (APInfo *) a;
	APInfo *ap_b = (APInfo *) b;

	return strcmp (ap_a->essid, ap_b->essid);
}

static void
hash_to_list (gpointer key, gpointer value, gpointer user_data)
{
	GSList **list = (GSList **) user_data;

	*list = g_slist_prepend (*list, value);
}

static void
aps_to_properties (HalDevice *d, GSList *aps)
{
	GSList *iter;
	GHashTable *networks;
	int ap_num = 0;
	GSList *network_list = NULL;
	int i;

	networks = g_hash_table_new_full (g_str_hash, g_str_equal,
					  NULL, g_free);

	for (iter = aps; iter != NULL; iter = iter->next) {
		APInfo *ap = (APInfo *) iter->data;
		APInfo *best_ap;

		best_ap = g_hash_table_lookup (networks, ap->essid);

		if (best_ap != NULL) {
			if (ap->link > best_ap->link)
				g_hash_table_replace (networks, ap->essid, ap);
			else
				g_free (ap);
		} else
			g_hash_table_insert (networks, ap->essid, ap);
	}
	
	hal_device_property_set_int (d, "net.ethernet.80211.available_networks",
				     g_hash_table_size (networks));

	g_hash_table_foreach (networks, hash_to_list, &network_list);
	network_list = g_slist_sort (network_list, ap_compare);

	for (iter = network_list; iter != NULL; iter = iter->next) {
		APInfo *ap = (APInfo *) iter->data;
		char prop_name[256];

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.address",
			  ap_num);

		hal_device_property_set_string (d, prop_name, ap->address);

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.frequency",
			  ap_num);

		hal_device_property_set_double (d, prop_name, ap->freq);

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.essid",
			  ap_num);

		hal_device_property_set_string (d, prop_name, ap->essid);

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.link",
			  ap_num);

		hal_device_property_set_int (d, prop_name, ap->link);

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.level",
			  ap_num);

		hal_device_property_set_int (d, prop_name, ap->level);

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.noise",
			  ap_num);

		hal_device_property_set_int (d, prop_name, ap->noise);

		ap_num++;
	}

	g_slist_free (aps);
	g_hash_table_destroy (networks);

	/* 
	 * Clean out old properties.  There'll probably never be more than 64
	 * networks, right?
	 */
	for (i = ap_num; i < 64; i++) {
		char prop_name[256];

		snprintf (prop_name, 256, "net.ethernet.80211.network%d.essid", i);

		if (hal_device_has_property (d, prop_name)) {
			char *prop_names[] = {
				"address", "essid", "frequency",
				"link", "level", "noise", NULL };
			char **c;

			for (c = prop_names; *c != NULL; c++) {
				snprintf (prop_name, 256,
					  "net.ethernet.80211.network%d.%s",
					  i, *c);

				hal_device_property_remove (d, prop_name);
			}
		} else
			break;
	}
}

static APInfo *
parse_scanning_token (struct iw_event *iwe, APInfo *old_ap)
{
	APInfo *ap;
	float val;

	if (iwe->cmd == SIOCGIWAP)
		ap = g_new0 (APInfo, 1);
	else {
		g_assert (old_ap != NULL);
		ap = old_ap;
	}

	switch (iwe->cmd) {
	case SIOCGIWAP:
		memset (ap->address, 0, 128);
		iw_pr_ether (ap->address, iwe->u.ap_addr.sa_data);
		break;

	case SIOCGIWFREQ:
		/*
		 * If the value is less than 1000, then it's the channel.
		 * Otherwise, it's the frequency.  I swear to god the
		 * iwlib code is like this.
		 */
		val = iw_freq2float(&(iwe->u.freq));
		
		if (val > 1000)
			ap->freq = val;
		break;

	case SIOCGIWESSID:
		memcpy (ap->essid, iwe->u.essid.pointer,
			IW_ESSID_MAX_SIZE + 1);
		ap->essid[iwe->u.essid.length] = 0;
		break;

	case IWEVQUAL:
		ap->link = iwe->u.qual.qual;
		ap->level = iwe->u.qual.level;
		ap->noise = iwe->u.qual.noise;
		break;
	}

	return ap;
}	

typedef struct {
	HalDevice *d;
	int skfd;
} ScanningInfo;

static gboolean
read_scanning_results (gpointer user_data)
{
	ScanningInfo *si = user_data;
	struct iwreq wrq;
	char buffer[IW_SCAN_MAX_DATA];

	wrq.u.data.pointer = buffer;
	wrq.u.data.length = IW_SCAN_MAX_DATA;
	wrq.u.data.flags = 0;

	strncpy (wrq.ifr_name,
		 hal_device_property_get_string (si->d, "net.interface"),
		 IFNAMSIZ);

	if (ioctl (si->skfd, SIOCGIWSCAN, &wrq) < 0) {
		if (errno == EAGAIN) {
			/* Results aren't ready yet.  Requeue. */
			return TRUE;
		} else {
			goto cleanup;
		}
	}

	if (wrq.u.data.length > 0) {
		struct iw_event iwe;
		struct stream_descr stream;
		int ret;
		GSList *aps = NULL;
		APInfo *old_ap = NULL, *ap;

		iw_init_event_stream (&stream, buffer, wrq.u.data.length);
		do {
			ret = iw_extract_event_stream (&stream, &iwe);

			if (ret > 0) {
				ap = parse_scanning_token (&iwe, old_ap);

				if (ap != old_ap)
					aps = g_slist_prepend (aps, ap);

				old_ap = ap;
			}
		} while (ret > 0);

		aps_to_properties (si->d, aps);
	}

cleanup:
	g_object_unref (si->d);
	close (si->skfd);
	g_free (si);

	return FALSE;
}

static void
get_wireless_properties (HalDevice *d, const char *sysfs_path)
{
	int skfd;
	const char *iface;
	struct iwreq wrq;
	char essid[IW_ESSID_MAX_SIZE + 1];
	char key[IW_ENCODING_TOKEN_MAX];
	gboolean close_skfd = TRUE;

	open_wireless_sysfs_subdir (d, sysfs_path);

	skfd = iw_sockets_open ();
	if (skfd < 0)
		return;

	iface = hal_device_property_get_string (d, "net.interface");

	/* Wireless protocol */
	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCGIWNAME, &wrq) < 0) {
		/* no wireless extensions */
		close (skfd);
		return;
	}

	hal_device_property_set_bool (d, "net.ethernet.is_80211", TRUE);

	wrq.u.name[IFNAMSIZ] = 0;
	hal_device_property_set_string (d, "net.ethernet.80211.protocol",
					wrq.u.name);

	/* Frequency */
	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCGIWFREQ, &wrq) >= 0) {
		hal_device_property_set_double (d, "net.ethernet.80211.frequency",
						iw_freq2float(&(wrq.u.freq)));
	}

	/* Crypto info */
	memset (key, 0, IW_ENCODING_TOKEN_MAX);
	wrq.u.data.pointer = (caddr_t) key;
	wrq.u.data.length = IW_ENCODING_TOKEN_MAX;
	wrq.u.data.flags = 0;

	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCGIWENCODE, &wrq) >= 0) {
		hal_device_property_set_string (d, "net.ethernet.80211.key", key);
	}

	/* ESSID */
	memset (essid, 0, IW_ESSID_MAX_SIZE + 1);
	wrq.u.essid.pointer = (caddr_t) essid;
	wrq.u.essid.length = IW_ESSID_MAX_SIZE + 1;
	wrq.u.essid.flags = 0;

	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCGIWESSID, &wrq) >= 0) {
		hal_device_property_set_string (d, "net.ethernet.80211.essid",
						essid);
	}

	/* Mode (ad-hoc, managed, etc) */
	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCGIWMODE, &wrq) >= 0) {
		/* stolen from iwlib */
		const char *modes[] = { "auto", "ad-hoc", "managed",
					"master", "repeater", "secondary",
					"monitor" };

		hal_device_property_set_int (d, "net.ethernet.80211.mode",
					     wrq.u.mode);
		hal_device_property_set_string (d, "net.ethernet.80211.mode_str",
						modes[wrq.u.mode]);
	}

	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCGIWAP, &wrq) >= 0) {
		char ap_addr[128];

		memset (ap_addr, 0, 128);
		iw_pr_ether (ap_addr, wrq.u.ap_addr.sa_data);

		hal_device_property_set_string (d, "net.ethernet.80211.ap_address",
						ap_addr);
	}
	
	/* Scan for other access points */
	strncpy (wrq.ifr_name, iface, IFNAMSIZ);
	if (ioctl (skfd, SIOCSIWSCAN, &wrq) >= 0) {
		ScanningInfo *si = g_new0 (ScanningInfo, 1);
		
		si->d = g_object_ref (d);
		si->skfd = skfd;
		close_skfd = FALSE;
			
		g_timeout_add (100, read_scanning_results, si);
	}
	
	if (close_skfd)
		close (skfd);

	hal_device_add_capability (d, "net.ethernet.80211");
}
#endif

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

	hal_device_property_set_string (d, "net.linux.sysfs_path", sysfs_path);
	hal_device_property_set_string (d, "net.interface",
					class_device->name);

	attr = sysfs_get_classdev_attr (class_device, "address");

	if (attr != NULL) {
		address = g_strstrip (g_strdup (attr->value));
		hal_device_property_set_string (d, "net.address", address);
	}

	attr = sysfs_get_classdev_attr (class_device, "type");

	if (attr != NULL)
		media_type = parse_dec (attr->value);

	/* FIXME: Other address types for non-ethernet devices */
	if (address != NULL && media_type == ARPHRD_ETHER) {
		unsigned int a5, a4, a3, a2, a1, a0;

		hal_device_property_set_string (d, "net.ethernet.mac_addr",
						address);

		if (sscanf (address, "%x:%x:%x:%x:%x:%x",
			    &a5, &a4, &a3, &a2, &a1, &a0) == 6) {
			dbus_uint32_t mac_upper, mac_lower;

			mac_upper = (a5 << 16) | (a4 << 8) | a3;
			mac_lower = (a2 << 16) | (a1 << 8) | a0;

			hal_device_property_set_int (d,
						     "net.ethernet.mac_addr_upper24",
						     (dbus_int32_t) mac_upper);
			hal_device_property_set_int (d,
						     "net.ethernet.mac_addr_lower24",
						     (dbus_int32_t) mac_lower);
		}

		/* Get the initial link state from the MII registers */
		mii_get_link (d);
	}

	g_free (address);

	hal_device_property_set_int (d, "net.arp_proto_hw_id", media_type);

	media = media_type_to_string (media_type);
	hal_device_property_set_string (d, "net.media", media);

#ifdef HAVE_IWLIB
	/* read any wireless properties */
	get_wireless_properties (d, sysfs_path);
#endif

	hal_device_add_capability (d, "net");
	hal_device_add_capability (d, "net.ethernet");
	hal_device_property_set_string (d, "info.category", "net.ethernet");
}

static dbus_bool_t
net_class_accept (ClassDeviceHandler *self, const char *path,
		  struct sysfs_class_device *class_device)
{
	struct sysfs_attribute *attr;

	/* If the class name isn't 'net', deny it. */
	if (strcmp (class_device->classname, self->sysfs_class_name) != 0)
		return FALSE;

	/* If we have a sysdevice, allow it. */
	if (class_device->sysdevice != NULL)
		return TRUE;

	/*
	 * Get the "type" attribute from sysfs to see if this is a
	 * network device we're interested in.
	 */
	attr = sysfs_get_classdev_attr (class_device, "type");

	if (attr == NULL || attr->value == NULL)
		return FALSE;

	/* 1 is the type for ethernet (incl. wireless) devices */
	if (attr->value[0] == '1')
		return TRUE;
	else
		return FALSE;
}

static void
net_class_post_merge (ClassDeviceHandler *self, HalDevice *d)
{
	if (hal_device_has_capability (d, "net.ethernet"))
		link_detection_init (d);
}

static char *
net_class_compute_udi (HalDevice *d, int append_num)
{
	const char *format;
	static char buf[256];

	hal_device_print (d);

	if (append_num == -1)
		format = "/org/freedesktop/Hal/devices/net-%s";
	else
		format = "/org/freedesktop/Hal/devices_net-%s-%d";

	snprintf (buf, 256, format,
		  hal_device_property_get_string (d, "net.address"),
		  append_num);

	return buf;
}

#ifdef HAVE_IWLIB
static gboolean
rehash_wireless (HalDeviceStore *gdl, HalDevice *d, gpointer user_data)
{
	const char *sysfs_path;

	if (!hal_device_has_property (d, "net.ethernet.is_80211"))
		return TRUE;

	if (hal_device_property_get_bool (d, "net.ethernet.is_80211") == FALSE)
		return TRUE;

	sysfs_path = hal_device_property_get_string (d, "linux.sysfs_path");

	get_wireless_properties (d, sysfs_path);

	return TRUE;
}
#endif

static void
net_class_tick (ClassDeviceHandler *self)
{
#ifdef HAVE_IWLIB
	/*
	 * We only want this to happen once every 10 seconds, not the
	 * normal 2.
	 */
	static int tick_count = 0;

	tick_count++;

	if (tick_count >= 5) {
		hal_device_store_foreach (hald_get_gdl (), rehash_wireless,
					  NULL);
		tick_count = 0;
	}
#endif
}

/** Method specialisations for input device class */
ClassDeviceHandler net_class_handler = {
	class_device_init,                  /**< init function */
	class_device_shutdown,              /**< shutdown function */
	net_class_tick,                     /**< timer function */
	net_class_accept,                   /**< accept function */
	class_device_visit,                 /**< visitor function */
	class_device_removed,               /**< class device is removed */
	class_device_udev_event,            /**< handle udev event */
	class_device_get_device_file_target,/**< where to store devfile name */
	net_class_pre_process,              /**< add more properties */
	net_class_post_merge,               /**< post merge function */
	class_device_got_udi,               /**< got UDI */
	net_class_compute_udi,              /**< compute UDI */
	"net",                              /**< sysfs class name */
	"net",                              /**< hal class name */
	FALSE,                              /**< require device file */
	TRUE                                /**< merge onto sysdevice */
};

/** @} */
