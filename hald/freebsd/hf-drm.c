/***************************************************************************
 * CVSID: $Id$
 *
 * hf-drm.c : DRM (Direct Rendering) device support
 *
 * Copyright (C) 2008 Joe Marcus Clarke <marcus@FreeBSD.org>
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
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../logger.h"
#include "../osspec.h"

#include "hf-drm.h"
#include "hf-devtree.h"
#include "hf-util.h"

#define HF_DRM_DEVICE		"/dev/dri/card"

typedef struct
{
  int major;
  int minor;
  int patchlevel;
  unsigned long name_len;
  char *name;
  unsigned long date_len;
  char *date;
  unsigned long desc_len;
  char *desc;
} hf_drm_version_t;

#define HF_DRM_VERSION_IOCTL	_IOWR('d', 0x00, hf_drm_version_t)

typedef struct
{
  int			index;
  char			*name;
  char			*version;
  char			*device_file;
  char			*vendor;
} Card;

static GSList *cards = NULL;

static Card *
hf_drm_find_card (int index)
{
  GSList *l;

  HF_LIST_FOREACH(l, cards)
  {
    Card *card = l->data;

    if (card->index == index)
      return card;
  }

  return NULL;
}

static void
hf_drm_version_free (hf_drm_version_t *vers)
{
  g_return_if_fail(vers != NULL);

  g_free(vers->name);
  g_free(vers->date);
  g_free(vers->desc);

  g_free(vers);
}

static void
hf_drm_set_properties_real (HalDevice *device, Card *card)
{
  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(card != NULL);

  hal_device_add_capability(device, "drm");

  if (card->vendor)
    hal_device_property_set_string(device, "info.vendor", card->vendor);
  hal_device_property_set_string(device, "info.product", "Direct Rendering Manager Device");
  hal_device_property_set_string(device, "info.subsystem", "drm");
  hal_device_property_set_string(device, "info.category", "drm");
  hf_device_property_set_string_printf(device, "freebsd.device_file", "%s%i", card->device_file, card->index);
  hal_device_property_set_string(device, "drm.dri_library", card->name);
  hal_device_property_set_string(device, "drm.version", card->version);

  hf_device_set_full_udi(device, "%s_drm_%s_card%i", hal_device_property_get_string(device, "info.parent"), card->name, card->index);
}

static HalDevice *
hf_drm_device_new (HalDevice *parent, Card *card)
{
  HalDevice *device;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  device = hf_device_new(parent);

  hf_drm_set_properties_real(device, card);

  return device;
}

static void
hf_drm_privileged_init (void)
{
  int i;

  for (i = 0; i < 16; i++)
    {
      char *filename;
      int fd;

      filename = g_strdup_printf(HF_DRM_DEVICE "%i", i);
      fd = open(filename, O_RDONLY);
      g_free(filename);

      if (fd >= 0)
        {
          Card *card;
          hf_drm_version_t *drm_version;
          int res;

          card = g_new0(Card, 1);
          card->index = i;
          drm_version = g_new0(hf_drm_version_t, 1);

          res = ioctl(fd, HF_DRM_VERSION_IOCTL, drm_version);
          if (res)
            {
              hf_drm_version_free(drm_version);
              g_free(card);
              close(fd);
              continue;
            }

          if (drm_version->name_len)
            drm_version->name = g_malloc(drm_version->name_len + 1);
          if (drm_version->date_len)
            drm_version->date = g_malloc(drm_version->date_len + 1);
          if (drm_version->desc_len)
            drm_version->desc = g_malloc(drm_version->desc_len + 1);

          res = ioctl(fd, HF_DRM_VERSION_IOCTL, drm_version);
          close(fd);

          if (res)
            {
              hf_drm_version_free(drm_version);
              g_free(card);
              continue;
            }

          if (drm_version->name)
            drm_version->name[drm_version->name_len] = '\0';
          if (drm_version->date)
            drm_version->date[drm_version->date_len] = '\0';
          if (drm_version->desc)
            drm_version->desc[drm_version->desc_len] = '\0';

          if (! drm_version->name || ! drm_version->date)
            {
              hf_drm_version_free(drm_version);
              g_free(card);
              continue;
            }

          card->version = g_strdup_printf("drm %i.%i.%i %s", drm_version->major, drm_version->minor, drm_version->patchlevel, drm_version->date);
          card->device_file = g_strdup(HF_DRM_DEVICE);
          card->name = g_strdup(drm_version->name);
          if (drm_version->desc)
            card->vendor = g_strdup(drm_version->desc);
          hf_drm_version_free(drm_version);

          cards = g_slist_append(cards, card);
        }
    }
}

void
hf_drm_set_properties (HalDevice *device)
{
  Card *card;
  int unit;

  if (! cards)
    return;

  unit = hal_device_property_get_int(device, "freebsd.unit");
  card = hf_drm_find_card(unit);

  if (! card)
    return;

  hf_drm_set_properties_real(device, card);
}

static void
hf_drm_probe (void)
{
  GSList *nvidia_devices, *l;

  nvidia_devices = hal_device_store_match_multiple_key_value_string(hald_get_gdl(),
                   "freebsd.driver", "nvidia");
  HF_LIST_FOREACH(l, nvidia_devices)
  {
    HalDevice *parent = HAL_DEVICE(l->data);

    if (! hal_device_property_get_bool(parent, "info.ignore"))
      {
        Card *card;
        HalDevice *device;
        int unit;

        unit = hal_device_property_get_int(parent, "freebsd.unit");
        card = g_new0(Card, 1);

        card->index = unit;
        card->name = g_strdup("nvidia");
        card->vendor = g_strdup("nVidia Corporation");
        card->device_file = g_strdup("/dev/nvidia");
        card->version = hf_get_string_sysctl(NULL, "hw.nvidia.version");

        device = hf_drm_device_new(parent, card);

        if (device)
          hf_device_preprobe_and_add(device);

        g_free(card->name);
        g_free(card->version);
        g_free(card->device_file);
        g_free(card->vendor);
        g_free(card);
      }
  }
  g_slist_free(nvidia_devices);
}

HFHandler hf_drm_handler =
{
  .privileged_init	= hf_drm_privileged_init,
  .probe		= hf_drm_probe
};
