/***************************************************************************
 * CVSID: $Id$
 *
 * probe-storage.c : storage prober
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

#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <netinet/in.h>
#include <glib.h>
#include <libvolume_id.h>

#include "libhal/libhal.h"

#include "../libprobe/hfp.h"
#include "../libprobe/hfp-cdrom.h"

#include "freebsd_dvd_rw_utils.h"

static void
hf_probe_storage_get_cdrom_capabilities (const char *device_file,
                                         const char *parent)
{
  HFPCDROM *cdrom;
  HFPCDROMCapabilities caps;
  gboolean status = FALSE;
  int i;
  int read_speed;
  int write_speed;
  char *write_speeds;

  g_return_if_fail(device_file != NULL);
  g_return_if_fail(parent != NULL);

  cdrom = hfp_cdrom_new(device_file, parent);
  if (! cdrom)
    {
      hfp_warning("unable to open CD-ROM device %s", device_file);
      return;
    }

  /* according to sys/dev/ata/atapi-cd.c some buggy drives need this loop */
  for (i = 0; i < 5; i++)
    {
      static char ccb[16] = { HFP_CDROM_MODE_SENSE_BIG, 0, HFP_CDROM_CAP_PAGE, 0, 0, 0, 0, sizeof(caps) >> 8, sizeof(caps) };
      char *err = NULL;

      if (! hfp_cdrom_send_ccb(cdrom, ccb, 10, HFP_CDROM_DIRECTION_IN, &caps, sizeof(caps), &err))
	{
	  hfp_warning("%s: unable to get capabilities: %s", device_file, err);
	  hfp_free(err);
	  continue;
	}
      if (caps.page_code != HFP_CDROM_CAP_PAGE)
	{
	  hfp_warning("%s: bad page code %i", device_file, caps.page_code);
	  continue;
	}

      status = TRUE;
    }

  if (! status)
    goto end;

  if ((caps.media & HFP_CDROM_MST_WRITE_CDR) != 0)
    libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.cdr", TRUE, &hfp_error);
  if ((caps.media & HFP_CDROM_MST_WRITE_CDRW) != 0)
    libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.cdrw", TRUE, &hfp_error);
  if ((caps.media & HFP_CDROM_MST_READ_DVDROM) != 0)
    {
      int profile;
      gboolean r;
      gboolean rw;
      gboolean rdl;
      gboolean rwdl;
      gboolean bd;
      gboolean bdr;
      gboolean bdre;
      gboolean hddvd;
      gboolean hddvdr;
      gboolean hddvdrw;

      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvd", TRUE, &hfp_error);

      profile = get_dvd_r_rw_profile(cdrom);
      r = (profile & DRIVE_CDROM_CAPS_DVDPLUSR) != 0;
      rw = (profile & DRIVE_CDROM_CAPS_DVDRW) != 0;
      rdl = (profile & DRIVE_CDROM_CAPS_DVDPLUSRDL) != 0;
      rwdl = (profile & DRIVE_CDROM_CAPS_DVDPLUSRWDL) != 0;
      bd = (profile & DRIVE_CDROM_CAPS_BDROM) != 0;
      bdr = (profile & DRIVE_CDROM_CAPS_BDR) != 0;
      bdre = (profile & DRIVE_CDROM_CAPS_BDRE) != 0;
      hddvd = (profile & DRIVE_CDROM_CAPS_HDDVDROM) != 0;
      hddvdr = (profile & DRIVE_CDROM_CAPS_HDDVDR) != 0;
      hddvdrw = (profile & DRIVE_CDROM_CAPS_HDDVDRW) != 0;

      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvdplusr", r, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvdplusrw", rw, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvdplusrdl", rdl, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvdplusrwdl", rwdl, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.bd", bd, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.bdr", bdr, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.bdre", bdre, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.hddvd", hddvd, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.hddvdr", hddvdr, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.hddvdrw", hddvdrw, &hfp_error);
    }
  if ((caps.media & HFP_CDROM_MST_WRITE_DVDR) != 0)
    libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvdr", TRUE, &hfp_error);
  if ((caps.media & HFP_CDROM_MST_WRITE_DVDRAM) != 0)
    libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.cdrom.dvdram", TRUE, &hfp_error);

  if (get_read_write_speed(cdrom, &read_speed, &write_speed, &write_speeds) >= 0)
    {
      libhal_device_set_property_int(hfp_ctx, hfp_udi, "storage.cdrom.read_speed", read_speed, &hfp_error);
      if (write_speed > 0)
	{
	  libhal_device_set_property_int(hfp_ctx, hfp_udi, "storage.cdrom.write_speed", write_speed, &hfp_error);

	  if (write_speeds != NULL)
	    {
	      char **speedv;
	      int i;

	      speedv = g_strsplit_set(write_speeds, ",", 0);
	      free(write_speeds);

	      for (i = 0; speedv[i] != NULL; i++)
		if (*(speedv[i]))
		  libhal_device_property_strlist_append(hfp_ctx, hfp_udi, "storage.cdrom.write_speeds", speedv[i], &hfp_error);

	      g_strfreev(speedv);
	    }
	}
    }

 end:
  if (cdrom)
    hfp_cdrom_free(cdrom);
}

int
main (int argc, char **argv)
{
  char *device_file;
  char *drive_type;
  char *parent;
  int ret = 0;			/* no media/filesystem */
  gboolean has_children;
  gboolean only_check_for_media;
  gboolean is_cdrom;

  if (! hfp_init(argc, argv))
    goto end;

  device_file = getenv("HAL_PROP_BLOCK_DEVICE");
  if (! device_file)
    goto end;

  drive_type = getenv("HAL_PROP_STORAGE_DRIVE_TYPE");
  if (! drive_type)
    goto end;

  parent = getenv("HAL_PROP_INFO_PARENT");
  if (! parent)
    goto end;

  /* give a meaningful process title for ps(1) */
  setproctitle("%s", device_file);

  has_children = hfp_getenv_bool("HF_HAS_CHILDREN");
  only_check_for_media = hfp_getenv_bool("HF_ONLY_CHECK_FOR_MEDIA");

  is_cdrom = ! strcmp(drive_type, "cdrom");

  if (! only_check_for_media && is_cdrom)
    hf_probe_storage_get_cdrom_capabilities(device_file, parent);

  if (is_cdrom)
    {
      HFPCDROM *cdrom;

      cdrom = hfp_cdrom_new(device_file, parent);
      if (! cdrom)
	goto end;

      if (hfp_cdrom_test_unit_ready(cdrom))
        {
	  int fd;
	  off_t size;

          libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.removable.media_available", TRUE, &hfp_error);
	  fd = open(device_file, O_RDONLY | O_NONBLOCK);
	  if (fd > -1)
            {
              if (ioctl (fd, DIOCGMEDIASIZE, &size) == 0)
                {
                  libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "storage.removable.media_size", size, &hfp_error);
		}
	      close(fd);
	    }
	  ret = 2;		/* has media */
	}
      else
        {
          libhal_device_set_property_bool(hfp_ctx, hfp_udi, "storage.removable.media_available", FALSE, &hfp_error);
	}

      hfp_cdrom_free(cdrom);
    }
  else if (! has_children) /* by definition, if it has children it has no fs */
    {
      struct volume_id *vid;

      vid = volume_id_open_node(device_file);
      if (! vid)
	goto end;

      if (volume_id_probe_all(vid, 0, 0) == 0 && vid->usage_id == VOLUME_ID_FILESYSTEM)
	ret = 2;		/* has a filesystem */

      volume_id_close(vid);
    }

 end:
  return ret;
}
