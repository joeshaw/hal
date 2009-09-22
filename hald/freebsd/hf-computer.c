/***************************************************************************
 * CVSID: $Id$
 *
 * hf-computer.c : the root computer device
 *
 * Copyright (C) 2006 Jean-Yves Lefort <jylefort@FreeBSD.org>
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
#include <sys/utsname.h>

#include "../hald.h"

#include "hf-computer.h"
#include "hf-util.h"

static void
hf_computer_device_probe (HalDevice *device)
{
  const char *chassis_type;
  const char *formfactor;
  const char *sys_manufacturer;
  const char *sys_product;
  const char *sys_version;

  hf_runner_run_sync(device, 0, "hald-probe-smbios", NULL);

  sys_manufacturer = hal_device_property_get_string(device, "system.hardware.vendor");
  sys_product = hal_device_property_get_string(device, "system.hardware.product");
  sys_version = hal_device_property_get_string(device, "system.hardware.version");

  if (sys_manufacturer && sys_product && sys_version)
    {
      if (strcmp(sys_version, "Not Specified"))
	hf_device_property_set_string_printf(device, "system.product", "%s %s", sys_product, sys_version);
      else
	hal_device_property_set_string(device, "system.product", sys_product);
    }

  chassis_type = hal_device_property_get_string(device, "system.chassis.type");
  formfactor = hal_device_property_get_string(device, "system.formfactor");

  if (chassis_type && (! formfactor || ! strcmp(formfactor, "unknown")))
    {
      int i;
      /* Map the chassis type from dmidecode.c to a sensible type used in hal
       *
       * See also 3.3.4.1 of the "System Management BIOS Reference Specification,
       * Version 2.6.1" (Preliminary Standard) document, available from
       * http://www.dmtf.org/standards/smbios.
       *
       * TODO: figure out WTF the mapping should be; "Lunch Box"? Give me a break :-)
       */
      const char *chassis_map[] = {
	"Other",			"unknown",
	"Unknown",			"unknown",
	"Desktop",			"desktop",
	"Low Profile Desktop",		"desktop",
	"Pizza Box",			"server",
	"Mini Tower",			"desktop",
	"Tower",			"desktop",
	"Portable",			"laptop",
	"Laptop",			"laptop",
	"Notebook",			"laptop",
	"Hand Held",			"handheld",
	"Docking Station",		"laptop",
	"All In One",			"unknown",
	"Sub Notebook",			"laptop",
	"Space-saving",			"desktop",
	"Lunch Box",			"unknown",
	"Main Server Chassis",		"server",
	"Expansion Chassis",		"unknown",
	"Sub Chassis",			"unknown",
	"Bus Expansion Chassis",	"unknown",
	"Peripheral Chassis",		"unknown",
	"RAID Chassis",			"unknown",
	"Rack Mount Chassis",		"unknown",
	"Sealed-case PC",		"unknown",
	"Multi-system",			"unknown",
	"CompactPCI",			"unknown",
	"AdvancedTCA",			"unknown",
	"Blade",                 	"server",
	"Blade Enclosure"        	"unknown" /* 0x1D */
      };

      for (i = 0; i < (int) G_N_ELEMENTS(chassis_map); i += 2)
	if (! strcmp(chassis_map[i], chassis_type))
	  {
	    hal_device_property_set_string(device, "system.formfactor", chassis_map[i + 1]);
	    break;
	  }
    }
}

gboolean
hf_computer_device_add (void)
{
  HalDevice *device;
  struct utsname un;
  const char *power_type = NULL;
  gboolean can_suspend_to_ram = FALSE;
  gboolean can_suspend_to_disk = FALSE;
  gboolean should_decode_dmi = FALSE;

  if (hal_device_store_find(hald_get_gdl(), HF_COMPUTER))
    return TRUE;

  device = hal_device_new();
  hf_device_set_udi(device, "computer");
  hal_device_property_set_string(device, "info.subsystem", "unknown");
  hal_device_property_set_string(device, "info.product", "Computer");

  if (PACKAGE_VERSION) {
      int major, minor, micro;

      hal_device_property_set_string (device, "org.freedesktop.Hal.version", PACKAGE_VERSION);
      if ( sscanf( PACKAGE_VERSION, "%d.%d.%d", &major, &minor, &micro ) == 3 ) {
	hal_device_property_set_int (device, "org.freedesktop.Hal.version.major", major);
        hal_device_property_set_int (device, "org.freedesktop.Hal.version.minor", minor);
        hal_device_property_set_int (device, "org.freedesktop.Hal.version.micro", micro);
      }
  }

  if (uname(&un) == 0)
    {
      hal_device_property_set_string(device, "system.kernel.name", un.sysname);
      hal_device_property_set_string(device, "system.kernel.version", un.release);
      hal_device_property_set_string(device, "system.kernel.machine", un.machine);
    }

  hal_device_property_set_string(device, "system.formfactor", "unknown");

  if (hf_has_sysctl("dev.acpi.0.%%driver"))
    {
      char *states;

      power_type = "acpi";
      should_decode_dmi = TRUE;

      states = hf_get_string_sysctl(NULL, "hw.acpi.supported_sleep_state");
      if (states)
	{
	  char **elements;

	  elements = g_strsplit(states, " ", 0);
	  g_free(states);

	  can_suspend_to_ram = hf_strv_find(elements, "S2") != -1 || hf_strv_find(elements, "S3") != -1;
	  can_suspend_to_disk = hf_strv_find(elements, "S4") != -1;
	  g_strfreev(elements);
	}
    }
  /* FIXME apm, pmu, ... */

  hal_device_property_set_string(device, "power_management.type", power_type);
  hal_device_property_set_bool(device, "power_management.can_suspend", can_suspend_to_ram);
  hal_device_property_set_bool(device, "power_management.can_hibernate", can_suspend_to_disk);

  if (hf_device_preprobe(device))
    {
      if (should_decode_dmi)
	hf_computer_device_probe(device);
      hf_device_add(device);

      return TRUE;
    }
  else
    return FALSE;
}
