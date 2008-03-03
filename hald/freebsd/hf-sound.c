/***************************************************************************
 * CVSID: $Id$
 *
 * hf-sound.c : sound (OSS) device support
 *
 * Copyright (C) 2006 Joe Marcus Clarke <marcus@FreeBSD.org>
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

#include "../logger.h"
#include "../util.h"

#include "hf-sound.h"
#include "hf-devtree.h"
#include "hf-util.h"

#define HF_SNDSTAT_DEV		"/dev/sndstat"

static GHashTable *drv_hash = NULL;

static const struct oss_sound_device {
  char *type;
  char *driver;
} oss_sound_devices[] = {
  { "pcm",	"dsp" },
  { "mixer",	"mixer" }
  /* XXX midi support has been removed which means no sequencer either
  { "midi",	"midi" }
  */
};

static HalDevice *
hf_sound_oss_device_new (HalDevice *parent,
                         const char *type,
                         const char *driver)
{
  HalDevice *device;
  char *product, *card_id, *devname, *dev_node;
  const char *pproduct;
  int unit;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  unit = hal_device_property_get_int(parent, "freebsd.unit");
  dev_node = g_strdup_printf("/dev/%s%i", driver, unit);

  if (! g_file_test(dev_node, G_FILE_TEST_EXISTS))
    {
      g_free(dev_node);
      return NULL;
    }

  device = hf_device_new(parent);

  hal_device_property_set_string(device, "oss.originating_device", hal_device_get_udi(parent));

  pproduct = hal_device_property_get_string(parent, "info.product");

  product = g_strdup_printf("%s (%s)", (pproduct != NULL) ? pproduct : "Sound Card",
                            type);
  hal_device_property_set_string(device, "info.product", product);
  hal_device_property_set_string(device, "oss.device_id", product);
  g_free(product);

  hal_device_property_set_string(device, "info.category", "oss");
  hal_device_add_capability(device, "oss");

  hal_device_property_set_string(device, "oss.type", type);

  hal_device_property_set_int(device, "oss.card", unit);
  hal_device_property_set_int(device, "oss.device", unit);

  devname = hf_devtree_device_get_name(parent);
  if (devname)
    {
      card_id = g_hash_table_lookup(drv_hash, (gconstpointer)devname);
      if (card_id)
        hal_device_property_set_string(device, "oss.card_id", card_id);
      else
	hal_device_copy_property(device, "oss.card_id", device, "oss.device_id");
    }
  else
    hal_device_copy_property(device, "oss.card_id", device, "oss.device_id");
  g_free(devname);

  hal_device_property_set_string(device, "oss.device_file", dev_node);
  g_free(dev_node);

  hf_device_set_full_udi(device, "%s_oss_%s_%i", hal_device_get_udi(parent), type, unit);

  return device;
}

static void
hf_sound_probe (void)
{
  GIOChannel *channel;
  GError *err = NULL;
  GSList *pcm_devices, *l;
  char *buf;
  char **toks;
  gsize len;
  int i;

  drv_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  channel = g_io_channel_new_file(HF_SNDSTAT_DEV, "r", &err);
  if (channel == NULL)
    {
      HAL_WARNING(("unable to open %s: %s", HF_SNDSTAT_DEV, err->message));
      goto nosndstat;
    }

  if (g_io_channel_read_to_end(channel, &buf, &len, &err) !=
      G_IO_STATUS_NORMAL)
    {
      HAL_WARNING(("failed to read %s: %s", HF_SNDSTAT_DEV, err->message));
      g_io_channel_unref(channel);
      goto nosndstat;
    }
  g_io_channel_unref(channel);

  toks = g_strsplit(buf, "\n", 0);
  g_free(buf);

  if (g_strv_length(toks) < 3)
    {
      HAL_INFO(("no soundcards found"));
      g_strfreev(toks);
      goto nosndstat;
    }

  for (i = 2; toks[i] != NULL; i++)
    {
      char *drv, *descr, *ptr;

      /* format:
pcm0: <Intel ICH5 (82801EB)> at io 0xffa7f800, 0xffa7f400 irq 17 bufsz 16384 kld snd_ich (1p/1r/0v channels duplex default)
      */

      /* These next two checks handle the case where hw.snd.verbose > 1 */
      if (g_ascii_isspace(toks[i][0]))
        continue;
      if (strcmp(toks[i], "File Versions:") == 0)
        break;

      ptr = strchr(toks[i], ':');
      if (ptr == NULL)
        continue;

      drv = g_strndup(toks[i], strlen(toks[i]) - strlen(ptr));

      ptr = strstr(toks[i], "snd_");
      if (ptr == NULL)
	{
	  g_free(drv);
	  continue;
	}

      descr = g_strdup(ptr);

      g_hash_table_insert(drv_hash, (gpointer)drv, (gpointer)descr);
    }
  g_strfreev(toks);

nosndstat:

  pcm_devices = hal_device_store_match_multiple_key_value_string(hald_get_gdl(),
    "freebsd.driver", "pcm");
  HF_LIST_FOREACH(l, pcm_devices)
    {
      HalDevice *parent = HAL_DEVICE(l->data);

      if (! hal_device_property_get_bool(parent, "info.ignore"))
	{
	  int unit;
	  int j;

	  unit = hal_device_property_get_int(parent, "freebsd.unit");

	  for (j = 0; j < (int) G_N_ELEMENTS(oss_sound_devices); j++)
	      if (! hf_device_store_match(hald_get_gdl(),
                                          "oss.card", HAL_PROPERTY_TYPE_INT32, unit,
					  "oss.type", HAL_PROPERTY_TYPE_STRING, oss_sound_devices[j].type,
					  NULL))
	        {
		  HalDevice *device;

		  device = hf_sound_oss_device_new(parent,
                                                   oss_sound_devices[j].type,
                                                   oss_sound_devices[j].driver);
		  if (device)
		    hf_device_preprobe_and_add(device);
	        }
	}
    }
  g_slist_free(pcm_devices);

  g_hash_table_destroy(drv_hash);
}

HFHandler hf_sound_handler = {
  .probe = hf_sound_probe
};
