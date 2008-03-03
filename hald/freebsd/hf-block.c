/***************************************************************************
 * CVSID: $Id$
 *
 * hf-block.c : block device support
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

#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "../logger.h"

#include "hf-block.h"
#include "hf-util.h"

static gboolean
hf_block_get_major_minor (const char *filename, int *major, int *minor)
{
  struct stat sb;

  g_return_val_if_fail(filename != NULL, FALSE);
  g_return_val_if_fail(major != NULL, FALSE);
  g_return_val_if_fail(minor != NULL, FALSE);

  if (stat(filename, &sb) < 0)
    {
      HAL_WARNING(("unable to stat %s: %s", filename, g_strerror(errno)));
      return FALSE;
    }

  *major = major(sb.st_rdev);
  *minor = minor(sb.st_rdev);

  return TRUE;
}

/* adapted from blockdev_compute_udi() in linux2/blockdev.c */
static void
hf_block_device_compute_udi (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  if (hal_device_property_get_bool(device, "block.is_volume"))
    {
      const char *label;
      const char *uuid;

      label = hal_device_property_get_string(device, "volume.label");
      uuid = hal_device_property_get_string(device, "volume.uuid");

      if (uuid && *uuid)
	hf_device_set_udi(device, "volume_uuid_%s", uuid);
      else if (label && *label)
	hf_device_set_udi(device, "volume_label_%s", label);
      else if (hal_device_property_get_bool(device, "volume.is_disc") &&
	       hal_device_property_get_bool(device, "volume.disc.is_blank"))
	/* this should be an empty CD/DVD */
	hf_device_set_udi(device, "volume_empty_%s", hal_device_property_get_string(device, "volume.disc.type"));
      else if (hal_device_has_property(device, "volume.partition.number")
	       && hal_device_has_property(device, "volume.size"))
	hf_device_set_udi(device, "volume_part%i_size_%ju",
			  hal_device_property_get_int(device, "volume.partition.number"),
			  hal_device_property_get_uint64(device, "volume.size"));
      else if (hal_device_has_property(device, "volume.partition.number"))
	hf_device_set_udi(device, "volume_part%i",
			  hal_device_property_get_int(device, "volume.partition.number"));
      else if (hal_device_has_property(device, "volume.size"))
	hf_device_set_udi(device, "volume_size_%ju",
			  hal_device_property_get_uint64(device, "volume.size"));
      else
	hf_device_set_full_udi(device, "%s_volume", hal_device_property_get_string(device, "info.parent"));
    }
  else if (hal_device_has_capability(device, "storage"))
    {
      const char *model;
      const char *serial;
      const char *physical_device;

      model = hal_device_property_get_string(device, "storage.model");
      serial = hal_device_property_get_string(device, "storage.serial");
      physical_device = hal_device_property_get_string(device, "storage.originating_device");

      if (serial && *serial)
	hf_device_set_udi(device, "storage_serial_%s", serial);
      else if (model && *model)
	hf_device_set_udi(device, "storage_model_%s", model);
      else if (physical_device && *physical_device)
	hf_device_set_full_udi(device, "%s_storage", physical_device);
      else
	hf_device_set_full_udi(device, "%s_storage", hal_device_property_get_string(device, "info.parent"));
    }
  else
    hf_device_set_full_udi(device, "%s_block", hal_device_property_get_string(device, "info.parent"));
}

static void
hf_block_device_compute_product (HalDevice *device)
{
g_return_if_fail(HAL_IS_DEVICE(device));

  if (hal_device_has_property(device, "info.product"))
    return;

  if (hal_device_property_get_bool(device, "block.is_volume"))
    {
      const char *str;

      if ((str = hal_device_property_get_string(device, "volume.label")) && *str)
	hal_device_property_set_string(device, "info.product", str);
      else if ((str = hal_device_property_get_string(device, "volume.fstype")) && *str)
	hf_device_property_set_string_printf(device, "info.product", "Volume (%s)", str);
      else if ((str = hal_device_property_get_string(device, "volume.fsusage")) && ! strcmp(str, "unused"))
	hal_device_property_set_string(device, "info.product", "Volume (Unused)");
      else
	hal_device_property_set_string(device, "info.product", "Volume");
    }
  else if (hal_device_has_capability(device, "storage"))
    hal_device_property_set_string(device, "info.product", "Storage Device");
  else
    hal_device_property_set_string(device, "info.product", "Block Device");
}

void
hf_block_device_enable (HalDevice *device, const char *devname)
{
  int major;
  int minor;

  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(devname != NULL);

  hal_device_add_capability(device, "block");

  hal_device_property_set_string(device, "info.subsystem", "block");
  hal_device_property_set_string(device, "info.category", "block"); /* FIXME? */

  hf_device_property_set_string_printf(device, "block.device", "/dev/%s", devname);
  if (hf_block_get_major_minor(hal_device_property_get_string(device, "block.device"), &major, &minor))
    {
      hal_device_property_set_int(device, "block.major", major);
      hal_device_property_set_int(device, "block.minor", minor);
    }
}

void
hf_block_device_complete (HalDevice *device,
			  HalDevice *storage_device,
			  gboolean is_volume)
{
  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(HAL_IS_DEVICE(storage_device));

  hal_device_property_set_bool(device, "block.is_volume", is_volume);

  hf_block_device_compute_udi(device);
  hf_block_device_compute_product(device);

  /* set this last, in case device == storage_device */
  hal_device_copy_property(storage_device, "info.udi", device, "block.storage_device");
}
