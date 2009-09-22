/***************************************************************************
 * CVSID: $Id$
 *
 * hf-usb.c : USB support
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
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if __FreeBSD_version < 800092
#if __FreeBSD_version >= 800064
#include <legacy/dev/usb/usb.h>
#else
#include <dev/usb/usb.h>
#endif
#endif

#include "../logger.h"
#include "../osspec.h"

#include "hf-usb.h"
#include "hf-devtree.h"
#include "hf-util.h"

#if __FreeBSD_version < 800092
#define HF_USB_DEVICE			"/dev/usb"
#if __FreeBSD_version < 800066
#define HF_USB2_DEVICE			"/dev/usb "
#else
#define HF_USB2_DEVICE			"/dev/usbctl"
#endif

typedef struct
{
  int		fd;
  int		index;
} Controller;

static GSList *controllers = NULL;

static int hf_usb_fd;

static gboolean hf_usb_probe_device (HalDevice *parent,
				     Controller *controller,
				     const struct usb_device_info *device_info);

static Controller *
hf_usb_find_controller (int index)
{
  GSList *l;

  HF_LIST_FOREACH(l, controllers)
    {
      Controller *controller = l->data;

      if (controller->index == index)
	return controller;
    }

  return NULL;
}

static HalDevice *
hf_usb_find_hub (const struct usb_device_info *device_info)
{
  GSList *a;

  g_return_val_if_fail(device_info != NULL, FALSE);

  HF_LIST_FOREACH(a, hald_get_gdl()->devices)
    {
      HalDevice *device = a->data;

      if (hal_device_property_get_int(device, "usb_device.bus_number") == device_info->udi_bus)
	{
          HalDeviceStrListIter iter;

	  for (hal_device_property_strlist_iter_init(device, "usb_device.freebsd.ports", &iter);
               hal_device_property_strlist_iter_is_valid(&iter);
	       hal_device_property_strlist_iter_next(&iter))
	    {
	      const char *port;

	      port = hal_device_property_strlist_iter_get_value(&iter);

	      if (atoi(port) == device_info->udi_addr)
		return device;
	    }
	}
    }

  return NULL;
}

static gboolean
hf_usb_get_descriptor (int fd,
		       int addr,
		       int type,
		       int index,
		       gpointer desc,
		       int len,
		       GError **err)
{
  struct usb_ctl_request req;
  char buf[len];

  g_return_val_if_fail(desc != NULL, FALSE);

  memset(&req, 0, sizeof(req));

  req.ucr_addr = addr;
  req.ucr_request.bmRequestType = UT_READ_DEVICE;
  req.ucr_request.bRequest = UR_GET_DESCRIPTOR;
  USETW2(req.ucr_request.wValue, type, index);
  USETW(req.ucr_request.wIndex, 0);
  USETW(req.ucr_request.wLength, len);
  req.ucr_data = buf;

  if (ioctl(fd, USB_REQUEST, &req) < 0)
    {
      g_set_error(err, 0, 0, "%s", g_strerror(errno));
      return FALSE;
    }
  if (req.ucr_actlen != len)
    {
      g_set_error(err, 0, 0, "bad length");
      return FALSE;
    }

  memcpy(desc, buf, len);
  return TRUE;
}

static char *
hf_usb_get_string_descriptor (int fd,
			      int addr,
			      int index,
			      GError **err)
{
  usb_string_descriptor_t string_desc;
  gunichar2 unicode_str[G_N_ELEMENTS(string_desc.bString)];
  int len;			/* in words */
  int i;
  char *str;
  GError *tmp_err = NULL;

  /* 1. get bLength */

  if (! hf_usb_get_descriptor(fd, addr, UDESC_STRING, index, &string_desc, 2, err))
    return NULL;

  if (string_desc.bLength < 2)
    {
      g_set_error(err, 0, 0, "bad length");
      return NULL;
    }

  /* 2. get the whole descriptor */

  if (! hf_usb_get_descriptor(fd, addr, UDESC_STRING, index, &string_desc, string_desc.bLength, err))
    return NULL;

  len = (string_desc.bLength - 2) / sizeof(uWord);
  for (i = 0; i < len; i++)
    unicode_str[i] = UGETW(string_desc.bString[i]);

  str = g_utf16_to_utf8(unicode_str, len, NULL, NULL, &tmp_err);
  if (! str)
    {
      g_set_error(err, 0, 0, "unable to convert string to UTF-8: %s", tmp_err->message);
      g_error_free(tmp_err);
    }

  return str;
}

static gpointer
hf_usb_get_full_config_descriptor (int fd,
				   int addr,
				   int index,
				   int *len,
				   GError **err)
{
  usb_config_descriptor_t config_desc;
  int _len;
  char *buf;

  /*
   * The USB configuration descriptor is padded with interface
   * descriptors and endpoint descriptors. See the USB specifications
   * for details.
   */

  /* 1. get the config descriptor alone, to obtain wTotalLength */

  if (! hf_usb_get_descriptor(fd, addr, UDESC_CONFIG, index, &config_desc, USB_CONFIG_DESCRIPTOR_SIZE, err))
    return NULL;

  /* 2. get the config descriptor and the padded if/endpoint descriptors */

  _len = UGETW(config_desc.wTotalLength);
  buf = g_new(char, _len);

  if (hf_usb_get_descriptor(fd, addr, UDESC_CONFIG, index, buf, _len, err))
    {
      if (len)
	*len = _len;

      return buf;
    }

  /* failure */

  g_free(buf);
  return NULL;
}

/* adapted from usbif_set_name() in linux2/physdev.c */
static const char *
hf_usb_get_interface_name (const usb_interface_descriptor_t *desc)
{
  switch (desc->bInterfaceClass)
    {
    default:
    case 0x00: return "USB Interface";
    case 0x01: return "USB Audio Interface";
    case 0x02: return "USB Communications Interface";
    case 0x03: return "USB HID Interface";
    case 0x06: return "USB Imaging Interface";
    case 0x07: return "USB Printer Interface";
    case 0x08: return "USB Mass Storage Interface";
    case 0x09: return "USB Hub Interface";
    case 0x0a: return "USB Data Interface";
    case 0x0b: return "USB Chip/Smartcard Interface";
    case 0x0d: return "USB Content Security Interface";
    case 0x0e: return "USB Video Interface";
    case 0xdc: return "USB Diagnostic Interface";
    case 0xe0: return "USB Wireless Interface";
    case 0xef: return "USB Miscelleneous Interface";
    case 0xfe: return "USB Application Specific Interface";
    case 0xff: return "USB Vendor Specific Interface";
    }
}

static const char *
hf_usb_get_devname (const struct usb_device_info *di, const char *driver)
{
  int i;

  g_return_val_if_fail(di != NULL, NULL);
  g_return_val_if_fail(driver != NULL, NULL);

  for (i = 0; i < USB_MAX_DEVNAMES; i++)
    if (hf_devtree_is_driver(di->udi_devnames[i], driver))
      return di->udi_devnames[i];

  return NULL;
}

static HalDevice *
hf_usb_device_new (HalDevice *parent,
		   Controller *controller,
		   const struct usb_device_info *di,
		   const usb_device_descriptor_t *device_desc)
{
  HalDevice *device;
  int speed;
  usb_config_descriptor_t config_desc;
  gboolean can_wake_up = FALSE;
  int num_interfaces = 0;
  int i;
  const char *devname;

  g_return_val_if_fail(controller != NULL, NULL);
  g_return_val_if_fail(di != NULL, NULL);
  g_return_val_if_fail(device_desc != NULL, NULL);

  device = hf_device_new(parent);

  hal_device_property_set_string(device, "info.subsystem", "usb_device");
  hal_device_property_set_string(device, "info.product", di->udi_product);
  hal_device_property_set_string(device, "info.vendor", di->udi_vendor);

  hal_device_property_set_int(device, "usb_device.bus_number", di->udi_bus);
  hal_device_property_set_int(device, "usb_device.configuration_value", di->udi_config);
  hal_device_property_set_int(device, "usb_device.num_configurations", device_desc->bNumConfigurations);
  hal_device_property_set_int(device, "usb_device.device_class", di->udi_class);
  hal_device_property_set_int(device, "usb_device.device_subclass", di->udi_subclass);
  hal_device_property_set_int(device, "usb_device.device_protocol", di->udi_protocol);
  hal_device_property_set_bool(device, "usb_device.is_self_powered", di->udi_power == 0);
  hal_device_property_set_int(device, "usb_device.max_power", di->udi_power);
  hal_device_property_set_int(device, "usb_device.num_ports", di->udi_nports);
  hal_device_property_set_int(device, "usb_device.port_number", di->udi_addr);

  switch (di->udi_speed)
    {
    case USB_SPEED_LOW:		speed = 0x00150; break;
    case USB_SPEED_FULL:	speed = 0x01200; break;
    case USB_SPEED_HIGH:	speed = 0x48000; break;
    default:			speed = 0; break;
    }
  hal_device_property_set_int(device, "usb_device.speed_bcd", speed);

  hal_device_property_set_int(device, "usb_device.version_bcd", UGETW(device_desc->bcdUSB));
  /* FIXME usb_device.level_number */
  hal_device_property_set_int(device, "usb_device.product_id", di->udi_productNo);
  hal_device_property_set_int(device, "usb_device.vendor_id", di->udi_vendorNo);
  hal_device_property_set_int(device, "usb_device.device_revision_bcd", UGETW(device_desc->bcdDevice));

  if (device_desc->iSerialNumber != 0)
    {
      char *serial;

      serial = hf_usb_get_string_descriptor(controller->fd, di->udi_addr, device_desc->iSerialNumber, NULL);
      if (serial)
	{
	  hal_device_property_set_string(device, "usb_device.serial", serial);
	  g_free(serial);
	}
    }

  hal_device_property_set_string(device, "usb_device.product", di->udi_product);
  hal_device_property_set_string(device, "usb_device.vendor", di->udi_vendor);

  if (di->udi_config != 0 && hf_usb_get_descriptor(controller->fd,
						   di->udi_addr,
						   UDESC_CONFIG,
						   di->udi_config - 1,
						   &config_desc,
						   USB_CONFIG_DESCRIPTOR_SIZE,
						   NULL))
    {
      can_wake_up = (config_desc.bmAttributes & UC_REMOTE_WAKEUP) != 0;
      num_interfaces = config_desc.bNumInterface;

      if (config_desc.iConfiguration != 0)
	{
	  char *configuration;

	  configuration = hf_usb_get_string_descriptor(controller->fd, di->udi_addr, config_desc.iConfiguration, NULL);
	  if (configuration)
	    {
	      hal_device_property_set_string(device, "usb_device.configuration", configuration);
	      g_free(configuration);
	    }
	}
    }

  hal_device_property_set_bool(device, "usb_device.can_wake_up", can_wake_up);
  hal_device_property_set_int(device, "usb_device.num_interfaces", num_interfaces);

  for (i = 0; i < di->udi_nports; i++)
    if (di->udi_ports[i] > 0 && di->udi_ports[i] < USB_MAX_DEVICES)
      {
	char *port;

	port = g_strdup_printf("%i", di->udi_ports[i]);
	hal_device_property_strlist_append(device, "usb_device.freebsd.ports", port, FALSE);
	g_free(port);
      }

  /*
   * Register the first attached driver (if any) with devtree (mostly
   * useful for allowing hf-scsi to find umass devices).
   */
  if (*di->udi_devnames[0])
    hf_devtree_device_set_name(device, di->udi_devnames[0]);

  if ((devname = hf_usb_get_devname(di, "ukbd")))	/* USB keyboard */
    hf_device_set_input(device, "keyboard", "keys", devname);
  else if ((devname = hf_usb_get_devname(di, "ums")))	/* USB mouse */
    hf_device_set_input(device, "mouse", NULL, devname);
  else if ((devname = hf_usb_get_devname(di, "uhid")))	/* UHID device */
    {
      hal_device_property_set_string(device, "info.category", "hiddev");
      hal_device_add_capability(device, "hiddev");
      hf_device_property_set_string_printf(device, "hiddev.device", "/dev/%s", devname);
      hal_device_copy_property(device, "info.product", device, "hiddev.product");
    }
  else if ((devname = hf_usb_get_devname(di, "ldev")))	/* Linux driver (webcam) */
    {
      /*
       * XXX This is a hack.  Currently, all ldev devices are webcams.  However,
       * that may not always be the case.  Hopefully, when other Linux driver
       * support is added, there will be a sysctl or some other way to
       * determine device class.
       */
      hf_usb_add_webcam_properties(device);
    }
  else if ((devname = hf_usb_get_devname(di, "pwc")))	/* Phillips Web Cam */
    {
      hf_usb_add_webcam_properties(device);
    }

  hf_usb_device_compute_udi(device);

  return device;
}

static HalDevice *
hf_usb_interface_device_new (HalDevice *parent,
			     Controller *controller,
			     const struct usb_device_info *di,
			     const usb_interface_descriptor_t *desc)
{
  HalDevice *device;
  const char *name;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);
  g_return_val_if_fail(desc != NULL, NULL);

  device = hf_device_new(parent);

  hal_device_property_set_string(device, "info.subsystem", "usb");

  hal_device_merge_with_rewrite(device, parent, "usb.", "usb_device.");

  name = hf_usb_get_interface_name(desc);
  hal_device_property_set_string(device, "info.product", name);
  hal_device_property_set_string(device, "usb.product", name);

  hal_device_property_set_int(device, "usb.interface.class", desc->bInterfaceClass);
  hal_device_property_set_int(device, "usb.interface.subclass", desc->bInterfaceSubClass);
  hal_device_property_set_int(device, "usb.interface.protocol", desc->bInterfaceProtocol);
  hal_device_property_set_int(device, "usb.interface.number", desc->bInterfaceNumber);

  if (desc->iInterface != 0)
    {
      char *interface;

      interface = hf_usb_get_string_descriptor(controller->fd, di->udi_addr, desc->iInterface, NULL);
      if (interface)
	{
	  hal_device_property_set_string(device, "usb.interface.description", interface);
	  g_free(interface);
	}
    }


  hf_usb_device_compute_udi(device);

  return device;
}

static void
hf_usb_probe_address (HalDevice *parent,
		      Controller *controller,
		      int addr)
{
  struct usb_device_info device_info;

  g_return_if_fail(controller != NULL);

  memset(&device_info, 0, sizeof(device_info));
  device_info.udi_addr = addr;

  if (ioctl(controller->fd, USB_DEVICEINFO, &device_info) != -1)
    {
      HalDevice *real_parent;

      /*
       * If we're called from hf_usb_probe_device() the passed parent
       * will be our actual one. However, if we're called from
       * hf_usb_probe() during a reprobe, the device may be located
       * down the bus while the passed parent is the root USB
       * controller. See if we can find our real parent (our hub).
       */
      real_parent = hf_usb_find_hub(&device_info);
      if (real_parent)
	{
	  parent = real_parent;
	  if (hal_device_property_get_bool(parent, "info.ignore"))
	    return;
	}

      hf_usb_probe_device(parent, controller, &device_info);
    }
}

static gboolean
hf_usb_probe_device (HalDevice *parent,
		     Controller *controller,
		     const struct usb_device_info *device_info)
{
  usb_device_descriptor_t device_desc;
  GError *err = NULL;
  HalDevice *device;
  int i;

  g_return_val_if_fail(controller != NULL, FALSE);
  g_return_val_if_fail(device_info != NULL, FALSE);

  device = hf_device_store_match(hald_get_gdl(),
                                 "usb_device.bus_number", HAL_PROPERTY_TYPE_INT32, device_info->udi_bus,
				 "usb_device.port_number", HAL_PROPERTY_TYPE_INT32, device_info->udi_addr,
				 NULL);

  if (device)
    return FALSE;		/* device already exists */

  if (! hf_usb_get_descriptor(controller->fd,
			      device_info->udi_addr,
			      UDESC_DEVICE,
			      0,
			      &device_desc,
			      USB_DEVICE_DESCRIPTOR_SIZE,
			      &err))
    {
      HAL_WARNING(("unable to get device descriptor of USB device %i.%i: %s", controller->index, device_info->udi_addr, err->message));
      g_error_free(err);
      return FALSE;
    }

  /* add device */

  device = hf_usb_device_new(parent, controller, device_info, &device_desc);
  if (hf_device_preprobe(device))
    {
      if (hal_device_has_capability(device, "hiddev"))
	hf_runner_run_sync(device, 0, "hald-probe-hiddev", NULL);
      if (hal_device_has_capability(device, "input.mouse"))
        hf_runner_run_sync(device, 0, "hald-probe-mouse", NULL);

      hf_device_add(device);
    }
  else
    return FALSE;		/* device ignored */

  /* add interfaces */

  for (i = 0; i < device_desc.bNumConfigurations; i++)
    {
      usb_config_descriptor_t *config_desc;
      int len;
      char *p;
      int j;

      config_desc = hf_usb_get_full_config_descriptor(controller->fd, device_info->udi_addr, i, &len, &err);
      if (! config_desc)
	{
	  HAL_WARNING(("unable to get configuration descriptor %i of USB device %i.%i: %s", i, controller->index, device_info->udi_addr, err->message));
	  g_clear_error(&err);
	  continue;
	}

      p = (char *) config_desc + USB_CONFIG_DESCRIPTOR_SIZE;
      for (j = 0; j < config_desc->bNumInterface; j++)
	{
	  usb_interface_descriptor_t *if_desc = (usb_interface_descriptor_t *) p;
	  HalDevice *if_device;

	  if (p + USB_INTERFACE_DESCRIPTOR_SIZE > (char *) config_desc + len)
	    {
	      HAL_WARNING(("USB device %i.%i: short configuration descriptor %i", controller->index, device_info->udi_addr, i));
	      break;
	    }

	  if_device = hf_usb_interface_device_new(device, controller, device_info, if_desc);
	  hf_device_preprobe_and_add(if_device);

	  p += USB_INTERFACE_DESCRIPTOR_SIZE + if_desc->bNumEndpoints * USB_ENDPOINT_DESCRIPTOR_SIZE;
	}

      g_free(config_desc);
    }

  /* add child devices if this is a hub */

  for (i = 0; i < device_info->udi_nports; i++)
    if (device_info->udi_ports[i] > 0 && device_info->udi_ports[i] < USB_MAX_DEVICES)
      hf_usb_probe_address(device, controller, device_info->udi_ports[i]);

  return TRUE;
}

static void
hf_usb_privileged_init (void)
{
  int i;

  if (g_file_test(HF_USB2_DEVICE, G_FILE_TEST_EXISTS))
    {
      hf_usb_fd = -1;
      return;
    }

  hf_usb_fd = open(HF_USB_DEVICE, O_RDONLY);
  if (hf_usb_fd < 0)
    {
      HAL_INFO(("unable to open %s: %s", HF_USB_DEVICE, g_strerror(errno)));
      return;
    }

  for (i = 0; i < 16; i++)
    {
      char *filename;
      int fd;

      filename = g_strdup_printf("/dev/usb%i", i);
      fd = open(filename, O_RDONLY);
      g_free(filename);

      if (fd >= 0)
	{
	  Controller *controller;

	  controller = g_new0(Controller, 1);
	  controller->fd = fd;
	  controller->index = i;

	  controllers = g_slist_append(controllers, controller);
	}
    }
}

static void
hf_usb_process_event (const struct usb_event *event)
{
  g_return_if_fail(event != NULL);

  switch (event->ue_type)
    {
    case USB_EVENT_CTRLR_ATTACH:
      HAL_INFO(("received USB_EVENT_CTRLR_ATTACH event, bus %i",
		event->u.ue_ctrlr.ue_bus));
      /* FIXME */
      break;

    case USB_EVENT_CTRLR_DETACH:
      HAL_INFO(("received USB_EVENT_CTRLR_DETACH event, bus %i",
		event->u.ue_ctrlr.ue_bus));
      /* FIXME */
      break;

    case USB_EVENT_DEVICE_ATTACH:
      {
	Controller *controller;

	HAL_INFO(("received USB_EVENT_DEVICE_ATTACH event, device %i.%i",
		  event->u.ue_device.udi_bus, event->u.ue_device.udi_addr));

	controller = hf_usb_find_controller(event->u.ue_device.udi_bus);
	if (controller)
	  {
	    HalDevice *parent;

	    parent = hf_usb_find_hub(&event->u.ue_device);
	    if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
	      hf_usb_probe_device(parent, controller, &event->u.ue_device);
	  }
      }
      break;

    case USB_EVENT_DEVICE_DETACH:
      {
	HalDevice *device;

	HAL_INFO(("received USB_EVENT_DEVICE_DETACH event, device %i.%i",
		  event->u.ue_device.udi_bus, event->u.ue_device.udi_addr));

	device = hf_device_store_match(hald_get_gdl(),
                                       "usb_device.bus_number", HAL_PROPERTY_TYPE_INT32, event->u.ue_device.udi_bus,
				       "usb_device.port_number", HAL_PROPERTY_TYPE_INT32, event->u.ue_device.udi_addr,
				       NULL);

	if (device)		/* device gone, remove it and all its children */
	  hf_device_remove_tree(device);
      }
      break;

      /*
       * For some reason the kernel does not actually emit
       * USB_EVENT_DRIVER_ATTACH/USB_EVENT_DRIVER_DETACH events. We
       * handle driver events in hf_usb_devd_add() and
       * hf_usb_devd_remove(), anyway.
       */

    case USB_EVENT_DRIVER_ATTACH:
      HAL_INFO(("received USB_EVENT_DRIVER_ATTACH event"));
      break;

    case USB_EVENT_DRIVER_DETACH:
      HAL_INFO(("received USB_EVENT_DRIVER_DETACH event"));
      break;

    default:
      HAL_WARNING(("received unknown USB event %i", event->ue_type));
      break;
    }
}

static gboolean
hf_usb_event_cb (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
  struct usb_event event;

  if (hf_is_waiting)
    return TRUE;

  if (read(hf_usb_fd, &event, sizeof(event)) == sizeof(event))
    hf_usb_process_event(&event);

  return TRUE;
}

static void
hf_usb_init (void)
{
  GIOChannel *channel;

  if (hf_usb_fd < 0)
    return;

  channel = g_io_channel_unix_new(hf_usb_fd);
  g_io_add_watch(channel, G_IO_IN, hf_usb_event_cb, NULL);
}

static void
hf_usb_probe (void)
{
  GSList *l;

  /* probe all devices */

  HF_LIST_FOREACH(l, controllers)
    {
      Controller *controller = l->data;
      HalDevice *parent;

      parent = hf_devtree_find_parent_from_info(hald_get_gdl(), "usb", controller->index);
      if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
	{
	  int i;

	  for (i = 1; i < USB_MAX_DEVICES; i++)
	    hf_usb_probe_address(parent, controller, i);
	}
    }
}

static gboolean
hf_usb_devd_add (const char *name,
		 GHashTable *params,
		 GHashTable *at,
		 const char *parent)
{
  HalDevice *parent_device;
  const char *port_index;
  const char *port;
  HalDevice *device;

  /*
   * A driver was attached; if it was attached to an USB device, we
   * should reprobe the device to catch the new driver.
   */

  if (! parent)
    return FALSE;

  port_index = g_hash_table_lookup(at, "port");
  if (! port_index)
    return FALSE;		/* no port information: not an USB driver */

  /*
   * If the device already exists, it's either not an USB device or we
   * had already picked up the driver; eitherway, we have nothing to
   * do.
   */
  if (hf_devtree_find_from_name(hald_get_gdl(), name))
    return FALSE;

  /*
   * Try to find the USB device to which the driver was attached. We
   * do that by using the port key reported by devd. That key is not
   * the USB port number, but the index of the port in the parent
   * hub's port list (udi_ports).
   */

  parent_device = hf_devtree_find_from_name(hald_get_gdl(), parent);
  if (! parent_device)
    return FALSE;

  port = hal_device_property_get_strlist_elem(parent_device, "usb_device.freebsd.ports", atoi(port_index));
  if (! port)
    return FALSE;

  device = hf_device_store_match(hald_get_gdl(),
                                 "info.parent", HAL_PROPERTY_TYPE_STRING, hal_device_get_udi(parent_device),
				 "usb_device.port_number", HAL_PROPERTY_TYPE_INT32, atoi(port),
				 NULL);

  if (device)
    {
      osspec_device_reprobe(device); /* found, reprobe it to catch driver */
      return TRUE;
    }

  return FALSE;
}

static gboolean
hf_usb_devd_remove (const char *name,
		    GHashTable *params,
		    GHashTable *at,
		    const char *parent)
{
  HalDevice *device;

  /*
   * A driver was detached. If its device is an USB device, reprobe it
   * to catch the detachment.
   *
   * Note that devd also reports driver detachment when an USB device
   * is physically detached. In that case we will probably have caught
   * the device detachment first (in hf_usb_process_event()), since
   * the USB event watch was installed before the devd event watch
   * (the USB handler is listed before the devd handler in osspec). If
   * devd actually caught the event before USB, reprobing the device
   * will not hurt anyway.
   */

  device = hf_devtree_find_from_name(hald_get_gdl(), name);
  if (device && hal_device_has_property(device, "usb_device.port_number"))
    {
      osspec_device_reprobe(device);
      return TRUE;
    }

  return FALSE;
}

HFHandler hf_usb_handler = {
  .privileged_init	= hf_usb_privileged_init,
  .init			= hf_usb_init,
  .probe		= hf_usb_probe
};

HFDevdHandler hf_usb_devd_handler = {
  .add =	hf_usb_devd_add,
  .remove =	hf_usb_devd_remove
};
#endif

/*
 * Adapted from usb_compute_udi() in linux2/physdev.c and
 * usbclass_compute_udi() in linux2/classdev.c.
 */
void
hf_usb_device_compute_udi (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  if (hal_device_has_capability(device, "hiddev"))
    hf_device_set_full_udi(device, "%s_hiddev",
			   hal_device_property_get_string(device, "info.parent"));
  else if (hal_device_has_capability(device, "video4linux"))
    hf_device_set_full_udi(device, "%s_video4linux",
		    	   hal_device_property_get_string(device, "info.parent"));
  else if (hal_device_has_property(device, "usb.interface.number"))
    hf_device_set_full_udi(device, "%s_if%i",
			   hal_device_property_get_string(device, "info.parent"),
			   hal_device_property_get_int(device, "usb.interface.number"));
  else
    hf_device_set_udi(device, "usb_device_%x_%x_%s",
		      hal_device_property_get_int(device, "usb_device.vendor_id"),
		      hal_device_property_get_int(device, "usb_device.product_id"),
		      (hal_device_has_property(device, "usb_device.serial") &&
		       strcmp(hal_device_property_get_string(device, "usb_device.serial"), ""))
		      ? hal_device_property_get_string(device, "usb_device.serial")
		      : "noserial");
}

void
hf_usb_add_webcam_properties (HalDevice *device)
{
  int unit;

  g_return_if_fail(HAL_IS_DEVICE(device));

  unit = hal_device_property_get_int(device, "freebsd.unit");
  if (unit < 0)
    unit = 0;

  hal_device_property_set_string(device, "info.category", "video4linux");
  hal_device_add_capability(device, "video4linux");
  hf_device_property_set_string_printf(device, "video4linux.device", "/dev/video%i", unit);
  hal_device_property_set_string(device, "info.product", "Video Device");
}
