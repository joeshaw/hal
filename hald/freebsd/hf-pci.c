/***************************************************************************
 * CVSID: $Id$
 *
 * hf-pci.c : enumerate PCI devices
 *
 * Copyright (C) 2006, 2007 Jean-Yves Lefort <jylefort@FreeBSD.org>
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
#include <sys/bitstring.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/pciio.h>
#include <glib.h>

#include "../hald.h"
#include "../ids.h"
#include "../device.h"
#include "../logger.h"
#include "../util.h"

#include "hf-pci.h"
#include "hf-devtree.h"
#include "hf-util.h"

#define HF_PCI_DEVICE			"/dev/pci"

/* from sys/dev/pci/pcireg.h */
#define PCIR_SECBUS_1			0x19
#define PCIR_HDRTYPE			0x0e
#define PCIM_HDRTYPE_BRIDGE		0x01
#define PCIM_HDRTYPE_CARDBUS		0x02
#define PCIM_MFDEV                      0x80

typedef struct
{
  HalDevice		*device;
  struct pci_conf	p;
  int			secondary_bus;
  int			header_type;
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
  hal_device_property_set_string(device, "info.subsystem", "pci");
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

static void
hf_pci_probe_bus (HalDevice *parent, int bus, bitstr_t *busmap)
{
  struct pci_match_conf match_conf;
  struct pci_conf_io pc;
  struct pci_conf conf[255];
  struct pci_conf *p;
  GSList *devices = NULL;
  GSList *l;

  g_return_if_fail(busmap != NULL);
  g_return_if_fail(bus >= 0 && bus <= 0xff);

  /* test taken from sysutils/pciutils */
  if (bit_test(busmap, bus))
    {
      HAL_WARNING(("PCI bus %.2x appearing twice (firmware bug), ignored", bus));
      return;
    }

  bit_set(busmap, bus);

 start:
  memset(&match_conf, 0, sizeof(match_conf));
  match_conf.pc_sel.pc_bus = bus;
  match_conf.flags = PCI_GETCONF_MATCH_BUS;

  memset(&pc, 0, sizeof(pc));
  pc.pat_buf_len = sizeof(match_conf);
  pc.num_patterns = 1;
  pc.patterns = &match_conf;
  pc.match_buf_len = sizeof(conf);
  pc.matches = conf;

  /* get the devices located on the specified bus */

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
        {
          DeviceInfo *info;

	  info = g_new(DeviceInfo, 1);
	  info->device = hf_device_store_match(hald_get_gdl(),
			  		       "pci.freebsd.bus",
					       HAL_PROPERTY_TYPE_INT32,
					       p->pc_sel.pc_bus,
					       "pci.freebsd.device",
					       HAL_PROPERTY_TYPE_INT32,
					       p->pc_sel.pc_dev,
					       "pci.freebsd.function",
					       HAL_PROPERTY_TYPE_INT32,
					       p->pc_sel.pc_func,
					       NULL);
	  info->p = *p;
	  info->secondary_bus = hf_pci_get_register(p, PCIR_SECBUS_1);
	  info->header_type = hf_pci_get_register(p, PCIR_HDRTYPE);

	  devices = g_slist_prepend(devices, info);
	}
    }
  while (pc.status == PCI_GETCONF_MORE_DEVS);

  /* add the devices and probe the children of bridges */

  HF_LIST_FOREACH(l, devices)
    {
      DeviceInfo *info = l->data;

      /*
       * If the device already exists, we are reprobing (from
       * osspec_device_reprobe()). In this case we must not add it
       * again, but only probe its children if it is a bridge.
       */
      if (! info->device)
        {
          info->device = hf_pci_device_new(parent, &info->p, info->secondary_bus);
	  if (hf_device_preprobe(info->device))
            hf_device_add(info->device);
	  else
            continue;	/* device ignored */
	}

      if (info->header_type == PCIM_HDRTYPE_BRIDGE || info->header_type == PCIM_HDRTYPE_CARDBUS || (info->header_type & ~PCIM_MFDEV) == PCIM_HDRTYPE_BRIDGE || (info->header_type & ~PCIM_MFDEV) == PCIM_HDRTYPE_CARDBUS)
        /* a bridge or cardbus, probe its children */
        hf_pci_probe_bus(info->device, info->secondary_bus, busmap);
    }

  /* cleanup */

  g_slist_foreach(devices, (GFunc) g_free, NULL);
  g_slist_free(devices);
}

static void
hf_pci_probe (void)
{
  bitstr_t bit_decl(busmap, 256) = { 0 };

  if (hf_pci_fd < 0)
    return;

  hf_pci_probe_bus(NULL, 0, busmap);
}

HFHandler hf_pci_handler = {
  .privileged_init	= hf_pci_privileged_init,
  .probe		= hf_pci_probe
};
