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

static boolean is_locked_by_hal = FALSE;
static boolean check_lock_state = TRUE;
static boolean polling_disabled = FALSE;

static struct
{
  const struct timespec	update_interval;
  char			*device_file;
  char			*parent;
  boolean		is_cdrom;
  boolean		is_scsi_removable;
  boolean		had_media;
  struct timespec	next_update;
} addon = { { 2, 0 } };

static void update_proc_title (const char *device);
static void unmount_volumes (void);

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
hf_addon_storage_scsi_read_capacity (HFPCDROM *scsi_device)
{
  unsigned char buf[8];
  static char ccb[16] = { /*HFP_CDROM_READ_CAPACITY*/ 0x25 };

  assert(scsi_device != NULL);

  /* We only check for success or error and discard the data. */
  return hfp_cdrom_send_ccb(scsi_device, ccb, 10, HFP_CDROM_DIRECTION_IN, buf, sizeof(buf), NULL);
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
  else if (addon.is_scsi_removable)
    {
      /* (ab)use cdrom-specific routines:
       * for what we are doing here there is no difference between
       * a SCSI CD-ROM and any other disk-like SCSI device
       * with removable media.
       * This is a gentler check than trying to open the device.
       */
      HFPCDROM *scsi_device;

      /* XXX hfp_cdrom_new_from_fd(-1) below is an ugly hack to prevent
       * regular open() in case cam_open_device() fails.
       */
      scsi_device = hfp_cdrom_new_from_fd(-1, addon.device_file, addon.parent);
      if (scsi_device)
        {
          /* some umass devices may lie in TEST UNIT READY
	   * so do READ CAPACITY to be sure.
	   */
          if (hfp_cdrom_test_unit_ready(scsi_device) && hf_addon_storage_scsi_read_capacity(scsi_device))
            has_media = TRUE;

	  hfp_cdrom_free(scsi_device);
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

  return has_media;
}

static void
unmount_volumes (void)
{
  int num_volumes;
  char **volumes;

  if ((volumes = libhal_manager_find_device_string_match(hfp_ctx,
                                                         "block.storage_device",
							 hfp_udi,
							 &num_volumes,
							 &hfp_error)) != NULL)
    {
      int i;

      dbus_error_free(&hfp_error);

      for (i = 0; i < num_volumes; i++)
        {
          char *vol_udi;

	  vol_udi = volumes[i];

	  if (libhal_device_get_property_bool(hfp_ctx, vol_udi, "volume.is_mounted", &hfp_error))
            {
              DBusMessage *msg = NULL;
	      DBusMessage *reply = NULL;
	      DBusConnection *dbus_connection;
	      unsigned int num_options = 0;
	      char **options = NULL;
	      char *devfile;

	      dbus_error_free(&hfp_error);
              hfp_info("Forcing unmount of volume '%s'", vol_udi);

	      dbus_connection = libhal_ctx_get_dbus_connection(hfp_ctx);
	      msg = dbus_message_new_method_call("org.freedesktop.Hal", vol_udi,
                                                 "org.freedesktop.Hal.Device.Volume",
						 "Unmount");
	      if (msg == NULL)
                {
                  hfp_warning("Could not create dbus message for %s", vol_udi);
		  continue;
		}

	      options = calloc(1, sizeof (char *));
	      if (options == NULL)
                {
                  hfp_warning("Could not allocation memory for options");
		  dbus_message_unref(msg);
		  continue;
		}

	      options[0] = "force";
	      num_options = 1;

	      devfile = libhal_device_get_property_string(hfp_ctx, vol_udi, "block.device", NULL);
	      if (devfile != NULL)
                {
                  hfp_info("Forcibly attempting to unmount %s as media was removed", devfile);
		  libhal_free_string(devfile);
		}

	      if (! dbus_message_append_args(msg, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &options, num_options, DBUS_TYPE_INVALID))
                 {
                   hfp_warning("Could not append args to dbus message for %s", vol_udi);
		   free(options);
		   dbus_message_unref(msg);
		   continue;
		 }

	      if (! (reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg, -1, &hfp_error)))
                {
                  hfp_warning("Unmount failed for %s: %s: %s", vol_udi, hfp_error.name, hfp_error.message);
		  dbus_error_free(&hfp_error);
		  free(options);
		  dbus_message_unref(msg);
		  continue;
		}

	      if (dbus_error_is_set(&hfp_error))
                {
                  hfp_warning("Unmount failed for %s: %s : %s", vol_udi, hfp_error.name, hfp_error.message);
		  dbus_error_free(&hfp_error);
		  free(options);
		  dbus_message_unref(msg);
		  dbus_message_unref(reply);
		  continue;
		}

	      hfp_info("Successfully unmounted udi '%s'", vol_udi);
	      free(options);
              dbus_message_unref(msg);
              dbus_message_unref(reply);
	    }
	}
      libhal_free_string_array(volumes);
    }
}

static boolean
poll_for_media (boolean check_only, boolean force)
{
  boolean has_media;

  if (check_lock_state)
    {
      boolean should_poll;

      check_lock_state = FALSE;

      hfp_info("Checking whether device %s is locked by HAL", addon.device_file);
      if (libhal_device_is_locked_by_others(hfp_ctx, hfp_udi, "org.freedesktop.Hal.Device.Storage", &hfp_error))
        {
          hfp_info("... device %s is locked by HAL", addon.device_file);
	  dbus_error_free(&hfp_error);
	  is_locked_by_hal = TRUE;
	  update_proc_title(addon.device_file);
	  goto skip_check;
	}
      else
        {
          hfp_info("... device %s is not locked by HAL", addon.device_file);
	  is_locked_by_hal = FALSE;
	}
      dbus_error_free(&hfp_error);

      should_poll = libhal_device_get_property_bool(hfp_ctx, hfp_udi, "storage.media_check_enabled", &hfp_error);
      dbus_error_free(&hfp_error);
      polling_disabled = ! should_poll;
      update_proc_title(addon.device_file);
    }

  if (! force && polling_disabled)
    goto skip_check;

  has_media = hf_addon_storage_update();
  if (check_only)
    return has_media;

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
#if __FreeBSD_version >= 800066
      /*
       * With newusb, it is safe to force unmount volumes.  This may be
       * safe on newer versions of the old USB stack, but we'll be
       * extra cautious.
       */
      unmount_volumes();
#endif

      libhal_device_rescan(hfp_ctx, hfp_udi, &hfp_error);
      dbus_error_free(&hfp_error);
      addon.had_media = has_media;

      return TRUE;
    }

skip_check:

  return FALSE;
}

static void
update_proc_title (const char *device)
{
  if (polling_disabled)
    setproctitle("no polling on %s because it is explicitly disabled", device);
  else if (is_locked_by_hal)
    setproctitle("no polling on %s because it is locked by HAL", device);
  else
    setproctitle("%s", device);
}

static DBusHandlerResult
dbus_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
  check_lock_state = TRUE;

  return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
direct_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
  if (dbus_message_is_method_call(message,
			  	  "org.freedesktop.Hal.Device.Storage.Removable",
				  "CheckForMedia"))
    {
      DBusMessage *reply;
      dbus_bool_t had_effect;

      hfp_info("Forcing poll for media becusse CheckForMedia() was called");

      had_effect = poll_for_media(FALSE, TRUE);

      reply = dbus_message_new_method_return (message);
      dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &had_effect, DBUS_TYPE_INVALID);
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
    }

  return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char **argv)
{
  char *drive_type;
  char *removable;
  char *bus;
  char *driver;
  char *filter_str;
  DBusConnection *connection;
  DBusConnection *syscon;

  if (! hfp_init(argc, argv))
    goto end;

  addon.device_file = getenv("HAL_PROP_BLOCK_DEVICE");
  if (! addon.device_file)
    goto end;

  drive_type = getenv("HAL_PROP_STORAGE_DRIVE_TYPE");
  if (! drive_type)
    goto end;

  removable = getenv("HAL_PROP_STORAGE_REMOVABLE");
  if (! removable)
    goto end;

  bus = getenv("HAL_PROP_STORAGE_BUS");
  if (! bus)
    goto end;

  driver = getenv("HAL_PROP_FREEBSD_DRIVER");
  if (! driver)
    goto end;

  addon.parent = getenv("HAL_PROP_INFO_PARENT");
  if (! addon.parent)
    goto end;

  addon.is_cdrom = ! strcmp(drive_type, "cdrom");
  addon.is_scsi_removable = (! strcmp(bus, "scsi") ||
    (! strcmp(bus, "usb") && (! strcmp(driver, "da") || ! strcmp(driver, "sa") ||
    ! strcmp(driver, "cd")))) && ! strcmp(removable, "true");
  addon.had_media = poll_for_media(TRUE, FALSE);

  if (! libhal_device_addon_is_ready(hfp_ctx, hfp_udi, &hfp_error))
    goto end;
  dbus_error_free(&hfp_error);

  syscon = dbus_bus_get(DBUS_BUS_SYSTEM, &hfp_error);
  dbus_error_free(&hfp_error);
  assert(syscon != NULL);
  dbus_connection_set_exit_on_disconnect(syscon, 0);

  dbus_bus_add_match(syscon,
		     "type='signal'"
		     ",interface='org.freedesktop.Hal.Manager'"
		     ",sender='org.freedesktop.Hal'",
		     NULL);
  dbus_bus_add_match(syscon,
		     "type='signal'"
		     ",interface='org.freedesktop.Hal.Manager'"
		     ",sender='org.freedesktop.Hal'",
		     NULL);
  filter_str = hfp_strdup_printf("type='signal'"
		  	       ",interface='org.freedesktop.Hal.Device'"
			       ",sender='org.freedesktop.Hal'"
			       ",path='%s'",
			       hfp_udi);
  dbus_bus_add_match(syscon, filter_str, NULL);
  hfp_free(filter_str);

  dbus_connection_add_filter(syscon, dbus_filter_function, NULL, NULL);

  connection = libhal_ctx_get_dbus_connection(hfp_ctx);
  assert(connection != NULL);
  dbus_connection_set_exit_on_disconnect(connection, 0);
  dbus_connection_add_filter(connection, direct_filter_function, NULL, NULL);

  if (! libhal_device_claim_interface(hfp_ctx,
			 	      hfp_udi,
				      "org.freedesktop.Hal.Device.Storage.Removable",
				      "    <method name=\"CheckForMedia\">\n"
				      "      <arg name=\"call_had_sideeffect\" direction=\"out\" type=\"b\"/>\n"
				      "    </method>\n",
				      &hfp_error))
    {
      hfp_critical("Cannot claim interface 'org.freedesktop.Hal.Device.Storage.Removable'");
      goto end;
    }
  dbus_error_free(&hfp_error);

  while (TRUE)
    {
      /* process dbus traffic until update interval has elapsed */
      while (TRUE)
	{
	  struct timespec now;

	  hfp_clock_gettime(&now);
	  if (hfp_timespeccmp(&now, &addon.next_update, <))
	    {
	      struct timespec timeout;

	      timeout = addon.next_update;
	      hfp_timespecsub(&timeout, &now);

	      if (timeout.tv_sec < 0) /* current time went backwards */
		timeout = addon.update_interval;

	      dbus_connection_read_write_dispatch(connection, (int) ((timeout.tv_sec * 1000 + timeout.tv_nsec / 1000000) / 2));
	      dbus_connection_read_write_dispatch(syscon, (int) ((timeout.tv_sec * 1000 + timeout.tv_nsec / 1000000) / 2));
	      if (! dbus_connection_get_is_connected(connection) ||
		  ! dbus_connection_get_is_connected(syscon))
		goto end;
	    }
	  else
	    break;
	}

      poll_for_media(FALSE, FALSE);
      hfp_clock_gettime(&addon.next_update);
      hfp_timespecadd(&addon.next_update, &addon.update_interval);
    }

 end:
  return 0;
}
