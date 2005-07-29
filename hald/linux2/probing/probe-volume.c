/***************************************************************************
 * CVSID: $Id$
 *
 * probe-volume.c : Probe for volume type (filesystems etc.)
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.0
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

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <asm/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <time.h>
#include <sys/time.h>

#include "libhal/libhal.h"

#include "drive_id/drive_id.h"
#include "volume_id/volume_id.h"
#include "volume_id/logging.h"
#include "volume_id/msdos.h"

#include "linux_dvd_rw_utils.h"

#include "shared.h"

void 
volume_id_log (const char *format, ...)
{
	va_list args;
	va_start (args, format);
	_do_dbg (format, args);
}

static void
set_volume_id_values (LibHalContext *ctx, const char *udi, struct volume_id *vid)
{
	char buf[256];
	const char *usage;
	DBusError error;

	dbus_error_init (&error);

	switch (vid->usage_id) {
	case VOLUME_ID_FILESYSTEM:
		usage = "filesystem";
		break;
	case VOLUME_ID_PARTITIONTABLE:
		usage = "partitiontable";
		break;
	case VOLUME_ID_OTHER:
		usage = "other";
		break;
	case VOLUME_ID_RAID:
		usage = "raid";
		break;
	case VOLUME_ID_CRYPTO:
		usage = "crypto";
		break;
	case VOLUME_ID_UNUSED:
		libhal_device_set_property_string (ctx, udi, "info.product", "Volume (unused)", &error);
		usage = "unused";
		return;
	default:
		usage = "";
	}

	libhal_device_set_property_string (ctx, udi, "volume.fsusage", usage, &error);
	dbg ("volume.fsusage = '%s'", usage);

	libhal_device_set_property_string (ctx, udi, "volume.fstype", vid->type, &error);
	dbg ("volume.fstype = '%s'", vid->type);
	if (vid->type_version[0] != '\0') {
		libhal_device_set_property_string (ctx, udi, "volume.fsversion", vid->type_version, &error);
		dbg ("volume.fsversion = '%s'", vid->type_version);
	}
	libhal_device_set_property_string (ctx, udi, "volume.uuid", vid->uuid, &error);
	dbg ("volume.uuid = '%s'", vid->uuid);
	libhal_device_set_property_string (ctx, udi, "volume.label", vid->label, &error);
	dbg ("volume.label = '%s'", vid->label);

	if (vid->label[0] != '\0') {
		libhal_device_set_property_string (ctx, udi, "info.product", vid->label, &error);
	} else {
		snprintf (buf, sizeof (buf), "Volume (%s)", vid->type);
		libhal_device_set_property_string (ctx, udi, "info.product", buf, &error);
	}
}

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	char *device_file;
	LibHalContext *ctx = NULL;
	DBusError error;
	char *parent_udi;
	char *sysfs_path;
	struct volume_id *vid;
	char *stordev_dev_file;
	char *partition_number_str;
	char *is_disc_str;
	dbus_bool_t is_disc;
	unsigned int partition_number;
	unsigned int block_size;
	dbus_uint64_t vol_size;
	dbus_bool_t should_probe_for_fs;
	dbus_uint64_t vol_probe_offset = 0;
	fd = -1;

	/* assume failure */
	ret = 1;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
	if ((parent_udi = getenv ("HAL_PROP_INFO_PARENT")) == NULL)
		goto out;
	if ((sysfs_path = getenv ("HAL_PROP_LINUX_SYSFS_PATH")) == NULL)
		goto out;
	partition_number_str = getenv ("HAL_PROP_VOLUME_PARTITION_NUMBER");
	if (partition_number_str != NULL)
		partition_number = (unsigned int) atoi (partition_number_str);
	else
		partition_number = (unsigned int) -1;

	is_disc_str = getenv ("HAL_PROP_VOLUME_IS_DISC");
	if (is_disc_str != NULL && strcmp (is_disc_str, "true") == 0)
		is_disc = TRUE;
	else
		is_disc = FALSE;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	dbg ("Doing probe-volume for %s\n", device_file);

	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	/* block size and total size */
	if (ioctl (fd, BLKSSZGET, &block_size) == 0) {
		dbg ("volume.block_size = %d", block_size);
		libhal_device_set_property_int (ctx, udi, "volume.block_size", block_size, &error);
	}
	if (ioctl (fd, BLKGETSIZE64, &vol_size) == 0) {
		dbg ("volume.size = %llu", vol_size);
		libhal_device_set_property_uint64 (ctx, udi, "volume.size", vol_size, &error);
	} else
		vol_size = 0;

	should_probe_for_fs = TRUE;

	if (is_disc) {
		int type;
		struct cdrom_tochdr toc_hdr;

		/* defaults */
		libhal_device_set_property_string (ctx, udi, "volume.disc.type", "unknown", &error);
		libhal_device_set_property_bool (ctx, udi, "volume.disc.has_audio", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "volume.disc.has_data", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "volume.disc.is_blank", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "volume.disc.is_appendable", FALSE, &error);
		libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", FALSE, &error);

		/* Suggested by Alex Larsson to get rid of log spewage
		 * on Alan's cd changer (RH bug 130649) */
		if (ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) != CDS_DISC_OK) {
			goto out;
		}

		/* check for audio/data/blank */
		type = ioctl (fd, CDROM_DISC_STATUS, CDSL_CURRENT);
		switch (type) {
		case CDS_AUDIO:		/* audio CD */
			libhal_device_set_property_bool (ctx, udi, "volume.disc.has_audio", TRUE, &error);
			dbg ("Disc in %s has audio", device_file);
			should_probe_for_fs = FALSE;
			break;
		case CDS_MIXED:		/* mixed mode CD */
			libhal_device_set_property_bool (ctx, udi, "volume.disc.has_audio", TRUE, &error);
			libhal_device_set_property_bool (ctx, udi, "volume.disc.has_data", TRUE, &error);
			dbg ("Disc in %s has audio+data", device_file);
			break;
		case CDS_DATA_1:	/* data CD */
		case CDS_DATA_2:
		case CDS_XA_2_1:
		case CDS_XA_2_2:
			libhal_device_set_property_bool (ctx, udi, "volume.disc.has_data", TRUE, &error);
			dbg ("Disc in %s has data", device_file);
			break;
		case CDS_NO_INFO:	/* blank or invalid CD */
			libhal_device_set_property_bool (ctx, udi, "volume.disc.is_blank", TRUE, &error);
			dbg ("Disc in %s is blank", device_file);
			should_probe_for_fs = FALSE;
			break;
			
		default:		/* should never see this */
			libhal_device_set_property_string (ctx, udi, "volume.disc_type", "unknown", &error);
			dbg ("Disc in %s returned unknown CDROM_DISC_STATUS", device_file);
			should_probe_for_fs = FALSE;
			break;
		}
		
		/* see table 373 in MMC-3 for details on disc type
		 * http://www.t10.org/drafts.htm#mmc3
		 */
		type = get_disc_type (fd);
		dbg ("get_disc_type returned 0x%02x", type);
		if (type != -1) {
			switch (type) {
			case 0x08: /* CD-ROM */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "cd_rom", &error);
				break;
			case 0x09: /* CD-R */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "cd_r", &error);
				break;
			case 0x0a: /* CD-RW */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "cd_rw", &error);
				libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", TRUE, &error);
				break;
			case 0x10: /* DVD-ROM */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_rom", &error);
				break;
			case 0x11: /* DVD-R Sequential */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_r", &error);
				break;
			case 0x12: /* DVD-RAM */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_ram", &error);
				libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", TRUE, &error);
				break;
			case 0x13: /* DVD-RW Restricted Overwrite */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_rw", &error);
				libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", TRUE, &error);
				break;
			case 0x14: /* DVD-RW Sequential */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_rw", &error);
				libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", TRUE, &error);
				break;
			case 0x1A: /* DVD+RW */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_plus_rw", &error);
				libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", TRUE, &error);
				break;
			case 0x1B: /* DVD+R */
				libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_plus_r", &error);
				break;
			case 0x2B: /* DVD+R Double Layer */
                          	libhal_device_set_property_string (ctx, udi, "volume.disc.type", "dvd_plus_r_dl", &error);
				break;
			default: 
				break;
			}
		}

		/* On some hardware the get_disc_type call fails, so we use this as a backup */
		if (disc_is_rewritable (fd)) {
			libhal_device_set_property_bool (ctx, udi, "volume.disc.is_rewritable", TRUE, &error);
		}
		
		if (disc_is_appendable (fd)) {
			libhal_device_set_property_bool (ctx, udi, "volume.disc.is_appendable", TRUE, &error);
		}

		/* check for multisession disks */
		if (ioctl (fd, CDROMREADTOCHDR, &toc_hdr) == 0) {
			struct cdrom_tocentry toc_entr;
			unsigned int vol_session_count = 0;

			vol_session_count = toc_hdr.cdth_trk1;
			dbg ("volume_session_count = %u", vol_session_count);

			/* read session header */
			memset (&toc_entr, 0x00, sizeof (toc_entr));
			toc_entr.cdte_track = vol_session_count;
			toc_entr.cdte_format = CDROM_LBA;
			if (ioctl (fd, CDROMREADTOCENTRY, &toc_entr) == 0)
				if ((toc_entr.cdte_ctrl & CDROM_DATA_TRACK) == 4) {
					dbg ("last session starts at block = %u", toc_entr.cdte_addr.lba);
					vol_probe_offset = toc_entr.cdte_addr.lba * block_size;
				}
		}

		/* try again, to get last session that way */
		if (vol_probe_offset == 0) {
			struct cdrom_multisession ms_info;

			memset(&ms_info, 0x00, sizeof(ms_info));
			ms_info.addr_format = CDROM_LBA;
			if (ioctl(fd, CDROMMULTISESSION, &ms_info) == 0)
				if (!ms_info.xa_flag)
					vol_probe_offset = ms_info.addr.lba * block_size;
		}
	}

	if (should_probe_for_fs) {

		/* Optical discs have problems reporting the exact
		 * size so we should never look for data there since
		 * it causes problems with the broken ide-cd driver
		 */
		if (is_disc)
			vol_size = 0;

		/* probe for file system */
		vid = volume_id_open_fd (fd);
		if (vid != NULL) {
			if (volume_id_probe_all (vid, vol_probe_offset , vol_size) == 0) {
				set_volume_id_values(ctx, udi, vid);
			} else {
				libhal_device_set_property_string (ctx, udi, "info.product", "Volume", &error);
			}
			volume_id_close(vid);
		}

		/* get partition type (if we are from partitioned media)
		 *
		 * (presently we only support PC style partition tables)
		 */
		if (partition_number_str != NULL) {
			if ((stordev_dev_file = libhal_device_get_property_string (
				     ctx, parent_udi, "block.device", &error)) == NULL) {
				goto out;
			}
			vid = volume_id_open_node (stordev_dev_file);
			if (vid != NULL) {
				if (volume_id_probe_msdos_part_table (vid, 0) == 0) {
					dbg ("Number of partitions = %d", vid->partition_count);
					
					if (partition_number > 0 && partition_number <= vid->partition_count) {
						struct volume_id_partition *p;
						p = &vid->partitions[partition_number-1];
						
						libhal_device_set_property_int (
							ctx, udi, "volume.partition.msdos_part_table_type",
							p->partition_type_raw, &error);
						
						/* NOTE: We trust the type from the partition table
						 * if it explicitly got correct entries for RAID and
						 * LVM partitions.
						 *
						 * Btw, in general it's not a good idea to trust the
						 * partition table type as many geek^Wexpert users use 
						 * FAT filesystems on type 0x83 which is Linux.
						 *
						 * Linux RAID autodetect is 0xfd and Linux LVM is 0x8e
						 */
						if (p->partition_type_raw == 0xfd ||
						    p->partition_type_raw == 0x8e ) {
							libhal_device_set_property_string (
								ctx, udi, "volume.fsusage", "raid", &error);
						}
						
					} else {
						dbg ("warning: partition_number=%d not in [0;%d[", 
						     partition_number, vid->partition_count);
					}
				}
				volume_id_close(vid);
			}		
			libhal_free_string (stordev_dev_file);
		}
	}

	/* good so far */
	ret = 0;


out:
	if (fd >= 0)
		close (fd);

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;

}
