/***************************************************************************
 * CVSID: $Id$
 *
 * probe-hiddev.c : USB HID prober
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

#include <sys/param.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#ifndef HAVE_LIBUSB20
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>
#else
#if __FreeBSD_version >= 800064
#include <dev/usb/usbhid.h>
#else
#include <dev/usb2/include/usb2_hid.h>
#endif
#endif
#include <usbhid.h>

#include "../libprobe/hfp.h"

#define HID_COLLECTION_APPLICATION	1

int
main (int argc, char **argv)
{
  char *device_file;
  int fd = -1;
  int report_id;
  report_desc_t report_desc;
  struct hid_data *data;
  hid_item_t item;
  boolean is_keyboard = FALSE;
  boolean is_keypad = FALSE;
  boolean is_mouse = FALSE;
  boolean is_joystick = FALSE;

  if (! hfp_init(argc, argv))
    goto end;

  device_file = getenv("HAL_PROP_HIDDEV_DEVICE");
  if (! device_file)
    goto end;

  fd = open(device_file, O_RDONLY);
  if (fd < 0)
    goto end;

  /* give a meaningful process title for ps(1) */
  setproctitle("%s", device_file);

#ifdef HAVE_LIBUSB20
  report_id = hid_get_report_id(fd);
  if (report_id == -1)
#else
  if (ioctl(fd, USB_GET_REPORT_ID, &report_id) < 0)
#endif
    goto end;

  hid_init(NULL);

  report_desc = hid_get_report_desc(fd);
  if (! report_desc)
    goto end;

  for (data = hid_start_parse(report_desc, ~0, report_id); hid_get_item(data, &item);)
    if (item.kind == hid_collection)
      {
	if (item.collection == HID_COLLECTION_APPLICATION)
	  {
	    const char *page;
	    char *full_page;
	    int i;

	    page = hid_usage_page(HID_PAGE(item.usage));

	    full_page = hfp_strdup_printf("%s Page", page);
	    for (i = 0; full_page[i] != 0; i++)
	      if (full_page[i] == '_')
		full_page[i] = ' ';

	    libhal_device_property_strlist_append(hfp_ctx, hfp_udi, "hiddev.application_pages", full_page, &hfp_error);
	    hfp_free(full_page);
	  }

	switch (item.usage)
	  {
	  case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_MOUSE):
	    is_mouse = TRUE;
	    break;

	  case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_JOYSTICK):
	  case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_GAME_PAD):
	    is_joystick = TRUE;
	    break;

	  case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_KEYBOARD):
	    is_keyboard = TRUE;
	    break;

	  case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_KEYPAD):
	    is_keypad = TRUE;
	    break;
	  }
      }
  hid_end_parse(data);

  hid_dispose_report_desc(report_desc);

  if (is_keyboard || is_mouse || is_joystick || is_keypad)
    {
      libhal_device_add_capability(hfp_ctx, hfp_udi, "input", &hfp_error);
      libhal_device_set_property_string(hfp_ctx, hfp_udi, "info.category", "input", &hfp_error);
      libhal_device_set_property_string(hfp_ctx, hfp_udi, "input.device", device_file, &hfp_error);
    }
  if (is_keyboard)
      libhal_device_add_capability(hfp_ctx, hfp_udi, "input.keyboard", &hfp_error);
  if (is_keypad)
      libhal_device_add_capability(hfp_ctx, hfp_udi, "input.keypad", &hfp_error);
  if (is_keyboard || is_keypad)
      libhal_device_add_capability(hfp_ctx, hfp_udi, "input.keys", &hfp_error);
  if (is_mouse)
      libhal_device_add_capability(hfp_ctx, hfp_udi, "input.mouse", &hfp_error);
  if (is_joystick)
      libhal_device_add_capability(hfp_ctx, hfp_udi, "input.joystick", &hfp_error);

 end:
  return 0;
}
