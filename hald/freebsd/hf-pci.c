/***************************************************************************
 * CVSID: $Id$
 *
 * hf-pci.c : enumerate PCI devices
 *
 * Copyright (C) 2006 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/pciio.h>
#include <glib.h>

#include "../hald.h"
#include "../ids.h"
#include "../logger.h"
#include "../util.h"

#include "hf-pci.h"
#include "hf-devtree.h"
#include "hf-util.h"

#define HF_PCI_DEVICE			"/dev/pci"

/* from sys/dev/pci/pcireg.h */
#define PCIR_SECBUS_1			0x19

typedef struct
{
  struct pci_conf	p;
  int			secondary_bus;
} DeviceInfo;

static int hf_pci_fd;

static int
hf_pci_get_register (const struct pci_conf *p, int reg)
{
  struct pci_io io;

  g_return_val_if_fail(p != NULL, 0);

  memset(&io, 0, sizeof(io));
  io.pi_sel = p->pc_sel;
  io.pi_reg = reg;
  io.pi_width = 1;

  if (ioctl(hf_pci_fd, PCIOCREAD, &io) < 0)
    {
      HAL_WARNING(("unable to read register %.2x of PCI device %i:%i:%i", reg, p->pc_sel.pc_bus, p->pc_sel.pc_dev, p->pc_sel.pc_func));
      return 0;
    }

  return io.pi_data;
}

static HalDevice *
hf_pci_device_new (HalDevice *parent, const struct pci_conf *p, int secondary_bus)
{
  HalDevice *device;
  char *vendor;
  char *product;
  char *subsys_vendor;
  char *subsys_product;

  g_return_val_if_fail(p != NULL, NULL);

  device = hf_device_new(parent);

  hf_device_set_udi(device, "pci_%.4x_%.4x", p->pc_vendor, p->pc_device);
  hal_device_property_set_string(device, "info.bus", "pci");
  hal_device_property_set_int(device, "pci.device_class", p->pc_class);
  hal_device_property_set_int(device, "pci.device_subclass", p->pc_subclass);
  hal_device_property_set_int(device, "pci.device_protocol", p->pc_progif);
  hal_device_property_set_int(device, "pci.product_id", p->pc_device);
  hal_device_property_set_int(device, "pci.vendor_id", p->pc_vendor);
  hal_device_property_set_int(device, "pci.subsys_product_id", p->pc_subdevice);
  hal_device_property_set_int(device, "pci.subsys_vendor_id", p->pc_subvendor);

  if (p->pd_name && *p->pd_name)
    hf_devtree_device_set_info(device, p->pd_name, p->pd_unit);

  hal_device_property_set_int(device, "pci.freebsd.bus", p->pc_sel.pc_bus);
  hal_device_property_set_int(device, "pci.freebsd.device", p->pc_sel.pc_dev);
  hal_device_property_set_int(device, "pci.freebsd.function", p->pc_sel.pc_func);
  hal_device_property_set_int(device, "pci.freebsd.secondary_bus", secondary_bus);

  ids_find_pci(p->pc_vendor, p->pc_device,
	       p->pc_subvendor, p->pc_subdevice,
	       &vendor, &product,
	       &subsys_vendor, &subsys_product);

  if (vendor)
    {
      hal_device_property_set_string(device, "info.vendor", vendor);
      hal_device_property_set_string(device, "pci.vendor", vendor);
    }
  if (product)
    {
      hal_device_property_set_string(device, "info.product", product);
      hal_device_property_set_string(device, "pci.product", product);
    }
  if (subsys_vendor)
    hal_device_property_set_string(device, "pci.subsys_vendor", subsys_vendor);
  if (subsys_product)
    hal_device_property_set_string(device, "pci.subsys_product", subsys_product);

  return device;
}

static void
hf_pci_privileged_init (void)
{
  hf_pci_fd = open(HF_PCI_DEVICE, O_RDWR);
  if (hf_pci_fd < 0)
    HAL_INFO(("unable to open %s: %s", HF_PCI_DEVICE, g_strerror(errno)));
}

static GSList *
hf_pci_lookup (GSList *devices, int bus)
{
  if (bus != 0)
    {
      GSList *l;

      HF_LIST_FOREACH(l, devices)
	{
	  DeviceInfo *info = l->data;

	  if (info->secondary_bus == bus)
	    return l;
	}
    }

  return NULL;
}

static GSList *
hf_pci_get_root (GSList *devices)
{
  GSList *root;
  DeviceInfo *info;
  int bus;

  g_return_val_if_fail(devices != NULL, NULL);

  root = devices;
  info = root->data;

  bus = info->p.pc_sel.pc_bus;
  while (bus != -1)
    {
      GSList *new_root;

      new_root = hf_pci_lookup(devices, bus);
      bus = -1;

      if (new_root)
	{
	  root = new_root;
	  info = root->data;
	  bus = info->p.pc_sel.pc_bus;
	}
    }

  return root;
}

static void
hf_pci_probe (void)
{
  struct pci_conf_io pc;
  struct pci_conf conf[255];
  struct pci_conf *p;
  GSList *devices = NULL;

  if (hf_pci_fd < 0)
    return;

 start:
  memset(&pc, 0, sizeof(pc));
  pc.match_buf_len = sizeof(conf);
  pc.matches = conf;

  /* build a list of PCI devices */

  do
    {
      if (ioctl(hf_pci_fd, PCIOCGETCONF, &pc) < 0)
	{
	  HAL_WARNING(("ioctl PCIOCGETCONF: %s", g_strerror(errno)));
	  break;
	}

      if (pc.status == PCI_GETCONF_LIST_CHANGED)
	{
	  g_slist_foreach(devices, (GFunc) g_free, NULL);
	  g_slist_free(devices);
	  devices = NULL;
	  goto start;
	}
      else if (pc.status == PCI_GETCONF_ERROR)
	{
	  HAL_WARNING(("PCI_GETCONF_ERROR"));
	  break;
	}

      for (p = conf; p < &conf[pc.num_matches]; p++)
	  if (! hf_device_store_match(hald_get_gdl(),
                                      "pci.freebsd.bus", HAL_PROPERTY_TYPE_INT32, p->pc_sel.pc_bus,
				      "pci.freebsd.device", HAL_PROPERTY_TYPE_INT32, p->pc_sel.pc_dev,
				      "pci.freebsd.function", HAL_PROPERTY_TYPE_INT32, p->pc_sel.pc_func,
				      NULL))
	    {
	      DeviceInfo *info;

	      info = g_new(DeviceInfo, 1);
	      info->p = *p;
	      info->secondary_bus = hf_pci_get_register(p, PCIR_SECBUS_1);

	      devices = g_slist_prepend(devices, info);
	    }
    }
  while (pc.status == PCI_GETCONF_MORE_DEVS);

  /* add the devices (parents first) */

  while (devices)
    {
      GSList *root;
      DeviceInfo *info;
      HalDevice *parent = NULL;

      root = hf_pci_get_root(devices);
      g_assert(root != NULL);

      info = root->data;

      if (info->p.pc_sel.pc_bus != 0)
	parent = hal_device_store_match_key_value_int(hald_get_gdl(), "pci.freebsd.secondary_bus", info->p.pc_sel.pc_bus);

      if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
	{
	  HalDevice *device;

	  device = hf_pci_device_new(parent, &info->p, info->secondary_bus);
	  hf_device_preprobe_and_add(device);
	}

      devices = g_slist_delete_link(devices, root);
      g_free(info);
    }
}

HFHandler hf_pci_handler = {
  .privileged_init	= hf_pci_privileged_init,
  .probe		= hf_pci_probe
};
