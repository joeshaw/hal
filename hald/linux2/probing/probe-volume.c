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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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

#include "libhal/libhal.h"

#include "drive_id/drive_id.h"
#include "volume_id/volume_id.h"
#include "volume_id/msdos.h"

#include "linux_dvd_rw_utils.h"

#include "shared.h"

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
	case VOLUME_ID_UNUSED:
		libhal_device_set_property_string (ctx, udi, "info.product", "Volume (unused)", &error);
		usage = "unused";
		return;
	default:
		usage = "";
	}

	libhal_device_set_property_string (ctx, udi, "volume.fsusage", usage, &error);
	HAL_INFO (("volume.fsusage = '%s'", usage));

	libhal_device_set_property_string (ctx, udi, "volume.fstype", vid->type, &error);
	HAL_INFO (("volume.fstype = '%s'", vid->type));
	if (vid->type_version[0] != '\0') {
		libhal_device_set_property_string (ctx, udi, "volume.fsversion", vid->type_version, &error);
		HAL_INFO (("volume.fsversion = '%s'", vid->type_version));
	}
	libhal_device_set_property_string (ctx, udi, "volume.uuid", vid->uuid, &error);
	HAL_INFO (("volume.uuid = '%s'", vid->uuid));
	libhal_device_set_property_string (ctx, udi, "volume.label", vid->label, &error);
	HAL_INFO (("volume.label = '%s'", vid->label));

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
	DBusConnection *conn;
	char *parent_udi;
	char *sysfs_path;
	struct volume_id *vid;
	char *stordev_dev_file;
	char *partition_number_str;
	unsigned int partition_number;
	unsigned int block_size;
	dbus_uint64_t vol_size;

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
	if ((partition_number_str = getenv ("HAL_PROP_VOLUME_PARTITION_NUMBER")) == NULL)
		goto out;
	partition_number = (unsigned int) atoi (partition_number_str);

	dbus_error_init (&error);
	if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
		goto out;

	if ((ctx = libhal_ctx_new ()) == NULL)
		goto out;
	if (!libhal_ctx_set_dbus_connection (ctx, conn))
		goto out;
	if (!libhal_ctx_init (ctx, &error))
		goto out;

	printf ("**************************************************\n");
	printf ("**************************************************\n");
	printf ("Doing probe-volume for %s\n", device_file);
	printf ("**************************************************\n");
	printf ("**************************************************\n");

	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	/* probe for file system */
	vid = volume_id_open_fd (fd);
	if (vid != NULL) {
		if (volume_id_probe_all (vid, 0, 0 /* size */) == 0) {
			set_volume_id_values(ctx, udi, vid);
		} else {
			libhal_device_set_property_string (ctx, udi, "info.product", "Volume", &error);
		}
		volume_id_close(vid);
	}

	/* get partition type - presently we only support PC style partition tables */
	if ((stordev_dev_file = libhal_device_get_property_string (ctx, parent_udi, "block.device", &error)) == NULL) {
		goto out;
	}
	vid = volume_id_open_node (stordev_dev_file);
	if (vid != NULL) {
 		if (volume_id_probe_msdos_part_table (vid, 0) == 0) {
			HAL_INFO (("Number of partitions = %d", vid->partition_count));
			
			if (partition_number > 0 && partition_number <= vid->partition_count) {
				struct volume_id_partition *p;
				p = &vid->partitions[partition_number-1];

				libhal_device_set_property_int (ctx, udi,
								"volume.partition.msdos_part_table_type",
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
					libhal_device_set_property_string (ctx, udi, "volume.fsusage", "raid", &error);
				}
				
			} else {
				HAL_WARNING (("partition_number=%d not in [0;%d[", 
					      partition_number, vid->partition_count));
			}
		}
		volume_id_close(vid);
	}		
	libhal_free_string (stordev_dev_file);

	/* block size and total size */
	if (ioctl (fd, BLKSSZGET, &block_size) == 0) {
		HAL_INFO (("volume.block_size = %d", block_size));
		libhal_device_set_property_int (ctx, udi, "volume.block_size", block_size, &error);
	}
	if (ioctl (fd, BLKGETSIZE64, &vol_size) == 0) {
		HAL_INFO (("volume.size = %llu", vol_size));
		libhal_device_set_property_uint64 (ctx, udi, "volume.size", vol_size, &error);
	}

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
