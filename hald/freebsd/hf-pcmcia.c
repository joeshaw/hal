/***************************************************************************
 * CVSID: $Id$
 *
 * hf-pcmcia.c : PCMCIA processing functions
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

#include <stdio.h>
#include <string.h>

#include "../hald_dbus.h"
#include "../logger.h"

#include "hf-pcmcia.h"
#include "hf-devtree.h"
#include "hf-util.h"

static void
hf_pcmcia_set_oem_info (HalDevice *device,
				const char *info,
				const char *info_property,
				const char *id_property)
{
  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(info_property != NULL);
  g_return_if_fail(id_property != NULL);

  if (info)
    hal_device_property_set_string(device, info_property, info);
  else
    {
      int id;

      id = hal_device_property_get_int(device, id_property);
      if (id > -1)
	hf_device_property_set_string_printf(device, info_property, "Unknown (0x%04x)", id);
    }
}

static void
hf_pcmcia_update_cis (HalDevice *device)
{
  char *devname;
  char *output;
  char *vendor = NULL, *product = NULL;
  char **lines;
  const char *command;
  int i;

  devname = hf_devtree_device_get_name(device);
  if (g_file_test("/usr/sbin/dumpcis", G_FILE_TEST_IS_EXECUTABLE))
    {
      command = "/usr/sbin/dumpcis";
    }
  else
    {
      command = "/usr/sbin/pccardc dumpcisfile";
    }
  output = hf_run(NULL, "%s /dev/%s.cis", command, devname);
  g_free(devname);

  if (! output)
    /* The CIS device may not exist, or the slot might be empty. */
    return;

  lines = g_strsplit(output, "\n", 0);
  g_free(output);

  for (i = 0; lines[i]; i++)
    {
      if (g_str_has_prefix(lines[i], "\tPCMCIA ID ="))
        {
          int manuf_id, card_id;

	  if (sscanf(lines[i], "\tPCMCIA ID = 0x%x, OEM ID = 0x%x", &manuf_id,
              &card_id))
            {
              hal_device_property_set_int(device, "pcmcia.manf_id", manuf_id);
	      hal_device_property_set_int(device, "pcmcia.card_id", card_id);
	    }
	}
	if (strstr(lines[i], "Functional ID"))
          {
            if (lines[i + 1])
              {
                int func_id;

                if (sscanf(lines[i + 1], "   000:  %x", &func_id))
                  hal_device_property_set_int(device, "pcmcia.func_id",
                                              func_id);
	      }
	  }
	if (g_str_has_prefix(lines[i], "\tVersion ="))
          {
            char **toks;

	    toks = g_strsplit_set(lines[i], "[]", 0);
	    if (g_strv_length(toks) >= 2)
	      vendor = g_strdup(toks[1]);
	    g_strfreev(toks);
	  }
        if (g_str_has_prefix(lines[i], "\tAddit. info ="))
          {
            char **toks;

            toks = g_strsplit_set(lines[i], "[]", 0);
            if (g_strv_length(toks) >= 2)
	      product = g_strdup(toks[1]);
            g_strfreev(toks);
          }
    }
  g_strfreev(lines);

  hf_pcmcia_set_oem_info(device, vendor, "info.vendor", "pcmcia.manf_id");
  g_free(vendor);

  hf_pcmcia_set_oem_info(device, product, "info.product", "pcmcia.card_id");
  g_free(product);
}

void
hf_pcmcia_set_properties (HalDevice *device)
{

  hal_device_property_set_string(device, "info.subsystem", "pcmcia");
  hal_device_add_capability(device, "pcmcia_socket");
  hal_device_property_set_string(device, "info.category", "pcmcia_socket");
  hal_device_property_set_int(device, "pcmcia_socket.number",
    hal_device_property_get_int(device, "freebsd.unit"));
  hf_pcmcia_update_cis(device);
}

static gboolean
hf_pcmcia_devd_add (const char *name,
		    GHashTable *params,
		    GHashTable *at,
		    const char *parent)
{
  HalDevice *device;

  if (! parent)
    return FALSE;
  if (! hf_devtree_is_driver(parent, "pccard") && ! hf_devtree_is_driver(parent, "cardbus"))
    return FALSE;

  device = hf_devtree_find_from_name(hald_get_gdl(), parent);
  if (device)
    {
      if (name)
        HAL_INFO(("found new PC Card %s on %s", name, parent));
      else
        HAL_INFO(("updating PC Card information for %s", parent));

      device_property_atomic_update_begin();
      hf_pcmcia_update_cis(device);
      device_property_atomic_update_end();

      return TRUE;
    }

  return FALSE;
}

static gboolean
hf_pcmcia_devd_nomatch (GHashTable *at,
                        const char *parent)
{
  return hf_pcmcia_devd_add(NULL, NULL, NULL, parent);
}

static gboolean
hf_pcmcia_devd_remove (const char *name,
                       GHashTable *params,
		       GHashTable *at,
		       const char *parent)
{
  if (! parent)
    return FALSE;
  if (! hf_devtree_is_driver(parent, "pccard") && ! hf_devtree_is_driver(parent, "cardbus"))
    return FALSE;

  HAL_INFO(("detected removal of PC Card %s", name));
  return hf_pcmcia_devd_add(NULL, NULL, NULL, parent);
}

HFDevdHandler hf_pcmcia_devd_handler = {
  .add =	hf_pcmcia_devd_add,
  .remove =	hf_pcmcia_devd_remove,
  .nomatch =	hf_pcmcia_devd_nomatch
};
