/***************************************************************************
 * CVSID: $Id$
 *
 * hf-ata.c : ATA support
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

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/ata.h>

#include "../logger.h"

#include "hf-ata.h"
#include "hf-block.h"
#include "hf-devtree.h"
#include "hf-pci.h"
#include "hf-storage.h"
#include "hf-util.h"

#define HF_ATA_DEVICE			"/dev/ata"

static int hf_ata_fd;

GList *hf_ata_pending_devices = NULL;

/* adapted from ad_describe() in sys/dev/ata/atadisk.c */
static char *
hf_ata_get_vendor (const char *model)
{
  char *sep;

  g_return_val_if_fail(model != NULL, NULL);

  sep = strpbrk(model, " -");
  if (sep)
    return g_strndup(model, sep - model);
  else if (g_str_has_prefix(model, "ST"))
    return g_strdup("Seagate");
  else
    return NULL;
}

static HalDevice *
hf_ata_ide_device_new (HalDevice *parent, int ms)
{
  HalDevice *device;
  int host;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  device = hf_device_new(parent);

  host = hal_device_property_get_int(parent, "ide_host.number");
  hf_device_set_udi(device, "ide_%i_%i", host, ms);

  hal_device_property_set_string(device, "info.subsystem", "ide");
  hf_device_property_set_string_printf(device, "info.product", "IDE Device (%s)", ms == 0 ? "Master" : "Slave");

  hal_device_property_set_int(device, "ide.host", host);
  hal_device_property_set_int(device, "ide.channel", ms);

  return device;
}

static HalDevice *
hf_ata_block_device_new (HalDevice *parent,
			 int ms,
#ifdef IOCATADEVICES
			 const struct ata_ioc_devices *devices)
#else
                         const struct ata_cmd *devices)
#endif
{
  HalDevice *device;
  const struct ata_params *params;
  char *vendor;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);
  g_return_val_if_fail(devices != NULL, NULL);

#ifdef IOCATADEVICES
  params = &devices->params[ms];
#else
  params = &devices->u.param.params[ms];
#endif
  vendor = hf_ata_get_vendor(params->model);

  device = hf_device_new(parent);

#ifdef IOCATADEVICES
  hf_devtree_device_set_name(device, devices->name[ms]);
#else
  hf_devtree_device_set_name(device, devices->u.param.name[ms]);
#endif

#ifdef IOCATADEVICES
  hf_block_device_enable(device, devices->name[ms]);
#else
  hf_block_device_enable(device, devices->u.param.name[ms]);
#endif
  hf_storage_device_enable(device);

  hal_device_property_set_string(device, "info.product", params->model);
  if (vendor)
    hal_device_property_set_string(device, "info.vendor", vendor);

  if (params->satacapabilities && params->satacapabilities != 0xffff)
    hal_device_property_set_string(device, "storage.bus", "sata");
  else
    hal_device_property_set_string(device, "storage.bus", "ide");

  switch (params->config & ATA_ATAPI_TYPE_MASK)
    {
    case ATA_ATAPI_TYPE_TAPE:
      hf_storage_device_enable_tape(device);
      break;

    case ATA_ATAPI_TYPE_CDROM:
    case ATA_ATAPI_TYPE_OPTICAL:
      hf_storage_device_enable_cdrom(device);
      break;
    }

  if ((params->support.command1 & ATA_SUPPORT_REMOVABLE) != 0)
    hal_device_property_set_bool(device, "storage.removable", TRUE);

  hal_device_property_set_string(device, "storage.originating_device", hal_device_get_udi(parent));
  hal_device_property_set_string(device, "storage.model", params->model);
  hal_device_property_set_string(device, "storage.vendor", vendor);
  if (*params->serial)
    hal_device_property_set_string(device, "storage.serial", params->serial);
  if (*params->revision)
    hf_device_property_set_string_printf(device, "storage.firmware_revision", "%.8s", params->revision);

  g_free(vendor);

  hf_block_device_complete(device, device, FALSE);

  return device;
}

static void
hf_ata_probe_devices (HalDevice *ide_host)
{
#ifdef IOCATADEVICES
  struct ata_ioc_devices devices;
#else
  struct ata_cmd devices;
#endif
  int i;

  g_return_if_fail(HAL_IS_DEVICE(ide_host));

#ifdef IOCATADEVICES
  devices.channel = hal_device_property_get_int(ide_host, "ide_host.number");
  if (ioctl(hf_ata_fd, IOCATADEVICES, &devices) < 0)
    {
      HAL_WARNING(("unable to probe devices of ATA channel %i: %s", devices.channel, g_strerror(errno)));
      return;
    }
#else
  memset(&devices, 0, sizeof(devices));
  devices.cmd = ATAGPARM;
  devices.device = -1;
  devices.channel = hal_device_property_get_int(ide_host, "ide_host.number");
  if (ioctl(hf_ata_fd, IOCATA, &devices) < 0)
    {
      HAL_WARNING(("unable to probe devices of ATA channel %i: %s", devices.channel, g_strerror(errno)));
      return;
    }
#endif

  for (i = 0; i < 2; i++)
#ifdef IOCATADEVICES
    if (*devices.name[i])
#else
    if (*devices.u.param.name[i])
#endif
      {
	HalDevice *ide_device;

	ide_device = hf_device_store_match(hald_get_gdl(),
                                           "ide.host", HAL_PROPERTY_TYPE_INT32, devices.channel,
					   "ide.channel", HAL_PROPERTY_TYPE_INT32, i,
					   NULL);

	if (! ide_device)
	  {
	    ide_device = hf_ata_ide_device_new(ide_host, i);
	    hf_device_preprobe_and_add(ide_device);
	  }

	if (! hal_device_property_get_bool(ide_device, "info.ignore")
#ifdef IOCATADEVICES
	    && ! hf_devtree_find_from_name(hald_get_gdl(), devices.name[i]))
#else
	    && ! hf_devtree_find_from_name(hald_get_gdl(), devices.u.param.name[i]))
#endif
	  {
	    HalDevice *block_device;

	    block_device = hf_ata_block_device_new(ide_device, i, &devices);

	    /*
	     * hf-scsi might need to ignore the device, so we cannot
	     * add it immediately (we cannot ignore a device after it
	     * was added). We will therefore add the device to a
	     * pending list, and let hf-scsi call
	     * hf_ata_add_pending_devices() to add the pending
	     * devices.
	     */
	    hf_ata_pending_devices = g_list_append(hf_ata_pending_devices, block_device);
	  }
      }
}

void
hf_ata_add_pending_devices (void)
{
  GList *l;

  HF_LIST_FOREACH(l, hf_ata_pending_devices)
    hf_storage_device_add(l->data);

  g_list_free(hf_ata_pending_devices);
  hf_ata_pending_devices = NULL;
}

static void
hf_ata_privileged_init (void)
{
  hf_ata_fd = open(HF_ATA_DEVICE, O_RDONLY);
  if (hf_ata_fd < 0)
    HAL_INFO(("unable to open %s: %s", HF_ATA_DEVICE, g_strerror(errno)));
}

static void
hf_ata_probe (void)
{
  GSList *gdl_devices;
  GSList *l;

  /*
   * There must be no pending device, otherwise hf-scsi did not call
   * hf_ata_add_pending_devices() during the previous probe and there
   * is a bug somewhere.
   */
  g_assert(hf_ata_pending_devices == NULL);

  if (hf_ata_fd < 0)
    return;

  /* we might modify the gdl while iterating, so we must use a copy */
  gdl_devices = g_slist_copy(hald_get_gdl()->devices);
  HF_LIST_FOREACH(l, gdl_devices)
    {
      HalDevice *device = l->data;

      if (hal_device_has_property(device, "ide_host.number") && ! hal_device_property_get_bool(device, "info.ignore"))
	hf_ata_probe_devices(device);
    }
  g_slist_free(gdl_devices);
}

void
hf_ata_channel_set_properties (HalDevice *device)
{
  int unit;

  unit = hal_device_property_get_int(device, "freebsd.unit");

  hf_device_set_udi(device, "ide_host_%i", unit);

  hal_device_property_set_string(device, "info.subsystem", "ide_host");
  hal_device_property_set_int(device, "ide_host.number", unit);
}

HFHandler hf_ata_handler = {
  .privileged_init	= hf_ata_privileged_init,
  .probe		= hf_ata_probe
};
