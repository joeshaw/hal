/***************************************************************************
 * CVSID: $Id$
 *
 * hf-serial.c : serial device support
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

#include "hf-serial.h"
#include "hf-util.h"

static HalDevice *
hf_serial_device_new (HalDevice *parent)
{
  HalDevice *device;
  const char *product;
  int unit;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  device = hf_device_new(parent);

  product = hal_device_property_get_string(parent, "info.product");
  if (product)
    hal_device_property_set_string(device, "info.product", product);

  hal_device_property_set_string(device, "info.category", "serial");
  hal_device_add_capability(device, "serial");

  hal_device_property_set_string(device, "serial.originating_device", hal_device_get_udi(parent));

  /* callin devices: /dev/ttyd[0-9a-v] -- see sio(4) */
  unit = hal_device_property_get_int(parent, "freebsd.unit");
  if (unit < 10)		/* 0-9 */
    hf_device_property_set_string_printf(device, "serial.device", "/dev/ttyd%i", unit);
  else if (unit < 32)		/* a-v */
    hf_device_property_set_string_printf(device, "serial.device", "/dev/ttyd%c", unit - 10 + 'a');
  else
    hal_device_property_set_string(device, "serial.device", NULL);

  hal_device_property_set_int(device, "serial.port", unit);
  hal_device_property_set_string(device, "serial.type", "platform");

  /* UDI from serial_compute_udi() in linux2/classdev.c */
  hf_device_set_full_udi(device, "%s_serial_%s_%i", hal_device_get_udi(parent), "platform", unit);

  return device;
}

static void
hf_serial_probe (void)
{
  GSList *sio_devices;
  GSList *l;

  sio_devices = hal_device_store_match_multiple_key_value_string(hald_get_gdl(), "freebsd.driver", "sio");
  HF_LIST_FOREACH(l, sio_devices)
    {
      HalDevice *parent = l->data;

      if (! hal_device_store_match_key_value_int(hald_get_gdl(), "serial.port", hal_device_property_get_int(parent, "freebsd.unit"))
	  && ! hal_device_property_get_bool(parent, "info.ignore"))
	{
	  HalDevice *device;

	  device = hf_serial_device_new(l->data);
	  hf_device_preprobe_and_add(device);
	}
    }
  g_slist_free(sio_devices);
}

HFHandler hf_serial_handler = {
  .probe = hf_serial_probe
};
