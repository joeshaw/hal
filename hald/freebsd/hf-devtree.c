/***************************************************************************
 * CVSID: $Id$
 *
 * hf-devtree.c : generic device tree support
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

#include "../ids.h"
#include "../logger.h"

#include "hf-devtree.h"
#include "hf-acpi.h"
#include "hf-ata.h"
#include "hf-block.h"
#include "hf-drm.h"
#include "hf-pcmcia.h"
#include "hf-storage.h"
#include "hf-util.h"

typedef struct
{
  const char	*driver;
  void		(*set_properties)	(HalDevice *device);
} Handler;

typedef struct
{
  const Handler	*handler;
  int		unit;
} DeviceInfo;

static gboolean
hf_devtree_parse_name (const char *name,
		       char **driver,
		       int *unit)
{
  char *_driver;
  int _unit;
  gboolean status = FALSE;

  g_return_val_if_fail(name != NULL, FALSE);

  _driver = g_new(char, strlen(name) + 1);
  if (sscanf(name, "%[^0-9]%i", _driver, &_unit) == 2)
    {
      if (driver)
	{
	  *driver = _driver;
	  _driver = NULL;
	}
      if (unit)
	*unit = _unit;
      status = TRUE;
    }

  g_free(_driver);
  return status;
}

static gboolean
hf_devtree_cpu_can_throttle (int cpu)
{
  gboolean can = FALSE;
  char *levels;

#ifdef notyet
  levels = hf_get_string_sysctl(NULL, "dev.cpu.%i.freq_levels", cpu);
#else
  levels = hf_get_string_sysctl(NULL, "dev.cpu.0.freq_levels");
#endif
  if (levels)
    {
      char **toks;

      toks = g_strsplit(levels, " ", 0);

      if (g_strv_length(toks) > 1)
        can = TRUE;

      g_strfreev(toks);
      g_free(levels);
    }

  return can;
}

static int
hf_devtree_cpu_get_maxfreq (int cpu)
{
  char *levels;
  int freq = -1;

#ifdef notyet
  levels = hf_get_string_sysctl(NULL, "dev.cpu.%i.freq_levels", cpu);
#else
  levels = hf_get_string_sysctl(NULL, "dev.cpu.0.freq_levels");
#endif
  if (levels)
    {
      sscanf(levels, "%i/", &freq);
      g_free(levels);
    }

  if (freq == -1)
    {
      int ncpu;

      /* freq not found, on UP systems fallback to hw.clockrate */

      if (hf_get_int_sysctl(&ncpu, NULL, "hw.ncpu") && ncpu == 1)
	hf_get_int_sysctl(&freq, NULL, "hw.clockrate");
    }

  return freq;
}

static void
hf_devtree_cpu_set_properties (HalDevice *device)
{
  int unit;
  int freq;
  int ncpu;

  unit = hal_device_property_get_int(device, "freebsd.unit");

  hal_device_property_set_string(device, "info.category", "processor");
  hal_device_add_capability(device, "processor");

  hal_device_property_set_int(device, "processor.number", unit);
  hal_device_property_set_bool(device, "processor.can_throttle", hf_devtree_cpu_can_throttle(unit));

  freq = hf_devtree_cpu_get_maxfreq(unit);
  if (freq != -1)
    hal_device_property_set_int(device, "processor.maximum_speed", freq);

  /* on UP systems, set a better info.product */
  if (hf_get_int_sysctl(&ncpu, NULL, "hw.ncpu") && ncpu == 1)
    {
      char *model;

      model = hf_get_string_sysctl(NULL, "hw.model");
      if (model)
	{
	  hal_device_property_set_string(device, "info.product", model);
	  g_free(model);
	}
    }
}

static void
hf_devtree_fd_set_properties (HalDevice *device)
{
  char *devname;

  devname = hf_devtree_device_get_name(device);
  hf_block_device_enable(device, devname);
  g_free(devname);

  hf_storage_device_enable(device);

  hal_device_property_set_string(device, "storage.drive_type", "floppy");

  hal_device_property_set_bool(device, "storage.removable", TRUE);
  hal_device_property_set_bool(device, "storage.no_partitions_hint", TRUE);

  hal_device_copy_property(device, "info.product", device, "storage.model");

  hf_block_device_complete(device, device, FALSE);
}

static void
hf_devtree_atkbd_set_properties (HalDevice *device)
{
  hf_device_set_input(device, "keyboard", "keys", NULL);
}

static void
hf_devtree_psm_set_properties (HalDevice *device)
{
  char *devname;

  devname = hf_devtree_device_get_name(device);
  hf_device_set_input(device, "mouse", NULL, devname);
  g_free(devname);
}

static void
hf_devtree_joy_set_properties (HalDevice *device)
{
  char *devname;

  devname = hf_devtree_device_get_name(device);
  hf_device_set_input(device, "joystick", NULL, devname);
  g_free(devname);

  if (! hal_device_has_property(device, "info.product"))
    hal_device_property_set_string(device, "info.product", "PC Joystick");
}

static HalDevice *
hf_devtree_device_new (HalDevice *parent, const Handler *handler, int unit)
{
  HalDevice *device;
  char *desc;
  char *pnpinfo;

  g_return_val_if_fail(handler != NULL, NULL);

  device = hf_device_new(parent);

  hf_device_set_udi(device, "%s_%i", handler->driver, unit);

  desc = hf_get_string_sysctl(NULL, "dev.%s.%i.%%desc", handler->driver, unit);
  if (desc && *desc)
    hal_device_property_set_string(device, "info.product", desc);
  g_free(desc);

  hf_devtree_device_set_info(device, handler->driver, unit);

  /* find PNP ID */
  pnpinfo = hf_get_string_sysctl(NULL, "dev.%s.%i.%%pnpinfo", handler->driver, unit);
  if (pnpinfo)
    {
      char **items;
      int i;

      items = g_strsplit(pnpinfo, " ", 0);
      g_free(pnpinfo);

      for (i = 0; items[i]; i++)
	if (g_str_has_prefix(items[i], "_HID="))
	  {
	    if (strcmp(items[i], "_HID=none"))
              {
                char *pnp_description;

	        hal_device_property_set_string(device, "pnp.id", items[i] + 5);

		ids_find_pnp(items[i] + 5, &pnp_description);
		if (pnp_description)
                  {
                    hal_device_property_set_string(device, "pnp.description", pnp_description);
		    if (! hal_device_has_property(device, "info.product"))
                      hal_device_property_set_string(device, "pnp.description", pnp_description);
		  }
	      }

	    break;
	  }
      g_strfreev(items);
    }

  if (handler->set_properties)
    handler->set_properties(device);

  if (! hal_device_has_property(device, "info.subsystem"))
    {
      hal_device_property_set_string(device, "info.subsystem", "platform");
      hf_device_property_set_string_printf(device, "platform.id", "%s.%i", handler->driver, unit);
    }

  return device;
}

static gboolean
hf_devtree_device_is (HalDevice *device)
{
  g_return_val_if_fail(HAL_IS_DEVICE(device), FALSE);

  return hal_device_has_property(device, "freebsd.driver");
}

static GSList *
hf_devtree_lookup (GSList *devices, const char *driver, int unit)
{
  GSList *l;

  g_return_val_if_fail(driver != NULL, NULL);

  HF_LIST_FOREACH(l, devices)
    {
      DeviceInfo *info = l->data;

      if (! strcmp(info->handler->driver, driver) && info->unit == unit)
	return l;
    }

  return NULL;
}

static GSList *
hf_devtree_get_root (GSList *devices)
{
  GSList *root;
  DeviceInfo *info;
  char *driver;
  int unit;

  g_return_val_if_fail(devices != NULL, NULL);

  root = devices;
  info = root->data;

  driver = g_strdup(info->handler->driver);
  unit = info->unit;

  while (driver)
    {
      char *parent_name;

      parent_name = hf_get_string_sysctl(NULL, "dev.%s.%i.%%parent", driver, unit);

      g_free(driver);
      driver = NULL;

      if (parent_name)
	{
	  if (hf_devtree_parse_name(parent_name, &driver, &unit))
	    {
	      GSList *new_root;

	      new_root = hf_devtree_lookup(devices, driver, unit);
	      if (new_root)
		{
		  root = new_root;
		  info = root->data;

		  g_free(driver);
		  driver = g_strdup(info->handler->driver);
		  unit = info->unit;
		}
	    }
	  g_free(parent_name);
	}
    }

  return root;
}

static Handler handlers[] = {
  { "acpi_acad",	hf_acpi_acad_set_properties		},
  { "acpi_asus",	NULL					},
  { "acpi_button",	hf_acpi_button_set_properties		},
  { "acpi_fujitsu",	NULL					},
  { "acpi_ibm",		NULL					},
  { "acpi_lid",		hf_acpi_button_set_properties		},
  { "acpi_panasonic",	NULL					},
  { "acpi_sony",	NULL					},
  { "acpi_toshiba",	NULL					},
  { "acpi_tz",		hf_acpi_tz_set_properties		},
  { "acpi_video",	NULL					},
  { "ata",		hf_ata_channel_set_properties		},
  { "atkbd",		hf_devtree_atkbd_set_properties		},
  { "atkbdc",		NULL					},
  { "battery",		hf_acpi_battery_set_properties		},
  { "cardbus",		hf_pcmcia_set_properties		},
  { "cpu",		hf_devtree_cpu_set_properties		},
  { "drm",		hf_drm_set_properties			},
  { "fd",		hf_devtree_fd_set_properties		},
  { "fdc",		NULL					},
  { "joy",		hf_devtree_joy_set_properties		},
  { "nvidia",		NULL					},
  { "pccard",		hf_pcmcia_set_properties		},
  { "pcm",		NULL					},
  { "psm",		hf_devtree_psm_set_properties		},
  { "sio",		NULL					},
  { "speaker",		NULL					},
  { "usbus",		NULL					}
};

static void
hf_devtree_probe (void)
{
  GSList *devices = NULL;
  int i;

  /* build a list of devices */

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    {
      int j;

      for (j = 0; hf_has_sysctl("dev.%s.%i.%%driver", handlers[i].driver, j); j++)
	if (! hf_devtree_find_from_info(hald_get_gdl(), handlers[i].driver, j))
	  {
	    DeviceInfo *info;

	    info = g_new(DeviceInfo, 1);
	    info->handler = &handlers[i];
	    info->unit = j;

	    devices = g_slist_prepend(devices, info);
	  }
    }

  /* add the devices (parents first) */

  while (devices)
    {
      GSList *root;
      DeviceInfo *info;
      HalDevice *parent;

      root = hf_devtree_get_root(devices);
      g_assert(root != NULL);

      info = root->data;

      parent = hf_devtree_find_parent_from_info(hald_get_gdl(), info->handler->driver, info->unit);
      if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
	{
	  HalDevice *device;

	  device = hf_devtree_device_new(parent, info->handler, info->unit);
	  if (hf_device_preprobe(device))
            {
              if (hal_device_has_capability(device, "input.mouse"))
                hf_runner_run_sync(device, 0, "hald-probe-mouse", NULL);

	      hf_device_add(device);
	    }
	}

      devices = g_slist_delete_link(devices, root);
      g_free(info);
    }
}

static gboolean
hf_devtree_rescan (HalDevice *device)
{
  if (hal_device_has_capability(device, "input.mouse"))
    {
      hf_runner_run_sync(device, 0, "hald-probe-mouse", NULL);
      return TRUE;
    }
  return FALSE;
}

HalDevice *
hf_devtree_find_from_name (HalDeviceStore *store, const char *name)
{
  char *driver;
  int unit;
  HalDevice *device = NULL;

  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);
  g_return_val_if_fail(name != NULL, NULL);

  if (hf_devtree_parse_name(name, &driver, &unit))
    {
      device = hf_devtree_find_from_info(store, driver, unit);
      g_free(driver);
    }

  return device;
}

HalDevice *
hf_devtree_find_from_info (HalDeviceStore *store, const char *driver, int unit)
{
  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);
  g_return_val_if_fail(driver != NULL, NULL);

  return hf_device_store_match(store,
                               "freebsd.driver", HAL_PROPERTY_TYPE_STRING, driver,
			       "freebsd.unit", HAL_PROPERTY_TYPE_INT32, unit,
			       NULL);
}

HalDevice *
hf_devtree_find_parent_from_name (HalDeviceStore *store, const char *name)
{
  char *driver;
  int unit;
  HalDevice *device = NULL;

  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);
  g_return_val_if_fail(name != NULL, NULL);

  if (hf_devtree_parse_name(name, &driver, &unit))
    {
      device = hf_devtree_find_parent_from_info(store, driver, unit);
      g_free(driver);
    }

  return device;
}

HalDevice *
hf_devtree_find_parent_from_info (HalDeviceStore *store,
				  const char *driver,
				  int unit)
{
  HalDevice *parent = NULL;
  char *driver_iter;

  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);
  g_return_val_if_fail(driver != NULL, NULL);

  driver_iter = g_strdup(driver);
  while (! parent && driver_iter)
    {
      char *parent_name;

      parent_name = hf_get_string_sysctl(NULL, "dev.%s.%i.%%parent", driver_iter, unit);

      g_free(driver_iter);
      driver_iter = NULL;

      if (parent_name)
	{
	  parent = hf_devtree_find_from_name(store, parent_name);
	  if (! parent)
	    hf_devtree_parse_name(parent_name, &driver_iter, &unit);
	  g_free(parent_name);
	}
    }

  return parent;
}

void
hf_devtree_device_set_info (HalDevice *device, const char *driver, int unit)
{
  char *devfile;

  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(driver != NULL);

  hal_device_property_set_string(device, "freebsd.driver", driver);
  hal_device_property_set_int(device, "freebsd.unit", unit);

  devfile = g_strdup_printf("/dev/%s%i", driver, unit);
  if (g_file_test(devfile, G_FILE_TEST_EXISTS))
    hf_device_property_set_string_printf(device, "freebsd.device_file", devfile);
  g_free(devfile);
}

gboolean
hf_devtree_device_get_info (HalDevice *device, const char **driver, int *unit)
{
  g_return_val_if_fail(HAL_IS_DEVICE(device), FALSE);

  if (hf_devtree_device_is(device))
    {
      if (driver)
	*driver = hal_device_property_get_string(device, "freebsd.driver");
      if (unit)
	*unit = hal_device_property_get_int(device, "freebsd.unit");

      return TRUE;
    }
  else
    return FALSE;
}

void
hf_devtree_device_set_name (HalDevice *device, const char *name)
{
  char *driver;
  int unit;

  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(name != NULL);

  if (hf_devtree_parse_name(name, &driver, &unit))
    {
      hf_devtree_device_set_info(device, driver, unit);
      g_free(driver);
    }
}

char *
hf_devtree_device_get_name (HalDevice *device)
{
  g_return_val_if_fail(HAL_IS_DEVICE(device), NULL);

  if (hf_devtree_device_is(device))
    return g_strdup_printf("%s%i",
			   hal_device_property_get_string(device, "freebsd.driver"),
			   hal_device_property_get_int(device, "freebsd.unit"));
  else
    return NULL;
}

gboolean
hf_devtree_is_driver (const char *name, const char *driver)
{
  char *unit;

  g_return_val_if_fail(name != NULL, FALSE);
  g_return_val_if_fail(driver != NULL, FALSE);

  unit = strpbrk(name, "0123456789");
  if (unit)
    return ! strncmp(name, driver, unit - name);
  else
    return FALSE;
}

HFHandler hf_devtree_handler = {
  .probe = hf_devtree_probe,
  .device_rescan = hf_devtree_rescan
};
