/***************************************************************************
 * CVSID: $Id$
 *
 * probe-scsi.c : SCSI prober
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
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <camlib.h>

#include "../libprobe/hfp.h"

int
main (int argc, char **argv)
{
  struct cam_device *cam;
  char *device_file;

  if (! hfp_init(argc, argv))
    goto end;

  device_file = getenv("HAL_PROP_BLOCK_DEVICE");
  if (! device_file)
    goto end;

  /* give a meaningful process title for ps(1) */
  setproctitle("%s", device_file);

  /* cam_open_device() fails unless we use O_RDWR */
  cam = cam_open_device(device_file, O_RDWR);
  if (cam)
    {
      if (cam->serial_num_len > 0)
	{
	  char *serial;

	  serial = hfp_strndup(cam->serial_num, cam->serial_num_len);
	  if (*serial)
	    libhal_device_set_property_string(hfp_ctx, hfp_udi, "storage.serial", serial, &hfp_error);
	  hfp_free(serial);
	}
      cam_close_device(cam);
    }

 end:
  return 0;
}
