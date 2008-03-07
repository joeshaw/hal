/***************************************************************************
 * CVSID: $Id$
 *
 * hf-net.c : networking device support
 *
 * Copyright (C) 2006 Jean-Yves Lefort <jylefort@FreeBSD.org>
 * Copyright (C) 2006, 2007 Joe Marcus Clarke <marcus@FreeBSD.org>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_media.h>
#include <net/if_mib.h>

#include "../hald_dbus.h"
#include "../logger.h"
#include "../util.h"

#include "hf-net.h"
#include "hf-devtree.h"
#include "hf-util.h"

static gboolean
hf_net_get_link_up (const char *interface)
{
  int fd;
  struct ifreq req;
  gboolean is_up = FALSE;

  g_return_val_if_fail(interface != NULL, FALSE);

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return FALSE;

  memset(&req, 0, sizeof(req));
  strncpy(req.ifr_name, interface, sizeof(req.ifr_name));

  if (ioctl(fd, SIOCGIFFLAGS, &req) != -1 && (req.ifr_flags & IFF_UP) != 0)
    is_up = TRUE;

  close(fd);

  return is_up;
}

static guint64
hf_net_get_rate (int ifindex)
{
  struct ifmibdata ifmd;
  int oid[6];
  size_t len;
  guint64 result;

  oid[0] = CTL_NET;
  oid[1] = PF_LINK;
  oid[2] = NETLINK_GENERIC;
  oid[3] = IFMIB_IFDATA;
  oid[4] = ifindex;
  oid[5] = IFDATA_GENERAL;

  len = sizeof(ifmd);

  if (sysctl(oid, sizeof(oid)/sizeof(int), &ifmd, &len, NULL, 0) == -1)
    result = 0;
  else
    result = ifmd.ifmd_data.ifi_baudrate;

  return result;
}

static void
hf_net_device_set_link_up (HalDevice *device, gboolean is_up)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hal_device_property_set_bool(device, "net.interface_up", is_up);
  if (hal_device_has_capability(device, "net.80203"))
    hal_device_property_set_bool(device, "net.80203.link", is_up);
}

static HalDevice *
hf_net_device_new (const char *interface, HalDevice *parent, GError **err)
{
  char *output;
  char **lines;
  int i;
  int ifindex;
  GError *tmp_err = NULL;
  const char *mac = NULL;
  const char *media = NULL;
  gboolean is_ethernet = FALSE;
  gboolean is_wireless = FALSE;
  gboolean is_tokenring = FALSE;
  HalDevice *device = NULL;

  g_return_val_if_fail(interface != NULL, NULL);
  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  output = hf_run(&tmp_err, "/sbin/ifconfig %s", interface);
  if (! output)
    {
      g_set_error(err, 0, 0, "ifconfig failure: %s", tmp_err->message);
      g_error_free(tmp_err);
      return NULL;
    }

  lines = g_strsplit(output, "\n", 0);
  g_free(output);

  for (i = 0; lines[i]; i++)
    {
      if (g_str_has_prefix(lines[i], "\tether "))
	mac = lines[i] + 7;
      if (g_str_has_prefix(lines[i], "\tmedia: "))
	media = lines[i] + 8;
      if (g_str_has_prefix(lines[i], "\tmedia: Ethernet"))
	is_ethernet = TRUE;
      else if (g_str_has_prefix(lines[i], "\tmedia: IEEE 802.11 Wireless Ethernet"))
        {
          is_ethernet = TRUE;
          is_wireless = TRUE;
        }
      else if (g_str_has_prefix(lines[i], "\tmedia: Token ring"))
        is_tokenring = TRUE;
    }

  device = hf_device_new(parent);

  hf_device_set_udi(device, "net_%s", mac ? mac : hal_util_get_last_element(hal_device_get_udi(parent)));
  hal_device_property_set_string(device, "info.product", "Networking Interface");

  hal_device_add_capability(device, "net");
  hal_device_property_set_string(device, "net.address", mac ? mac : "00:00:00:00:00:00");
  hal_device_property_set_string(device, "net.interface", interface);
  hal_device_property_set_string(device, "net.originating_device", hal_device_get_udi(parent));
  hal_device_property_set_string(device, "net.media", media);
  if (hf_devtree_is_driver(interface, "fwe"))
    hal_device_property_set_int(device, "net.arp_proto_hw_id", ARPHRD_IEEE1394);
  else if (is_ethernet)
    hal_device_property_set_int(device, "net.arp_proto_hw_id", ARPHRD_ETHER);
  else if (is_tokenring)
    hal_device_property_set_int(device, "net.arp_proto_hw_id", ARPHRD_IEEE802);
  /* FIXME Add additional net.arp_proto_hw_id support */

  ifindex = if_nametoindex(interface);
  hal_device_property_set_int(device, "net.freebsd.ifindex", ifindex);

  if (is_ethernet)
    {
      dbus_uint64_t numeric_mac = 0;
      unsigned int a5, a4, a3, a2, a1, a0;

      if (mac && sscanf(mac, "%x:%x:%x:%x:%x:%x", &a5, &a4, &a3, &a2, &a1, &a0) == 6)
	numeric_mac =
	  ((dbus_uint64_t) a5 << 40) |
	  ((dbus_uint64_t) a4 << 32) |
	  ((dbus_uint64_t) a3 << 24) |
	  ((dbus_uint64_t) a2 << 16) |
	  ((dbus_uint64_t) a1 << 8) |
	  ((dbus_uint64_t) a0 << 0);

      if (is_wireless)
        {
          hal_device_property_set_string(device, "info.product", "WLAN Networking Interface");
          hal_device_add_capability(device, "net.80211");
          hal_device_property_set_string(device, "info.category", "net.80211");
          hal_device_property_set_uint64(device, "net.80211.mac_address", numeric_mac);
        }
      else
        {
          hal_device_add_capability(device, "net.80203");
          hal_device_property_set_string(device, "info.category", "net.80203");
          hal_device_property_set_uint64(device, "net.80203.mac_address", numeric_mac);
	  hal_device_property_set_uint64(device, "net.80203.rate", hf_net_get_rate(ifindex));
        }
    }
  else
    hal_device_property_set_string(device, "info.category", "net");

  g_strfreev(lines);

  hf_net_device_set_link_up(device, hf_net_get_link_up(interface));

  return device;
}

static gboolean
hf_net_update_timeout_cb (gpointer data)
{
  GSList *l;

  if (hf_is_waiting)
    return TRUE;

  HF_LIST_FOREACH(l, hald_get_gdl()->devices)
    {
      HalDevice *device = l->data;
      const char *interface;

      interface = hal_device_property_get_string(device, "net.interface");
      if (interface)
	{
	  device_property_atomic_update_begin();
	  hf_net_device_set_link_up(device, hf_net_get_link_up(interface));
	  device_property_atomic_update_end();
	}
    }

  return TRUE;
}

static void
hf_net_init (void)
{
  g_timeout_add(3000, hf_net_update_timeout_cb, NULL);
}

static void
hf_net_probe (void)
{
  GError *err = NULL;
  char *output;
  char *terminator;
  char **interfaces;
  int i;

  output = hf_run(&err, "/sbin/ifconfig -l");
  if (! output)
    {
      HAL_WARNING(("ifconfig failure: %s", err->message));
      g_error_free(err);
      return;
    }

  terminator = strrchr(output, '\n');
  if (terminator)
    *terminator = 0;

  interfaces = g_strsplit(output, " ", 0);
  g_free(output);

  for (i = 0; interfaces[i]; i++)
    if (! hal_device_store_match_key_value_string(hald_get_gdl(), "net.interface", interfaces[i]))
      {
	HalDevice *parent;

	parent = hf_devtree_find_from_name(hald_get_gdl(), interfaces[i]);
	if (parent && ! hal_device_property_get_bool(parent, "info.ignore"))
	  {
	    HalDevice *device;

	    device = hf_net_device_new(interfaces[i], parent, &err);
	    if (device)
	      hf_device_preprobe_and_add(device);
	    else
	      {
		HAL_WARNING(("unable to handle network interface %s: %s", interfaces[i], err->message));
		g_clear_error(&err);
	      }
	  }
      }
  g_strfreev(interfaces);
}

static gboolean
hf_net_devd_add (const char *name,
		 GHashTable *params,
		 GHashTable *at,
		 const char *parent)
{
  int s;
  gboolean consumed = FALSE;

  /* Adapted code from devd.cc to find out if this is a network
   * interface. */

  s = socket(PF_INET, SOCK_DGRAM, 0);
  if (s >= 0)
    {
      struct ifmediareq ifmr;

      memset(&ifmr, 0, sizeof(ifmr));
      strncpy(ifmr.ifm_name, name, sizeof(ifmr.ifm_name));

      if (ioctl(s, SIOCGIFMEDIA, (caddr_t) &ifmr) >= 0
	  && (ifmr.ifm_status & IFM_AVALID) != 0)
	{
	  HAL_INFO(("found new network interface: %s", name));
	  hf_net_probe();
	  consumed = TRUE;
	}
      close(s);
    }

  return consumed;
}

static gboolean
hf_net_devd_remove (const char *name,
		    GHashTable *params,
		    GHashTable *at,
		    const char *parent)
{
  HalDevice *device;

  /*
   * If a network driver was detached, do not let hf-devd remove the
   * physical device, just remove the interface device.
   */
  device = hal_device_store_match_key_value_string(hald_get_gdl(), "net.interface", name);
  if (device)
    {
      hf_device_remove_tree(device);
      return TRUE;
    }

  return FALSE;
}

static gboolean
hf_net_devd_notify (const char *system,
		    const char *subsystem,
		    const char *type,
		    const char *data)
{
  HalDevice *device;

  if (strcmp(system, "IFNET"))
    return FALSE;

  device = hal_device_store_match_key_value_string(hald_get_gdl(), "net.interface", subsystem);
  if (device)
    {
      gboolean is_up;

      if (! strcmp(type, "LINK_UP"))
	is_up = TRUE;
      else if (! strcmp(type, "LINK_DOWN"))
	is_up = FALSE;
      else
	return TRUE;

      device_property_atomic_update_begin();
      hf_net_device_set_link_up(device, is_up);
      device_property_atomic_update_end();
    }

  return TRUE;
}

HFHandler hf_net_handler = {
  .init =	hf_net_init,
  .probe =	hf_net_probe
};

HFDevdHandler hf_net_devd_handler = {
  .add =	hf_net_devd_add,
  .remove =	hf_net_devd_remove,
  .notify =	hf_net_devd_notify
};
