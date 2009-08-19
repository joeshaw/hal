/***************************************************************************
 * CVSID: $Id$
 *
 * hf-scsi.c : SCSI support
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
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>

#include "../logger.h"

#include "hf-scsi.h"
#include "hf-ata.h"
#include "hf-block.h"
#include "hf-devtree.h"
#include "hf-storage.h"
#include "hf-util.h"

#define HF_SCSI_DEVICE			"/dev/xpt0"

#define N_RESULTS			100

static int hf_scsi_fd;

static gboolean
hf_scsi_is_cdrom (int type)
{
  return (type == T_CDROM || type == T_WORM || type == T_OPTICAL);
}

static HalDevice *
hf_scsi_bus_device_new (HalDevice *parent,
			const struct bus_match_result *match)
{
  HalDevice *device;

  g_return_val_if_fail(match != NULL, NULL);

  device = hf_device_new(parent);

  hal_device_property_set_string(device, "info.subsystem", "scsi_host");
  hal_device_property_set_int(device, "scsi_host.host", match->path_id);
  hal_device_property_set_string(device, "info.product", "SCSI Host Adapter");

  /* scsi_host_compute_udi() in linux2/classdev.c */
  hf_device_set_full_udi(device, "%s_scsi_host", hal_device_property_get_string(device, "info.parent"));

  return device;
}

static HalDevice *
hf_scsi_scsi_device_new (HalDevice *parent,
			 const struct device_match_result *match)
{
  HalDevice *device;
  /* buffer sizes from camcontrol.c */
  char vendor[16];
  char product[48];
  int type;

  g_return_val_if_fail(match != NULL, NULL);

  device = hf_device_new(parent);

  hal_device_property_set_string(device, "info.subsystem", "scsi");
  hal_device_property_set_int(device, "scsi.host", match->path_id);
  hal_device_property_set_int(device, "scsi.bus", match->path_id);
  hal_device_property_set_int(device, "scsi.target", match->target_id);
  hal_device_property_set_int(device, "scsi.lun", match->target_lun);
  hal_device_property_set_string(device, "info.product", "SCSI Device");

  cam_strvis(vendor, match->inq_data.vendor, sizeof(match->inq_data.vendor), sizeof(vendor));
  cam_strvis(product, match->inq_data.product, sizeof(match->inq_data.product), sizeof(product));

  if (*vendor)
    {
      hal_device_property_set_string(device, "info.vendor", vendor);
      hal_device_property_set_string(device, "scsi.vendor", vendor);
    }
  if (*product)
    hal_device_property_set_string(device, "scsi.model", product);

  /* types from cam/scsi/scsi_all.h */
  type = SID_TYPE(&match->inq_data);
  switch (type)
    {
    case T_DIRECT:
    case T_RBC:
      /* From linux2/physdev.c, T_RBC is a Reduced Block Command device
       * which is sometimes used by firewire devices */
      hal_device_property_set_string(device, "scsi.type", "disk");
      break;
    case T_SEQUENTIAL:
      hal_device_property_set_string(device, "scsi.type", "tape");
      break;
    case T_PRINTER:
      hal_device_property_set_string(device, "scsi.type", "printer");
      break;
    case T_PROCESSOR:
      hal_device_property_set_string(device, "scsi.type", "processor");
      break;
    case T_WORM:
    case T_CDROM:
    case T_OPTICAL:
      hal_device_property_set_string(device, "scsi.type", "cdrom");
      break;
    case T_CHANGER:
      hal_device_property_set_string(device, "scsi.type", "medium_changer");
      break;
    case T_SCANNER:
      hal_device_property_set_string(device, "scsi.type", "scanner");
      break;
    case T_STORARRAY:
      hal_device_property_set_string(device, "scsi.type", "array");
      break;
    default:
      hal_device_property_set_string(device, "scsi.type", "unknown");
    }

  /* scsi_compute_udi() in linux2/physdev.c */
  hf_device_set_full_udi(device, "%s_scsi_device_lun%i",
			 hal_device_property_get_string(device, "info.parent"),
			 hal_device_property_get_int(device, "scsi.lun"));

  return device;
}

static HalDevice *
hf_scsi_block_device_new (HalDevice *parent,
			  const struct device_match_result *match,
			  const char *devname)
{
  HalDevice *device;
  int type;
  /* buffer size from camcontrol.c */
  char revision[16];

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);
  g_return_val_if_fail(match != NULL, NULL);
  g_return_val_if_fail(devname != NULL, NULL);

  device = hf_device_new(parent);

  hf_devtree_device_set_name(device, devname);
  hf_block_device_enable(device, devname);

  hf_storage_device_enable(device);

  type = SID_TYPE(&match->inq_data);

  if (type == T_SEQUENTIAL)
    hf_storage_device_enable_tape(device);
  else if (hf_scsi_is_cdrom(type))
    hf_storage_device_enable_cdrom(device);

  if (SID_IS_REMOVABLE(&match->inq_data))
    {
      hal_device_property_set_bool(device, "storage.removable", TRUE);
      hal_device_property_set_bool(device, "storage.media_check_enabled", TRUE);
      hal_device_property_set_bool(device, "storage.removable.support_async_notification", FALSE);
    }

  cam_strvis(revision, match->inq_data.revision, sizeof(match->inq_data.revision), sizeof(revision));

  if (hal_device_has_property(parent, "scsi.vendor"))
    {
      hal_device_copy_property(parent, "scsi.vendor", device, "info.vendor");
      hal_device_copy_property(parent, "scsi.vendor", device, "storage.vendor");
    }
  if (hal_device_has_property(parent, "scsi.model"))
    {
      hal_device_copy_property(parent, "scsi.model", device, "info.product");
      hal_device_copy_property(parent, "scsi.model", device, "storage.model");
    }
  if (*revision)
    hal_device_property_set_string(device, "storage.firmware_revision", revision);

  /*
   * Walk up the device chain to find the physical device (adapted
   * from hotplug_event_begin_add_blockdev() in linux2/blockdev.c).
   */
  while (parent)
    {
      const char *bus;
      const char *parent_udi;

      bus = hal_device_property_get_string(parent, "info.subsystem");
      if (bus)
	{
	  if (! strcmp(bus, "scsi"))
	    {
	      hal_device_property_set_string(device, "storage.bus", "scsi");
	      hal_device_property_set_string(device, "storage.originating_device", hal_device_get_udi(parent));
	      hal_device_copy_property(parent, "scsi.lun", device, "storage.lun");
	      /* do not stop here, in case it's an umass device */
	    }
	  else if (! strcmp(bus, "usb"))
	    {
	      hal_device_property_set_string(device, "storage.bus", "usb");
	      hal_device_property_set_string(device, "storage.originating_device", hal_device_get_udi(parent));
	      hal_device_property_set_bool(device, "storage.hotpluggable", TRUE);
	      break;		/* done */
	    }
	}

      parent_udi = hal_device_property_get_string(parent, "info.parent");
      if (parent_udi)
	{
	  parent = hal_device_store_find(hald_get_gdl(), parent_udi);
	  g_assert(parent != NULL);
	}
      else
	parent = NULL;
    }

  return device;
}

static void
hf_scsi_privileged_init (void)
{
  hf_scsi_fd = open(HF_SCSI_DEVICE, O_RDWR);
  if (hf_scsi_fd < 0)
    HAL_INFO(("unable to open %s: %s", HF_SCSI_DEVICE, g_strerror(errno)));
}

static HalDevice *
hf_scsi_get_ata_channel (HalDevice *scsi_bus)
{
  HalDevice *parent;

  g_return_val_if_fail(HAL_IS_DEVICE(scsi_bus), NULL);

  parent = hf_device_store_get_parent(hald_get_gdl(), scsi_bus);
  if (parent)
    {
      const char *driver;

      driver = hal_device_property_get_string(parent, "freebsd.driver");
      if (! driver || strcmp(driver, "ata"))
	parent = NULL;		/* parent is not an ATA channel */
    }

  return parent;
}

static HalDevice *
hf_scsi_get_atapi_device (HalDevice *ata_channel, int target_id)
{
  HalDevice *device = NULL;
  int ide_host;
  GList *l;

  g_return_val_if_fail(HAL_IS_DEVICE(ata_channel), NULL);
  g_return_val_if_fail(target_id == 0 || target_id == 1, NULL); /* ATA master or slave */

  ide_host = hal_device_property_get_int(ata_channel, "ide_host.number");

  /*
   * If there's an ATAPI device it will be in hf_ata_pending_devices,
   * since we haven't called hf_ata_add_pending_devices() yet.
   */

  HF_LIST_FOREACH(l, hf_ata_pending_devices)
    {
      HalDevice *child = HAL_DEVICE(l->data);
      HalDevice *parent;
      const char *driver;
      const char *phys_device;
      int host;
      int channel;

      driver = hal_device_property_get_string(child, "freebsd.driver");
      /* ATAPI devices: CD-ROM (acd), tape (ast) or floppy (afd) */
      if (! driver || (strcmp(driver, "acd") && strcmp(driver, "ast") && strcmp(driver, "afd")))
        continue;

      phys_device = hal_device_property_get_string(child, "storage.originating_device");
      if (! phys_device)
        continue;

      parent = hal_device_store_find(hald_get_gdl(), phys_device);
      if (! parent)
        continue;

      host = hal_device_property_get_int(parent, "ide.host");
      if (host != ide_host)
        continue;

      channel = hal_device_property_get_int(parent, "ide.channel");
      if (target_id == channel)
        {
	  device = child;
	  break;
	}
    }

  return device;
}

static void
hf_scsi_handle_pending_device (struct device_match_result **match,
			       char **devname)
{
  g_return_if_fail(match != NULL);
  g_return_if_fail(devname != NULL);

  if (*match && *devname)
    {
      HalDevice *parent;

      parent = hal_device_store_match_key_value_int(hald_get_gdl(), "scsi_host.host", (*match)->path_id);
      if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
	{
	  HalDevice *atapi_device = NULL;
	  HalDevice *scsi_device;
	  int type;

	  /*
	   * If the device's parent (the SCSI bus) is an atapicam(4)
	   * bus, we shall ignore the corresponding ATAPI device
	   * unless this device was ignored. This is to ensure that
	   * the same physical device will only be handled once
	   * (through ATA or SCSI).
	   */
	  if (parent)
	    {
	      HalDevice *ata_channel;

	      ata_channel = hf_scsi_get_ata_channel(parent);
	      if (ata_channel)
		{
		  atapi_device = hf_scsi_get_atapi_device(ata_channel, (*match)->target_id);
		  if (atapi_device)
                    {
		      char *cam_devname;
		      char *scsi_path;

		      cam_devname = g_strdup_printf("/dev/%s", *devname);
		      scsi_path = g_strdup_printf("%d,%d,%d", (*match)->path_id, (*match)->target_id, (*match)->target_lun);
		      hal_device_property_set_string(atapi_device, "block.freebsd.atapi_cam_device", cam_devname);
		      hal_device_property_set_string(atapi_device, "block.freebsd.cam_path", scsi_path);
		      g_free(cam_devname);
		      g_free(scsi_path);
		    }
		}
	    }

	  type = SID_TYPE(&(*match)->inq_data);
	  scsi_device = hf_scsi_scsi_device_new(parent, *match);
	  if (hf_device_preprobe_and_add(scsi_device) && (type == T_DIRECT ||
              type == T_SEQUENTIAL || type == T_RBC || type == T_STORARRAY ||
	      hf_scsi_is_cdrom(type)))
	    {
	      HalDevice *block_device;
	      char *scsi_path;

	      block_device = hf_scsi_block_device_new(scsi_device, *match, *devname);

	      scsi_path = g_strdup_printf("%d,%d,%d", (*match)->path_id, (*match)->target_id, (*match)->target_lun);
	      hal_device_property_set_string(block_device, "block.freebsd.cam_path", scsi_path);
	      g_free(scsi_path);

	      if (hf_device_preprobe(block_device))
		{
		  /*
		   * The device was not ignored. If there is a
		   * corresponding ATAPI device, ignore it (see above
		   * comment).
		   */
		  if (atapi_device)
		    hal_device_property_set_bool(atapi_device, "info.ignore", TRUE);

		  hf_runner_run_sync(block_device, 0, "hald-probe-scsi", NULL);

		  /*
		   * hald-probe-scsi might have set storage.serial,
		   * which is used in the UDI computed in
		   * hf_block_device_complete().
		   */
		  hf_block_device_complete(block_device, block_device, FALSE);

		  hf_device_add(block_device);
		  hf_storage_device_probe(block_device, FALSE);
		}
	    }
	}
    }

  g_free(*match);
  *match = NULL;

  g_free(*devname);
  *devname = NULL;
}

/* inspired by getdevtree() in sbin/camcontrol/camcontrol.c */
static void
hf_scsi_probe (void)
{
  union ccb ccb;
  struct device_match_result *pending_device = NULL;
  char *pending_devname = NULL;

  if (hf_scsi_fd < 0)
    goto end;			/* already warned in hf_scsi_init() */

  memset(&ccb, 0, sizeof(ccb));

  ccb.ccb_h.path_id = CAM_XPT_PATH_ID;
  ccb.ccb_h.target_id = CAM_TARGET_WILDCARD;
  ccb.ccb_h.target_lun = CAM_LUN_WILDCARD;

  ccb.ccb_h.func_code = XPT_DEV_MATCH;
  ccb.cdm.match_buf_len = sizeof(struct dev_match_result) * N_RESULTS;
  ccb.cdm.matches = g_new(struct dev_match_result, N_RESULTS);
  ccb.cdm.num_matches = 0;
  ccb.cdm.num_patterns = 0;
  ccb.cdm.pattern_buf_len = 0;

  do
    {
      int i;

      if (ioctl(hf_scsi_fd, CAMIOCOMMAND, &ccb) < 0)
	{
	  HAL_WARNING(("unable to get list of SCSI devices: %s", g_strerror(errno)));
	  break;
	}

      if (ccb.ccb_h.status != CAM_REQ_CMP
	  || (ccb.cdm.status != CAM_DEV_MATCH_LAST
	      && ccb.cdm.status != CAM_DEV_MATCH_MORE))
	{
	  HAL_WARNING(("got CAM error %#x, CDM error %d", ccb.ccb_h.status, ccb.cdm.status));
	  break;
	}

      for (i = 0; i < (int) ccb.cdm.num_matches; i++)
	switch (ccb.cdm.matches[i].type)
	  {
	  case DEV_MATCH_BUS:
	    {
	      struct bus_match_result *match;
	      HalDevice *device;
	      HalDevice *parent = NULL;

	      match = &ccb.cdm.matches[i].result.bus_result;
	      if ((int) match->path_id == -1)
		break;

	      hf_scsi_handle_pending_device(&pending_device, &pending_devname);

	      device = hal_device_store_match_key_value_int(hald_get_gdl(), "scsi_host.host", match->path_id);
	      if (device)
		break;		/* device already exists */

	      if (! strcmp(match->dev_name, "umass-sim"))
		{
		  parent = hf_devtree_find_from_info(hald_get_gdl(), "umass", match->unit_number);

		  /* make it a child of the mass storage interface */
		  if (parent)
		    {
		      HalDevice *parent_if;

		      parent_if = hf_device_store_match(hald_get_gdl(),
                                                        "info.parent", HAL_PROPERTY_TYPE_STRING, hal_device_get_udi(parent),
							"usb.interface.class", HAL_PROPERTY_TYPE_INT32, 0x08,
							NULL);

		      if (parent_if)
			parent = parent_if;
		    }
		}
	      else if (! strcmp(match->dev_name, "ata")) /* ATAPI/CAM */
		parent = hf_devtree_find_from_info(hald_get_gdl(), "ata", match->unit_number);

	      if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
		{
		  device = hf_scsi_bus_device_new(parent, match);
		  hf_device_preprobe_and_add(device);
		}
	    }
	    break;

	  case DEV_MATCH_DEVICE:
	    {
	      struct device_match_result *match;
	      HalDevice *device;

	      match = &ccb.cdm.matches[i].result.device_result;
	      if ((int) match->path_id == -1)
		break;

	      hf_scsi_handle_pending_device(&pending_device, &pending_devname);

	      device = hf_device_store_match(hald_get_gdl(),
                                             "scsi.bus", HAL_PROPERTY_TYPE_INT32, match->path_id,
					     "scsi.target", HAL_PROPERTY_TYPE_INT32, match->target_id,
					     "scsi.lun", HAL_PROPERTY_TYPE_INT32, match->target_lun,
					     NULL);

	      if (device)
		break;		/* device already exists */

	      pending_device = g_new(struct device_match_result, 1);
	      *pending_device = *match;
	    }
	    break;

	  case DEV_MATCH_PERIPH:
	    {
	      struct periph_match_result *match;

	      if (! pending_device)
		break;

	      if (pending_devname)
		break;		/* only use the first peripheral */

	      match = &ccb.cdm.matches[i].result.periph_result;
	      if ((int) match->path_id == -1 || ! strcmp(match->periph_name, "pass") || ! strcmp(match->periph_name, "probe"))
		break;

	      pending_devname = g_strdup_printf("%s%i", match->periph_name, match->unit_number);
	    }
	    break;
	  }
    }
  while (ccb.ccb_h.status == CAM_REQ_CMP && ccb.cdm.status == CAM_DEV_MATCH_MORE);

  hf_scsi_handle_pending_device(&pending_device, &pending_devname);

  g_free(ccb.cdm.matches);

 end:
  /*
   * Now that we have probed the SCSI devices and ignored the ATAPI
   * devices as necessary, it is time to add the pending ATA block
   * devices.
   */
  hf_ata_add_pending_devices();
}

HFHandler hf_scsi_handler = {
  .privileged_init	= hf_scsi_privileged_init,
  .probe		= hf_scsi_probe
};
