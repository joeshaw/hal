/***************************************************************************
 * CVSID: $Id$
 *
 * hfp-cdrom.c : ATAPI/SCSI CD-ROM abstraction layer
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
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/ata.h>
#include <stdio.h>
#include <camlib.h>
#include <cam/scsi/scsi_message.h>

#include "hfp.h"
#include "hfp-cdrom.h"

struct _HFPCDROM
{
  struct cam_device	*cam;		/* for SCSI drives */
  int			fd;		/* for ATAPI drives */
  int			channel;	/* for ATAPI on 5.X */
  int			device;		/* for ATAPI on 5.X */
  boolean		fd_owned;
};

static HFPCDROM *
hfp_cdrom_new_real (boolean has_fd,
                    int fd,
                    const char *path,
                    const char *parent)
{
  HFPCDROM *cdrom = NULL;
  struct cam_device *cam;

  assert(path != NULL);
  assert(parent != NULL);

  /* cam_open_device() fails unless we use O_RDWR */
  cam = cam_open_device(path, O_RDWR);
  if (cam)
    {
      cdrom = hfp_new0(HFPCDROM, 1);
      cdrom->cam = cam;
      cdrom->fd = -1;
    }
  else
    {
#ifndef IOCATAREQUEST
      fd = open("/dev/ata", O_RDONLY);
#else
      if (! has_fd)
	fd = open(path, O_RDONLY);
#endif
      if (fd >= 0)
	{
	  cdrom = hfp_new0(HFPCDROM, 1);
	  cdrom->fd = fd;
#ifndef IOCATAREQUEST
          cdrom->fd_owned = TRUE;
          cdrom->channel = libhal_device_get_property_int(hfp_ctx,
                                                          parent,
							  "ide.host",
							  &hfp_error);
	  dbus_error_free(&hfp_error);
	  cdrom->device = libhal_device_get_property_int(hfp_ctx,
                                                         parent,
							 "ide.channel",
							 &hfp_error);
	  dbus_error_free(&hfp_error);
#else
	  cdrom->fd_owned = ! has_fd;
	  cdrom->channel = -1;
	  cdrom->device = -1;
#endif
	}
    }

  return cdrom;
}

HFPCDROM *
hfp_cdrom_new (const char *path, const char *parent)
{
  assert(path != NULL);
  assert(parent != NULL);

  return hfp_cdrom_new_real(FALSE, -1, path, parent);
}

HFPCDROM *
hfp_cdrom_new_from_fd (int fd, const char *path, const char *parent)
{
  assert(path != NULL);
  assert(parent != NULL);

  return hfp_cdrom_new_real(TRUE, fd, path, parent);
}

boolean
hfp_cdrom_send_ccb (HFPCDROM *cdrom,
		    const char *ccb,
		    int ccb_len,
		    HFPCDROMDirection direction,
		    void *data,
		    int len,
		    char **err)
{
  int timeout;

  assert(cdrom != NULL);
  assert(ccb != NULL);
  assert(direction == HFP_CDROM_DIRECTION_NONE
	 || direction == HFP_CDROM_DIRECTION_IN
	 || direction == HFP_CDROM_DIRECTION_OUT);
  assert(direction == HFP_CDROM_DIRECTION_NONE || data != NULL);

  timeout = 10;

  if (cdrom->fd >= 0)		/* ATAPI transport */
    {
#ifdef IOCATAREQUEST
      struct ata_ioc_request req;

      memset(&req, 0, sizeof(req));
      req.flags = ATA_CMD_ATAPI;
      req.timeout = timeout;
      memcpy(req.u.atapi.ccb, ccb, 16);

      if (data)
	{
	  static int atapi_direction[] = { 0, ATA_CMD_READ, ATA_CMD_WRITE };

	  req.flags |= atapi_direction[direction];
	  req.data = data;
	  req.count = len;
	}

      if (ioctl(cdrom->fd, IOCATAREQUEST, &req) < 0)
	{
	  if (err)
	    *err = hfp_strdup_printf("IOCATAREQUEST failure: %s", strerror(errno));
	  return FALSE;
	}
      if (req.error != 0)
	{
	  if (err)
	    *err = hfp_strdup_printf("ATAPI error %i", req.error);
	  return FALSE;
	}
#else
      struct ata_cmd iocmd;

      /* Better to assert here than panic the machine. */
      /* XXX Should this be a conditional?  How likely is this? */
      assert(cdrom->channel >= 0);
      assert(cdrom->device >= 0 && cdrom->device < 2);

      memset(&iocmd, 0, sizeof(iocmd));
      iocmd.u.request.flags = ATA_CMD_ATAPI;
      iocmd.u.request.timeout = timeout;
      iocmd.cmd = ATAREQUEST;
      iocmd.channel = cdrom->channel;
      iocmd.device = cdrom->device;
      memcpy(iocmd.u.request.u.atapi.ccb, ccb, 16);

      if (data)
        {
	  static int atapi_direction[] = { 0, ATA_CMD_READ, ATA_CMD_WRITE };

	  iocmd.u.request.flags |= atapi_direction[direction];
	  iocmd.u.request.data = data;
	  iocmd.u.request.count = len;
	}

      if (ioctl(cdrom->fd, IOCATA, &iocmd) < 0)
        {
	  if (err)
	    *err = hfp_strdup_printf("IOCATA failure: %s", strerror(errno));
	  return FALSE;
	}
      if (iocmd.u.request.error != 0)
        {
	  if (err)
	    *err = hfp_strdup_printf("ATAPI error %i", iocmd.u.request.error);
	  return FALSE;
	}
#endif
    }
  else				/* SCSI transport */
    {
      union ccb cam_ccb;
      static int scsi_direction[] = { CAM_DIR_NONE, CAM_DIR_IN, CAM_DIR_OUT };

      memset(&cam_ccb, 0, sizeof(cam_ccb));

      cam_ccb.ccb_h.path_id = cdrom->cam->path_id;
      cam_ccb.ccb_h.target_id = cdrom->cam->target_id;
      cam_ccb.ccb_h.target_lun = cdrom->cam->target_lun;

      cam_fill_csio(&cam_ccb.csio,
		    1,
		    NULL,
		    scsi_direction[direction],
		    MSG_SIMPLE_Q_TAG,
		    data,
		    len,
		    sizeof(cam_ccb.csio.sense_data),
		    ccb_len,
		    timeout * 1000);

      memcpy(cam_ccb.csio.cdb_io.cdb_bytes, ccb, 16);

      if (cam_send_ccb(cdrom->cam, &cam_ccb) == -1)
	{
	  if (err)
	    *err = hfp_strdup_printf("cam_send_ccb() failure: %s", strerror(errno));
	}
      if ((cam_ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP)
	{
	  if (err)
	    *err = hfp_strdup_printf("CCB request failed with status %i", cam_ccb.ccb_h.status & CAM_STATUS_MASK);
	  return FALSE;
	}
    }

  return TRUE;
}

boolean
hfp_cdrom_test_unit_ready (HFPCDROM *cdrom)
{
  static char ccb[16] = { HFP_CDROM_TEST_UNIT_READY };

  assert(cdrom != NULL);

  return hfp_cdrom_send_ccb(cdrom, ccb, 6, HFP_CDROM_DIRECTION_NONE, NULL, 0, NULL);
}

void
hfp_cdrom_free (HFPCDROM *cdrom)
{
  assert(cdrom != NULL);

  if (cdrom->cam)
    cam_close_device(cdrom->cam);
  if (cdrom->fd_owned && cdrom->fd >= 0)
    close(cdrom->fd);

  hfp_free(cdrom);
}
