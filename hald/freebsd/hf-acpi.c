/***************************************************************************
 * CVSID: $Id$
 *
 * hf-acpi.c : poll for ACPI properties
 *
 * Copyright (C) 2006, 2007 Joe Marcus Clarke <marcus@FreeBSD.org>
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

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <dev/acpica/acpiio.h>
#include <glib.h>

#include "../hald.h"
#include "../hald_dbus.h"
#include "../logger.h"
#include "../util.h"
#include "../util_pm.h"

#include "hf-acpi.h"
#include "hf-util.h"

#ifndef ACPI_BIF_UNITS_MA
#define ACPI_BIF_UNITS_MA	1 /* added to CURRENT on 20051023 */
#endif

#define HF_ACPIDEV			"/dev/acpi"

static const struct laptop_panel_type {
  char *access;
  char *name;
  char *get_sysctl;
  char *max_sysctl;
  int max_levels;
#define HF_ACPI_IBM_MAX_LEVELS		8
#define HF_ACPI_TOSHIBA_MAX_LEVELS	8
#define HF_ACPI_SONY_MAX_LEVELS		8
#define HF_ACPI_PANASONIC_MAX_LEVELS	16 /* XXX This is a fallback */
#define HF_ACPI_ASUS_MAX_LEVELS		16
#define HF_ACPI_FUJITSU_MAX_LEVELS	8
  /* NOTE: Each new type must also be added to hf-devtree.c */
} laptop_panel_types[] = {
  { "ibm",		"IBM",
    "dev.acpi_ibm.0.lcd_brightness",
    NULL,
    HF_ACPI_IBM_MAX_LEVELS },
  { "toshiba",		"Toshiba",
    "hw.acpi.toshiba.lcd_brightness",
    NULL,
    HF_ACPI_TOSHIBA_MAX_LEVELS },
  { "sony",		"Sony",
    "dev.acpi_sony.0.brightness",
    NULL,
    HF_ACPI_SONY_MAX_LEVELS },
  { "panasonic",	"Panasonic",
    "hw.acpi.panasonic.lcd_brightness",
    "hw.acpi.panasonic.lcd_brightness_max",
    HF_ACPI_PANASONIC_MAX_LEVELS },
  { "asus",		"Asus",
    "hw.acpi.asus.lcd_brightness",
    NULL,
    HF_ACPI_ASUS_MAX_LEVELS },
  { "fujitsu",		"Fujitsu",
    "hw.acpi.fujitsu.lcd_brightness",
    NULL,
    HF_ACPI_FUJITSU_MAX_LEVELS }
};

static const char *video_outs[] = {
  "crt",
  "lcd",
  "tv",
  "out"
};

static void
hf_acpi_poll_acad (HalDevice *device)
{
  int acline;

  if (! hf_get_int_sysctl(&acline, NULL, "hw.acpi.acline"))
    return;

  hal_device_property_set_bool(device, "ac_adapter.present",
                               acline ? TRUE : FALSE);
}

static void
hf_acpi_poll_batt (HalDevice *device)
{
  int fd;
  int volt, dvolt, rate, lastfull, cap, dcap, lcap, wcap, gra1, gra2;
  gboolean ispresent;
  union acpi_battery_ioctl_arg battif, battst, battinfo;

  if (! hf_has_sysctl("hw.acpi.battery.units"))
    return;

  battif.unit = battst.unit = battinfo.unit =
    hal_device_property_get_int(device, "freebsd.unit");

  fd = open(HF_ACPIDEV, O_RDONLY);
  if (fd < 0)
    {
      HAL_WARNING(("unable to open %s: %s", HF_ACPIDEV, g_strerror(errno)));
      return;
    }

#ifdef ACPIIO_BATT_GET_BIF
  if (ioctl(fd, ACPIIO_BATT_GET_BIF, &battif) == -1)
#else
  if (ioctl(fd, ACPIIO_CMBAT_GET_BIF, &battif) == -1)
#endif
    {
      HAL_WARNING(("ioctl ACPIIO_BATT_GET_BIF failed for battery %d: %s",
        battif.unit, g_strerror(errno)));
      goto end;
    }
#ifdef ACPIIO_BATT_GET_BST
  if (ioctl(fd, ACPIIO_BATT_GET_BST, &battst) == -1)
#else
  if (ioctl(fd, ACPIIO_CMBAT_GET_BST, &battst) == -1)
#endif
    {
      HAL_WARNING(("ioctl ACPIIO_BATT_GET_BST failed for battery %d: %s",
        battst.unit, g_strerror(errno)));
      goto end;
    }
  if (ioctl(fd, ACPIIO_BATT_GET_BATTINFO, &battinfo) == -1)
    {
      HAL_WARNING(("ioctl ACPIIO_BATT_GET_BATTINFO failed for battery %d: %s",
        battinfo.unit, g_strerror(errno)));
      goto end;
    }

  ispresent = (battst.bst.state == ACPI_BATT_STAT_NOT_PRESENT) ? FALSE : TRUE;
  hal_device_property_set_bool(device, "battery.present", ispresent);

  if (! ispresent)
    goto end;

  dvolt = battif.bif.dvol;
  volt = battst.bst.volt;
  cap = battst.bst.cap;
  dcap = battif.bif.dcap;
  rate = battst.bst.rate;
  lastfull = battif.bif.lfcap;
  lcap = battif.bif.lcap;
  wcap = battif.bif.wcap;
  gra1 = battif.bif.gra1;
  gra2 = battif.bif.gra2;

  hal_device_property_set_string(device, "battery.voltage.unit", "mV");
  hal_device_property_set_int(device, "battery.voltage.current", volt);
  hal_device_property_set_int(device, "battery.voltage.design", dvolt);

  hal_device_property_set_int(device, "battery.reporting.design", dcap);
  hal_device_property_set_int(device, "battery.reporting.current", cap);
  hal_device_property_set_int(device, "battery.reporting.rate", rate);
  hal_device_property_set_int(device, "battery.reporting.last_full", lastfull);
  hal_device_property_set_int(device, "battery.reporting.low", lcap);
  hal_device_property_set_int(device, "battery.reporting.warning", wcap);

  hal_device_property_set_string(device, "battery.charge_level.unit", "mWh");

  /*
   * ACPI gives out the special 'Ones' value for rate when it's unable
   * to calculate the true rate. We should set the rate zero, and wait
   * or the BIOS to stabilize.
   *
   * full details here: http://bugzilla.gnome.org/show_bug.cgi?id=348201
   */
  if (rate == 0xffff)
    rate = 0;

  if (battif.bif.units == ACPI_BIF_UNITS_MA)
    {
       hal_device_property_set_string(device, "battery.reporting.units", "mAh");

       if (dvolt <= 0)
         dvolt = 1;
       if (volt <= 0 || volt > dvolt)
         volt = dvolt;

       cap = (int) rint((cap * volt) / 1000.0);
       dcap = (int) rint((dcap * volt) / 1000.0);
       rate = (int) rint((rate * volt) / 1000.0);
       lastfull = (int) rint((lastfull * volt) / 1000.0);
       lcap = (int) rint((lcap * volt) / 1000.0);
       wcap = (int) rint((wcap * volt) / 1000.0);
       gra1 = (int) rint((gra1 * volt) / 1000.0);
       gra2 = (int) rint((gra2 * volt) / 1000.0);
    }
  else
    hal_device_property_set_string(device, "battery.reporting.unit", "mWh");

  hal_device_property_set_int(device, "battery.charge_level.design", dcap);
  hal_device_property_set_int(device, "battery.charge_level.last_full",
                              lastfull);
  hal_device_property_set_int(device, "battery.charge_level.current", cap);
  hal_device_property_set_int(device, "battery.charge_level.rate", rate);
  hal_device_property_set_int(device, "battery.charge_level.warning", wcap);
  hal_device_property_set_int(device, "battery.charge_level.low", lcap);
  hal_device_property_set_int(device, "battery.charge_level.granularity_1",
                              gra1);
  hal_device_property_set_int(device, "battery.charge_level.granularity_2",
                              gra2);


  hal_device_property_set_bool(device, "battery.is_rechargeable",
                               battif.bif.btech == 0 ? FALSE : TRUE);
  hal_device_property_set_int(device, "battery.charge_level.percentage",
                              battinfo.battinfo.cap);

  if (hal_device_property_get_bool(device, "battery.is_rechargeable"))
    {
      hal_device_property_set_bool(device, "battery.rechargeable.is_charging",
                                   battinfo.battinfo.state & ACPI_BATT_STAT_CHARGING ? TRUE : FALSE);
      hal_device_property_set_bool(device, "battery.rechargeable.is_discharging",
                                   battinfo.battinfo.state & ACPI_BATT_STAT_DISCHARG ? TRUE : FALSE);
    }

  /* remaining time is in seconds */
  if (battinfo.battinfo.min > 0)
    {
      hal_device_property_set_int(device, "battery.remaining_time",
                              battinfo.battinfo.min * 60);
      hal_device_property_set_bool(device, "battery.remaining_time.calculate_per_time", FALSE);
    }
  else
    {
      int remaining_time;

      remaining_time = util_compute_time_remaining(hal_device_get_udi(device), rate, cap,
		                                   lastfull,
                                                   hal_device_property_get_bool(device, "battery.rechargeable.is_discharging"),
		                                   hal_device_property_get_bool(device, "battery.rechargeable.is_charging"),
		                                   hal_device_property_get_bool(device, "battery.remaining_time.calculate_per_time"));
      if (remaining_time > 0)
        hal_device_property_set_int(device, "battery.remaining_time",
                                    remaining_time);
      else
        hal_device_property_remove(device, "battery.remaining_time");
    }

  hal_device_property_set_string(device, "info.vendor", battif.bif.oeminfo);

  hal_device_property_set_string(device, "battery.vendor", battif.bif.oeminfo);
  hal_device_property_set_string(device, "battery.model", battif.bif.model);
  hal_device_property_set_string(device, "battery.technology", battif.bif.type);
  hal_device_property_set_string(device, "battery.serial", battif.bif.serial);

end:
  close(fd);
}

static void
hf_acpi_poll_video (HalDevice *device)
{
  const char *type;
  int unit;
  int level;

  type = hal_device_property_get_string(device, "display_device.type");
  if (strcmp(type, "lcd") != 0)
    /* Only LCD device support brightness */
    return;

  unit = hal_device_property_get_int(device, "freebsd.unit");

  /* This value is returned as a percent from the sysctl, and a percent
   * is required by HAL */
  if (hf_get_int_sysctl(&level, NULL, "dev.acpi.video.lcd%i.brightness", unit))
    hal_device_property_set_int(device, "display_device.lcd.brightness", level);
  else
    /* XXX Some devices support ACPI video, but do not support setting the
     * brightness level via ACPI.  For those, we just assume it's 100%. */
    hal_device_property_set_int(device, "display_device.lcd.brightness", 100);
}

static void
hf_acpi_button_update_state (HalDevice *device, gboolean isclosed)
{
  /* Only Lid buttons will report state changes */
  if (strcmp(hal_device_property_get_string(device, "button.type"), "lid"))
    return;

  hal_device_property_set_bool(device, "button.has_state", TRUE);
  hal_device_property_set_bool(device, "button.state.value", isclosed);
}

void
hf_acpi_button_set_properties (HalDevice *device)
{
  const char *pnpid;
  const char *type = NULL;

  hal_device_property_set_string(device, "info.category", "button");
  hal_device_add_capability(device, "button");

  pnpid = hal_device_property_get_string(device, "pnp.id");
  if (pnpid)
    {
      if (! strcmp(pnpid, "PNP0C0C"))
	type = "power";
      else if (! strcmp(pnpid, "PNP0C0D"))
	type = "lid";
      else if (! strcmp(pnpid, "PNP0C0E"))
	type = "sleep";
    }

  if (type)
    {
      hal_device_property_set_string(device, "button.type", type);
      if (! strcmp(type, "lid"))
        {
          char *lid_state;

	  lid_state = hf_get_string_sysctl(NULL, "hw.acpi.lid_switch_state");
	  if (lid_state && ! strcmp(lid_state, "NONE"))
            hal_device_property_set_bool(device, "info.ignore", TRUE);
	  g_free(lid_state);
	}
      /* XXX This is a bit of hack.  We can only accurately set the lid
       * state AFTER a state event.  Therefore, we assume it's open by
       * default. */
      hf_acpi_button_update_state(device, FALSE);
    }
}

void
hf_acpi_tz_set_properties (HalDevice *device)
{
  hal_device_property_set_string(device, "info.category", "sensor");
  hal_device_add_capability(device, "sensor");

  hal_device_property_set_string(device, "sensor.type", "temperature");
  hal_device_property_set_string(device, "sensor.location", "cpu");
}

void
hf_acpi_acad_set_properties (HalDevice *device)
{
  hal_device_property_set_string(device, "info.category", "ac_adapter");
  hal_device_add_capability(device, "ac_adapter");
  if (hal_device_property_get_int(device, "freebsd.unit") > 0)
    /* XXX We only handle one acad device since there is no way to get
     * other devices' statuses */
    return;
  hf_acpi_poll_acad(device);
}

void
hf_acpi_battery_set_properties (HalDevice *device)
{
    hal_device_property_set_string(device, "battery.type", "primary");
    hal_device_property_set_string(device, "info.category", "battery");
    hal_device_add_capability(device, "battery");
    hf_acpi_poll_batt(device);
}

static gboolean
hf_acpi_poll_all_acads (void)
{
  HalDevice *device;

  /* XXX FreeBSD currently only has one AC adapter (the system AC adapter).
   * Therefore, we ensure that only the first AC adapter will be matched. */
  device = hal_device_store_match_key_value_string(hald_get_gdl(),
                                                   "info.category",
                                                   "ac_adapter");
  if (device != NULL)
    {
      device_property_atomic_update_begin();
      hf_acpi_poll_acad(device);
      device_property_atomic_update_end();

      return TRUE;
    }
  else
    return FALSE;
}

static gboolean
hf_acpi_poll_all_batts (void)
{
  GSList *l;
  GSList *batts;
  HalDevice *device;
  gboolean result = FALSE;

  batts = hal_device_store_match_multiple_key_value_string(hald_get_gdl(),
                                                           "info.category",
                                                           "battery");

  HF_LIST_FOREACH(l, batts)
    {
      device = HAL_DEVICE(l->data);
      device_property_atomic_update_begin();
      hf_acpi_poll_batt(device);
      device_property_atomic_update_end();
      result = TRUE;
    }
  g_slist_free(batts);

  return result;
}

static gboolean
hf_acpi_poll_all_videos (void)
{
  GSList *vouts, *l;
  HalDevice *device;
  gboolean result = FALSE;

  vouts = hal_device_store_match_multiple_key_value_string(hald_get_gdl(),
    "info.category",
    "display_device");

  HF_LIST_FOREACH(l, vouts)
    {
      device = HAL_DEVICE(l->data);
      device_property_atomic_update_begin();
      hf_acpi_poll_video(device);
      device_property_atomic_update_end();
      result = TRUE;
    }
  g_slist_free(vouts);

  return result;
}

static gboolean
hf_acpi_poll_cb (gpointer data)
{
  gboolean result = FALSE;

  if (hf_is_waiting)
    return TRUE;

  result |= hf_acpi_poll_all_acads();
  result |= hf_acpi_poll_all_batts();
  result |= hf_acpi_poll_all_videos();

  if (! result)
    return FALSE;

  return TRUE;
}

static HalDevice *
hf_acpi_video_device_new (HalDevice *parent, const char *type)
{
  HalDevice *device;
  char *product;
  int unit;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  unit = hal_device_property_get_int(parent, "freebsd.unit");
  if (! hf_has_sysctl("hw.acpi.video.%s%i.active", type, unit))
    return NULL;

  device = hf_device_new(parent);

  product = g_strdup_printf("%s (%s)",
    (hal_device_property_get_string(parent, "info.product") != NULL) ?
    hal_device_property_get_string(parent, "info.product") : "Video Output",
    type);
  hal_device_property_set_string(device, "info.product", type);
  g_free(product);

  /* We need this for polling purposes */
  hal_device_property_set_int(device, "freebsd.unit", unit);

  hal_device_property_set_string(device, "info.category", "display_device");
  hal_device_add_capability(device, "display_device");

  hal_device_property_set_string(device, "display_device.type", type);

  hf_device_set_full_udi(device, "%s_display_device_%s_%i", hal_device_get_udi(parent),
                         type, unit);

  hf_acpi_poll_video(device);

  return device;
}

static HalDevice *
hf_acpi_laptop_panel_new (HalDevice *parent, int max_levels,
                          const char *get_sysctl, const char *max_sysctl,
                          const char *access, const char *name)
{
  HalDevice *device;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);

  if (get_sysctl == NULL || ! hf_has_sysctl(get_sysctl))
    return NULL;
  device = hf_device_new(parent);

  hf_device_property_set_string_printf(device, "info.product", "Laptop Panel (%s)", name);

  hal_device_property_set_string(device, "info.category", "laptop_panel");
  hal_device_add_capability(device, "laptop_panel");

  hal_device_property_set_string(device, "laptop_panel.access_method", access);
  if (max_sysctl == NULL)
    hal_device_property_set_int(device, "laptop_panel.num_levels", max_levels);
  else
    {
      int bmax;

      if (hf_get_int_sysctl(&bmax, NULL, max_sysctl))
        hal_device_property_set_int(device, "laptop_panel.num_levels", bmax);
      else
        hal_device_property_set_int(device, "laptop_panel.num_levels", max_levels);
    }

  hf_device_set_full_udi(device, "%s_laptop_panel_%s", hal_device_get_udi(parent), access);

  return device;
}

static void
hf_acpi_init (void)
{
  g_timeout_add(30000, hf_acpi_poll_cb, NULL);
}

static void
hf_acpi_probe (void)
{
  GSList *video_devices, *l;
  int i;

  video_devices = hal_device_store_match_multiple_key_value_string(
    hald_get_gdl(), "freebsd.driver", "acpi_video");
  HF_LIST_FOREACH(l, video_devices)
    {
      HalDevice *parent = HAL_DEVICE(l->data);

      if (! hal_device_property_get_bool(parent, "info.ignore"))
        {
	  int unit;
          int j;

	  unit = hal_device_property_get_int(parent, "freebsd.unit");

	  for (j = 0; j < (int) G_N_ELEMENTS(video_outs); j++)
            {
	      if (! hf_device_store_match(hald_get_gdl(),
                                          "display_device.type", HAL_PROPERTY_TYPE_STRING, video_outs[j],
					  "freebsd.unit", HAL_PROPERTY_TYPE_INT32, unit,
					  NULL)) {
		  HalDevice *device;

		  device = hf_acpi_video_device_new(parent, video_outs[j]);
		  if (device)
		    hf_device_preprobe_and_add(device);
	        }
	    }
	  }
      }
  g_slist_free(video_devices);

  for (i = 0; i < (int) G_N_ELEMENTS(laptop_panel_types); i++)
    {
      HalDevice *parent;
      char *pname;

      pname = g_strdup_printf("acpi_%s", laptop_panel_types[i].access);

      /* There should only ever be one of these.  But we only care about the
       * first one anyway. */
      parent = hal_device_store_match_key_value_string(hald_get_gdl(),
        "freebsd.driver", pname);
      g_free(pname);

      if (parent && ! hal_device_property_get_bool(parent, "info.ignore"))
        {
          if (! hal_device_store_match_key_value_string(hald_get_gdl(),
                                                        "laptop_panel.access_method",
							 laptop_panel_types[i].access))
            {
              HalDevice *panel_device;

	      panel_device = hf_acpi_laptop_panel_new(parent,
						      laptop_panel_types[i].max_levels,
						      laptop_panel_types[i].get_sysctl,
						      laptop_panel_types[i].max_sysctl,
						      laptop_panel_types[i].access,
						      laptop_panel_types[i].name);
	      if (panel_device)
		hf_device_preprobe_and_add(panel_device);
	    }
	}
    }
}

static gboolean
hf_acpi_device_rescan (HalDevice *device)
{
  if (hal_device_has_capability(device, "ac_adapter"))
    hf_acpi_poll_acad(device);
  else if (hal_device_has_capability(device, "battery"))
    hf_acpi_poll_batt(device);
  else if (hal_device_has_capability(device, "display_device"))
    hf_acpi_poll_video(device);
  else
    return FALSE;

  return TRUE;
}

static gboolean
hf_acpi_devd_notify (const char *system,
		     const char *subsystem,
		     const char *type,
		     const char *data)
{
  if (strcmp(system, "ACPI"))
    return FALSE;

  if (! strcmp(subsystem, "ACAD"))
    hf_acpi_poll_all_acads();
  else if (! strcmp(subsystem, "CMBAT"))
    {
      char *ptr;
      int unit;

      ptr = strstr(type, ".BAT");

      if (ptr && sscanf(ptr, ".BAT%i", &unit))
	{
	  HalDevice *cmbat;

	  cmbat = hf_device_store_match(hald_get_gdl(),
                                        "info.category", HAL_PROPERTY_TYPE_STRING, "battery",
					"freebsd.unit", HAL_PROPERTY_TYPE_INT32, unit,
					NULL);

	  if (cmbat)
	    hf_acpi_poll_batt(cmbat);
	  else
	    hf_acpi_poll_all_batts();
	}
      else
	hf_acpi_poll_all_batts();
    }
  else if (! strcmp(subsystem, "Lid") || ! strcmp(subsystem, "Button"))
    {
      HalDevice *button;
      const char *btype = NULL;

      if (! strcmp(subsystem, "Lid"))
        btype = "lid";
      else if (data && ! strcmp(data, "notify=0x00"))
        btype = "power";
      else if (data && ! strcmp(data, "notify=0x01"))
        btype = "sleep";

      if (btype)
        {
          button = hal_device_store_match_key_value_string(hald_get_gdl(),
                                                           "button.type",
                                                           btype);

          if (button)
            {
              if (! strcmp(btype, "lid"))
                {
                  gboolean isclosed;

		  isclosed = (data && ! strcmp(data, "notify=0x00")) ?
                              TRUE : FALSE;
		  device_property_atomic_update_begin();
		  hf_acpi_button_update_state(button, isclosed);
		  device_property_atomic_update_end();
		}
              device_send_signal_condition(button, "ButtonPressed", btype);
	    }
	}
    }

  return TRUE;
}

HFHandler hf_acpi_handler = {
  .init =		hf_acpi_init,
  .probe =		hf_acpi_probe,
  .device_rescan =	hf_acpi_device_rescan
};

HFDevdHandler hf_acpi_devd_handler = {
  .notify = hf_acpi_devd_notify
};
