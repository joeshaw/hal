/***************************************************************************
 * CVSID: $Id$
 *
 * osspec.c : HAL backend for FreeBSD
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

#include "../ids.h"
#include "../logger.h"
#include "../osspec.h"

#include "hf-util.h"
#include "hf-osspec.h"
#include "hf-acpi.h"
#include "hf-ata.h"
#include "hf-computer.h"
#include "hf-devd.h"
#include "hf-devtree.h"
#include "hf-drm.h"
#include "hf-net.h"
#include "hf-pci.h"
#include "hf-scsi.h"
#include "hf-serial.h"
#include "hf-sound.h"
#include "hf-storage.h"
#include "hf-usb.h"
#ifdef HAVE_LIBUSB20
#include "hf-usb2.h"
#endif
#include "hf-volume.h"

/* the order matters: PCI devices must be created before their children, etc */
static HFHandler *handlers[] = {
  &hf_pci_handler,
  &hf_devtree_handler,
#if __FreeBSD_version < 800092
  &hf_usb_handler,
#endif
#ifdef HAVE_LIBUSB20
  &hf_usb2_handler,
#endif
  &hf_ata_handler,
  &hf_scsi_handler,
  &hf_storage_handler,
  &hf_volume_handler,
  &hf_net_handler,
  &hf_serial_handler,
  &hf_acpi_handler,
  &hf_sound_handler,
  &hf_drm_handler,
  &hf_devd_handler
};

static HalFileMonitor *file_monitor = NULL;

HalFileMonitor *
osspec_get_file_monitor (void)
{
  return file_monitor;
}

void
osspec_privileged_init (void)
{
  int i;

  file_monitor = hal_file_monitor_new ();
  if (file_monitor == NULL)
    {
      HAL_INFO(("Cannot initialize file monitor"));
    }

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->privileged_init)
      handlers[i]->privileged_init();
}

void
osspec_init (void)
{
  int i;

  pci_ids_init();

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->init)
      handlers[i]->init();
}

void
osspec_probe (void)
{
  if (hf_computer_device_add())
    {
      int i;

      for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
	if (handlers[i]->probe)
	  handlers[i]->probe();
    }

  if (hald_is_initialising)
    osspec_probe_done();
}

gboolean
osspec_device_rescan (HalDevice *d)
{
  int i;

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->device_rescan && handlers[i]->device_rescan(d))
      return TRUE;

  return FALSE;
}

gboolean
osspec_device_reprobe (HalDevice *d)
{
  int i;

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->device_reprobe && handlers[i]->device_reprobe(d))
      goto end;

  /* no match, default action */

  hf_device_remove_tree(d);
  osspec_probe();

 end:
  return FALSE;			/* this is what linux2 returns */
}

void
osspec_refresh_mount_state_for_block_device (HalDevice *d)
{
  hf_volume_update_mount(d);
}

DBusHandlerResult
osspec_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
