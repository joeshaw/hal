/***************************************************************************
 * CVSID: $Id$
 *
 * hfp-cdrom.h : ATAPI/SCSI CD-ROM abstraction layer
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

#ifndef _HFP_CDROM_H
#define _HFP_CDROM_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdint.h>

#include "hfp.h"

typedef struct _HFPCDROM HFPCDROM;

typedef enum
{
  HFP_CDROM_DIRECTION_NONE,
  HFP_CDROM_DIRECTION_IN,
  HFP_CDROM_DIRECTION_OUT
} HFPCDROMDirection;

/* ATAPI/SCSI commands */
enum
{
  HFP_CDROM_TEST_UNIT_READY			= 0x00,
  HFP_CDROM_GET_EVENT_STATUS_NOTIFICATION	= 0x4a,
  HFP_CDROM_MODE_SENSE_BIG			= 0x5a
};

/* structure from sys/dev/ata/atapi-cd.h */
typedef struct
{
  /* mode page data header */
  uint16_t   data_length;
  uint8_t    medium_type;
#define HFP_CDROM_MST_TYPE_MASK_LOW	0x0f
#define HFP_CDROM_MST_FMT_NONE		0x00
#define HFP_CDROM_MST_DATA_120		0x01
#define HFP_CDROM_MST_AUDIO_120		0x02
#define HFP_CDROM_MST_COMB_120		0x03
#define HFP_CDROM_MST_PHOTO_120		0x04
#define HFP_CDROM_MST_DATA_80		0x05
#define HFP_CDROM_MST_AUDIO_80		0x06
#define HFP_CDROM_MST_COMB_80		0x07
#define HFP_CDROM_MST_PHOTO_80		0x08

#define HFP_CDROM_MST_TYPE_MASK_HIGH	0x70
#define HFP_CDROM_MST_CDROM		0x00
#define HFP_CDROM_MST_CDR		0x10
#define HFP_CDROM_MST_CDRW		0x20

#define HFP_CDROM_MST_NO_DISC		0x70
#define HFP_CDROM_MST_DOOR_OPEN		0x71
#define HFP_CDROM_MST_FMT_ERROR		0x72

  uint8_t    dev_spec;
  uint16_t   unused;
  uint16_t   blk_desc_len;

  /* capabilities page */
  uint8_t    page_code;
#define HFP_CDROM_CAP_PAGE		0x2a

  uint8_t    param_len;

  uint16_t   media;
#define HFP_CDROM_MST_READ_CDR		0x0001
#define HFP_CDROM_MST_READ_CDRW		0x0002
#define HFP_CDROM_MST_READ_PACKET	0x0004
#define HFP_CDROM_MST_READ_DVDROM	0x0008
#define HFP_CDROM_MST_READ_DVDR		0x0010
#define HFP_CDROM_MST_READ_DVDRAM	0x0020
#define HFP_CDROM_MST_WRITE_CDR		0x0100
#define HFP_CDROM_MST_WRITE_CDRW	0x0200
#define HFP_CDROM_MST_WRITE_TEST	0x0400
#define HFP_CDROM_MST_WRITE_DVDR	0x1000
#define HFP_CDROM_MST_WRITE_DVDRAM	0x2000

  uint16_t   capabilities;
#define HFP_CDROM_MSTAUDIO_PLAY		0x0001
#define HFP_CDROM_MST_COMPOSITE		0x0002
#define HFP_CDROM_MST_AUDIO_P1		0x0004
#define HFP_CDROM_MST_AUDIO_P2		0x0008
#define HFP_CDROM_MST_MODE2_f1		0x0010
#define HFP_CDROM_MST_MODE2_f2		0x0020
#define HFP_CDROM_MST_MULTISESSION	0x0040
#define HFP_CDROM_MST_BURNPROOF		0x0080
#define HFP_CDROM_MST_READ_CDDA		0x0100
#define HFP_CDROM_MST_CDDA_STREAM	0x0200
#define HFP_CDROM_MST_COMBINED_RW	0x0400
#define HFP_CDROM_MST_CORRECTED_RW	0x0800
#define HFP_CDROM_MST_SUPPORT_C2	0x1000
#define HFP_CDROM_MST_ISRC		0x2000
#define HFP_CDROM_MST_UPC		0x4000

  uint8_t    mechanism;
#define HFP_CDROM_MST_LOCKABLE		0x01
#define HFP_CDROM_MST_LOCKED		0x02
#define HFP_CDROM_MST_PREVENT		0x04
#define HFP_CDROM_MST_EJECT		0x08
#define HFP_CDROM_MST_MECH_MASK		0xe0
#define HFP_CDROM_MST_MECH_CADDY	0x00
#define HFP_CDROM_MST_MECH_TRAY		0x20
#define HFP_CDROM_MST_MECH_POPUP	0x40
#define HFP_CDROM_MST_MECH_CHANGER	0x80
#define HFP_CDROM_MST_MECH_CARTRIDGE	0xa0

  uint8_t     audio;
#define HFP_CDROM_MST_SEP_VOL		0x01
#define HFP_CDROM_MST_SEP_MUTE		0x02

  uint16_t   max_read_speed;		/* max raw data rate in bytes/1000 */
  uint16_t   max_vol_levels;		/* number of discrete volume levels */
  uint16_t   buf_size;			/* internal buffer size in bytes/1024 */
  uint16_t   cur_read_speed;		/* current data rate in bytes/1000  */

  uint8_t    reserved3;
  uint8_t    misc;

  uint16_t   max_write_speed;		/* max raw data rate in bytes/1000 */
  uint16_t   cur_write_speed;		/* current data rate in bytes/1000  */
  uint16_t   copy_protect_rev;
  uint16_t   reserved4;
} HFPCDROMCapabilities;

HFPCDROM *hfp_cdrom_new (const char *path, const char *parent);
HFPCDROM *hfp_cdrom_new_from_fd (int fd, const char *path, const char *parent);

boolean hfp_cdrom_send_ccb (HFPCDROM *cdrom,
			    const char *ccb,
			    int ccb_len,
			    HFPCDROMDirection direction,
			    void *data,
			    int len,
			    char **err);

boolean hfp_cdrom_test_unit_ready (HFPCDROM *cdrom);

void hfp_cdrom_free (HFPCDROM *cdrom);

#endif /* _HFP_CDROM_H */
