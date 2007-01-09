/***************************************************************************
 * CVSID: $Id$
 *
 * addon-storage.c : poll storage devices for media changes
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

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "libhal/libhal.h"

#include "../libprobe/hfp.h"
#include "../libprobe/hfp-cdrom.h"

static struct
{
  const struct timeval	update_interval;
  char			*device_file;
  char			*parent;
  boolean		is_cdrom;
  boolean		had_media;
  struct timeval	next_update;
} addon = { { 2, 0 } };

/* see MMC-3 Working Draft Revision 10 */
static boolean
hf_addon_storage_cdrom_eject_pressed (HFPCDROM *cdrom)
{
  unsigned char buf[8];
  static char ccb[16] = {
    HFP_CDROM_GET_EVENT_STATUS_NOTIFICATION,
    1 << 0,			/* immediate */
    0,
    0,
    1 << 4,			/* notification class = media */
    0,
    0,
    0,
    HFP_N_ELEMENTS(buf)		/* allocation length LSB */
  };

  assert(cdrom != NULL);

  return hfp_cdrom_send_ccb(cdrom, ccb, 10, HFP_CDROM_DIRECTION_IN, buf, sizeof(buf), NULL)
    && buf[1] >= 6		/* event data length LSB */
    && buf[4] == 0x1;		/* media event = eject pressed */
}

static boolean
hf_addon_storage_update (void)
{
  boolean has_media = FALSE;

  if (addon.is_cdrom)
    {
      HFPCDROM *cdrom;

      cdrom = hfp_cdrom_new(addon.device_file, addon.parent);
      if (cdrom)
	{
	  if (hfp_cdrom_test_unit_ready(cdrom))
	    has_media = TRUE;

	  if (hf_addon_storage_cdrom_eject_pressed(cdrom))
	    {
	      libhal_device_emit_condition(hfp_ctx, hfp_udi, "EjectPressed", "", &hfp_error);
	      dbus_error_free(&hfp_error);
	    }

	  hfp_cdrom_free(cdrom);
	}
    }
  else
    {
      int fd;

      fd = open(addon.device_file, O_RDONLY);
      if (fd >= 0)		/* can open = has media */
	{
	  has_media = TRUE;
	  close(fd);
	}
    }

  hfp_gettimeofday(&addon.next_update);
  hfp_timevaladd(&addon.next_update, &addon.update_interval);

  return has_media;
}

int
main (int argc, char **argv)
{
  char *drive_type;
  DBusConnection *connection;

  if (! hfp_init(argc, argv))
    goto end;

  addon.device_file = getenv("HAL_PROP_BLOCK_DEVICE");
  if (! addon.device_file)
    goto end;

  drive_type = getenv("HAL_PROP_STORAGE_DRIVE_TYPE");
  if (! drive_type)
    goto end;

  addon.parent = getenv("HAL_PROP_INFO_PARENT");
  if (! addon.parent)
    goto end;

  /* give a meaningful process title for ps(1) */
  setproctitle("%s", addon.device_file);

  addon.is_cdrom = ! strcmp(drive_type, "cdrom");
  addon.had_media = hf_addon_storage_update();

  connection = libhal_ctx_get_dbus_connection(hfp_ctx);
  assert(connection != NULL);

  while (TRUE)
    {
      boolean has_media;

      /* process dbus traffic until update interval has elapsed */
      while (TRUE)
	{
	  struct timeval now;

	  hfp_gettimeofday(&now);
	  if (hfp_timevalcmp(&now, &addon.next_update, <))
	    {
	      struct timeval timeout;

	      timeout = addon.next_update;
	      hfp_timevalsub(&timeout, &now);

	      if (timeout.tv_sec < 0) /* current time went backwards */
		timeout = addon.update_interval;

	      dbus_connection_read_write(connection, timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
	      if (! dbus_connection_get_is_connected(connection))
		goto end;
	    }
	  else
	    break;
	}

      has_media = hf_addon_storage_update();
      if (has_media != addon.had_media)
	{
	  /*
	   * FIXME: if the media was removed, we should force-unmount
	   * all its child volumes (see linux2/addons/addon-storage.c).
	   * However, currently (FreeBSD 6.0) umount -f is broken and
	   * can cause kernel panics. When I tried to umount -f a
	   * flash card after removing it, it failed with EAGAIN. It
	   * continued to fail after I inserted the card. The system
	   * then hung while rebooting and did not unmount my other
	   * filesystems.
	   */

	  libhal_device_rescan(hfp_ctx, hfp_udi, &hfp_error);
	  dbus_error_free(&hfp_error);
	  addon.had_media = has_media;
	}
    }

 end:
  return 0;
}
