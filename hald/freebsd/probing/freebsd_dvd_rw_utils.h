//
// This is part of dvd+rw-tools by Andy Polyakov <appro@fy.chalmers.se>
//
// Use-it-on-your-own-risk, GPL bless...
//
// For further details see http://fy.chalmers.se/~appro/linux/DVD+RW/
//

#ifndef FREEBSD_DVD_RW_UTILS_H
#define FREEBSD_DVD_RW_UTILS_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>

#include "../libprobe/hfp-cdrom.h"

#define DRIVE_CDROM_CAPS_DVDRW		1
#define DRIVE_CDROM_CAPS_DVDPLUSR	2
#define DRIVE_CDROM_CAPS_DVDPLUSRW	4
#define DRIVE_CDROM_CAPS_DVDPLUSRWDL	8
#define DRIVE_CDROM_CAPS_DVDPLUSRDL	16
#define DRIVE_CDROM_CAPS_BDROM		32
#define DRIVE_CDROM_CAPS_BDR		64
#define DRIVE_CDROM_CAPS_BDRE		128
#define DRIVE_CDROM_CAPS_HDDVDROM	256
#define DRIVE_CDROM_CAPS_HDDVDR		512
#define DRIVE_CDROM_CAPS_HDDVDRW	1024

int get_dvd_r_rw_profile (HFPCDROM *cdrom);
int get_read_write_speed (HFPCDROM *cdrom, int *read_speed, int *write_speed, char **write_speeds);
int get_disc_capacity_for_type (HFPCDROM *cdrom, int type, guint64 *capacity);
int get_disc_type (HFPCDROM *cdrom);
int disc_is_appendable (HFPCDROM *cdrom);
int disc_is_rewritable (HFPCDROM *cdrom);

#endif				/* FREEBSD_DVD_RW_UTILS_H */
