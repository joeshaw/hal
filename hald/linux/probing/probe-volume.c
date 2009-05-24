/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-
 ***************************************************************************
 * CVSID: $Id$
 *
 * probe-volume.c : Probe for volume type (filesystems etc.)
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.1
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>

#include <glib.h>
#include <blkid.h>

#include "libhal/libhal.h"
#include "partutil/partutil.h"
#include "linux_dvd_rw_utils.h"
#include "../../logger.h"

static gchar *
strdup_valid_utf8 (const char *str)
{
	char *endchar;
	char *newstr;
	unsigned int fixes;

	if (str == NULL)
		return NULL;

	newstr = g_strdup (str);

	fixes = 0;
	while (!g_utf8_validate (newstr, -1, (const char **) &endchar)) {
		*endchar = '_';
		++fixes;
	}

	/* If we had to fix more than 20% of the characters, give up */
	if (fixes > 0 && g_utf8_strlen (newstr, -1) / fixes < 5) {
	    g_free (newstr);
	    newstr = g_strdup("");
	}

	return newstr;
}


static void
set_blkid_values (LibHalChangeSet *cs, blkid_probe pr)
{
	char buf[256];
	const char *usage;
	const char *type;
	const char *type_version;
	const char *label;
	const char *uuid;
	DBusError error;

	dbus_error_init (&error);

	if (blkid_probe_lookup_value(pr, "USAGE", &usage, NULL))
		usage = "";
	libhal_changeset_set_property_string (cs, "volume.fsusage", usage);
	HAL_DEBUG (("volume.fsusage = '%s'", usage));

	if (blkid_probe_lookup_value(pr, "TYPE", &type, NULL))
		type = "";
	if (!libhal_changeset_set_property_string (cs, "volume.fstype", type))
		libhal_changeset_set_property_string (cs, "volume.fstype", "");
	HAL_DEBUG(("volume.fstype = '%s'", type));

	if (blkid_probe_lookup_value(pr, "VERSION", &type_version, NULL))
		type_version = "";
	libhal_changeset_set_property_string (cs, "volume.fsversion", type_version);
	HAL_DEBUG(("volume.fsversion = '%s'", type_version));

	if (blkid_probe_lookup_value(pr, "UUID", &uuid, NULL))
		uuid = "";
	libhal_changeset_set_property_string (cs, "volume.uuid", uuid);
	HAL_DEBUG(("volume.uuid = '%s'", uuid));

	if (blkid_probe_lookup_value(pr, "LABEL", &label, NULL))
		label = "";

	if (label[0] != '\0') {
		char *volume_label;

		/* we need to be sure for a utf8 valid label, because dbus accept only utf8 valid strings */
		volume_label = strdup_valid_utf8 (label);
		if( volume_label != NULL ) {
			libhal_changeset_set_property_string (cs, "volume.label", volume_label);
			HAL_DEBUG(("volume.label = '%s'", volume_label));

			if (volume_label[0] != '\0') {
				libhal_changeset_set_property_string (cs, "info.product", volume_label);
				g_free(volume_label);
				return;
			}

			g_free(volume_label);
		}
	}

	if (type[0] != '\0') {
		snprintf (buf, sizeof (buf), "Volume (%s)", type);
	} else {
		snprintf (buf, sizeof (buf), "Volume (unknown)");
	}
	libhal_changeset_set_property_string (cs, "info.product", buf);
}

static void
advanced_disc_detect (LibHalChangeSet *cs, int fd, const char *device_file)
{
	/* the discs block size */
	unsigned short bs; 
	/* the path table size */
	unsigned short ts;
	/* the path table location (in blocks) */
	unsigned int tl;
	/* length of the directory name in current path table entry */
	unsigned char len_di = 0;
	/* the number of the parent directory's path table entry */
	unsigned int parent = 0; 
	/* filename for the current path table entry */
	char dirname[256];
	/* our position into the path table */
	int pos = 0; 
	/* the path table record we're on */
	int curr_record = 1; 
	/* loop counter */
	int i; 
	DBusError error;
	
	dbus_error_init (&error);
	
	/* set defaults */
	libhal_changeset_set_property_bool (cs, "volume.disc.is_videodvd", FALSE);
	libhal_changeset_set_property_bool (cs, "volume.disc.is_blurayvideo", FALSE);
	libhal_changeset_set_property_bool (cs, "volume.disc.is_vcd", FALSE);
	libhal_changeset_set_property_bool (cs, "volume.disc.is_svcd", FALSE);
	
	/* read the block size */
	lseek (fd, 0x8080, SEEK_CUR);
	if (read (fd, &bs, 2) != 2)
	{
		HAL_DEBUG(("Advanced probing on %s failed while reading block size", device_file));
		goto out;
	}

	/* read in size of path table */
	lseek (fd, 2, SEEK_CUR);
	if (read (fd, &ts, 2) != 2)
	{
		HAL_DEBUG(("Advanced probing on %s failed while reading path table size", device_file));
		goto out;
	}

	/* read in which block path table is in */
	lseek (fd, 6, SEEK_CUR);
	if (read (fd, &tl, 4) != 4)
	{
		HAL_DEBUG(("Advanced probing on %s failed while reading path table block", device_file));
		goto out;
	}

	/* seek to the path table */
	lseek (fd, GUINT16_FROM_LE (bs) * GUINT32_FROM_LE (tl), SEEK_SET);

	/* loop through the path table entriesi */
	while (pos < GUINT16_FROM_LE (ts))
	{
		/* get the length of the filename of the current entry */
		if (read (fd, &len_di, 1) != 1)
		{
			HAL_DEBUG(("Advanced probing on %s failed, cannot read more entries", device_file));
			break;
		}

		/* get the record number of this entry's parent
		   i'm pretty sure that the 1st entry is always the top directory */
		lseek (fd, 5, SEEK_CUR);
		if (read (fd, &parent, 2) != 2)
		{
			HAL_DEBUG(("Advanced probing on %s failed, couldn't read parent entry", device_file));
			break;
		}
		
		/* read the name */
		if (read (fd, dirname, len_di) != len_di)
		{
			HAL_DEBUG(("Advanced probing on %s failed, couldn't read the entry name", device_file));
			break;
		}
		dirname[len_di] = 0;

		/* strcasecmp is not POSIX or ANSI C unfortunately */
		i=0;
		while (dirname[i]!=0)
		{
			dirname[i] = (char)toupper (dirname[i]);
			i++;
		}

		/* if we found a folder that has the root as a parent, and the directory name matches 
		   one of the special directories then set the properties accordingly */
		if (GUINT16_FROM_LE (parent) == 1)
		{
			if (!strcmp (dirname, "VIDEO_TS"))
			{
				libhal_changeset_set_property_bool (cs, "volume.disc.is_videodvd", TRUE);
				HAL_DEBUG(("Disc in %s is a Video DVD", device_file));
				break;
			}
			else if (!strcmp (dirname, "BDMV"))
			{
				libhal_changeset_set_property_bool (cs, "volume.disc.is_blurayvideo", TRUE);
				HAL_DEBUG(("Disc in %s is a Blu-ray video disc", device_file));
				break;
			}
			else if (!strcmp (dirname, "VCD"))
			{
				libhal_changeset_set_property_bool (cs, "volume.disc.is_vcd", TRUE);
				HAL_DEBUG(("Disc in %s is a Video CD", device_file));
				break;
			}
			else if (!strcmp (dirname, "SVCD"))
			{
				libhal_changeset_set_property_bool (cs, "volume.disc.is_svcd", TRUE);
				HAL_DEBUG(("Disc in %s is a Super Video CD", device_file));
				break;
			}
		}

		/* all path table entries are padded to be even, 
		   so if this is an odd-length table, seek a byte to fix it */
		if (len_di%2 == 1)
		{
			lseek (fd, 1, SEEK_CUR);
			pos++;
		}

		/* update our position */
		pos += 8 + len_di;
		curr_record++;
	}

out:
	/* go back to the start of the file */
	lseek (fd, 0, SEEK_SET);
}

static char *device_file;

static void
handle_sigterm (int value)
{
	HAL_ERROR (("Timed out probing %s - broken device driver?", device_file));
	exit (1);
}

int 
main (int argc, char *argv[])
{
	int fd;
	int ret;
	char *udi;
	LibHalContext *ctx = NULL;
	DBusError error;
	char *parent_udi;
	blkid_probe pr;
	char *stordev_dev_file;
	char *partition_number_str;
	char *partition_start_str;
	char *is_disc_str;
	char *fsusage;
	dbus_bool_t is_disc;
	unsigned int partition_number;
	guint64 partition_start;
	unsigned int block_size;
	dbus_uint64_t vol_size;
	dbus_bool_t should_probe_for_fs;
	dbus_uint64_t vol_probe_offset = 0;
	LibHalChangeSet *cs;
	gboolean disc_may_have_data;
	fd = -1;

	cs = NULL;
	disc_may_have_data = FALSE;

	setup_logger ();

	/* assume failure */
	ret = 1;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
	if ((parent_udi = getenv ("HAL_PROP_INFO_PARENT")) == NULL)
		goto out;
	if (getenv ("HAL_PROP_LINUX_SYSFS_PATH") == NULL)
		goto out;
	partition_number_str = getenv ("HAL_PROP_VOLUME_PARTITION_NUMBER");
	if (partition_number_str != NULL)
		partition_number = (unsigned int) atoi (partition_number_str);
	else
		partition_number = (unsigned int) -1;

	partition_start_str = getenv ("HAL_PROP_VOLUME_PARTITION_START");
	if (partition_start_str != NULL)
		partition_start = (guint64) strtoll (partition_start_str, NULL, 0);
	else
		partition_start = (guint64) 0;

	is_disc_str = getenv ("HAL_PROP_VOLUME_IS_DISC");
	if (is_disc_str != NULL && strcmp (is_disc_str, "true") == 0)
		is_disc = TRUE;
	else
		is_disc = FALSE;

	fsusage = getenv ("HAL_PROP_VOLUME_FSUSAGE");

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	cs = libhal_device_new_changeset (udi);
	if (cs == NULL) {
		HAL_DEBUG(("Cannot initialize changeset"));
		goto out;
	}

	HAL_DEBUG(("Doing probe-volume for %s\n", device_file));

	/* set up signal handler in case I/O to the device takes too long and the runner kills us */
	signal (SIGTERM, handle_sigterm);

	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	/* block size and total size */
	if (ioctl (fd, BLKSSZGET, &block_size) == 0) {
		HAL_DEBUG(("volume.block_size = %d", block_size));
		libhal_changeset_set_property_int (cs, "volume.block_size", block_size);
	}
	if (ioctl (fd, BLKGETSIZE64, &vol_size) == 0) {
		HAL_DEBUG(("volume.size = %llu", vol_size));
		libhal_changeset_set_property_uint64 (cs, "volume.size", vol_size);
	} else
		vol_size = 0;

	should_probe_for_fs = TRUE;

	if (is_disc) {
		int type;
		guint64 capacity;
		struct cdrom_tochdr toc_hdr;

		/* defaults */
		libhal_changeset_set_property_string (cs, "volume.disc.type", "unknown");
		libhal_changeset_set_property_bool (cs, "volume.disc.has_audio", FALSE);
		libhal_changeset_set_property_bool (cs, "volume.disc.has_data", FALSE);
		libhal_changeset_set_property_bool (cs, "volume.disc.is_blank", FALSE);
		libhal_changeset_set_property_bool (cs, "volume.disc.is_appendable", FALSE);
		libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", FALSE);

		/* Suggested by Alex Larsson to get rid of log spewage
		 * on Alan's cd changer (RH bug 130649) */
		if (ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) != CDS_DISC_OK) {
			goto out;
		}

		/* check for audio/data/blank */
		type = ioctl (fd, CDROM_DISC_STATUS, CDSL_CURRENT);
		switch (type) {
		case CDS_AUDIO:		/* audio CD */
			libhal_changeset_set_property_bool (cs, "volume.disc.has_audio", TRUE);
			HAL_DEBUG(("Disc in %s has audio", device_file));
			should_probe_for_fs = FALSE;
			break;
		case CDS_MIXED:		/* mixed mode CD */
			libhal_changeset_set_property_bool (cs, "volume.disc.has_audio", TRUE);
			libhal_changeset_set_property_bool (cs, "volume.disc.has_data", TRUE);
			HAL_DEBUG(("Disc in %s has audio+data", device_file));
			break;
		case CDS_DATA_1:	/* data CD */
		case CDS_DATA_2:
		case CDS_XA_2_1:
		case CDS_XA_2_2:
			libhal_changeset_set_property_bool (cs, "volume.disc.has_data", TRUE);
			HAL_DEBUG(("Disc in %s has data", device_file));
			advanced_disc_detect (cs, fd, device_file);
			break;
		case CDS_NO_INFO:	/* blank or invalid CD */
			libhal_changeset_set_property_bool (cs, "volume.disc.is_blank", TRUE);
			/* set the volume size to 0 if disc is blank and not as 4 from BLKGETSIZE64 */
			libhal_changeset_set_property_int (cs, "volume.block_size", 0);
			HAL_DEBUG(("Disc in %s probably blank", device_file));
			/* 
			 * blank discs normally return 0x0800 (probably for the TOC) - if it's larger,
			 * it probably means that the drive/firmware is bad and CDROM_DISC_STATUS lies.
			 * In this case, actually try to probe for a file system. See RH #186334 for
			 * details: https://bugzilla.redhat.com/bugzilla/show_bug.cgi?id=186334
			 */
			if (vol_size > 0x800) {
				should_probe_for_fs = TRUE;
				disc_may_have_data = TRUE;
			} else {
				should_probe_for_fs = FALSE;
			}
			break;
			
		default:		/* should never see this */
			libhal_changeset_set_property_string (cs, "volume.disc_type", "unknown");
			HAL_DEBUG(("Disc in %s returned unknown CDROM_DISC_STATUS", device_file));
			should_probe_for_fs = FALSE;
			break;
		}
		
		/* see table 87 - Profile List in MMC-5 for details on disc type
		 * http://www.t10.org/drafts.htm#mmc5
		 */
		type = get_disc_type (fd);
		HAL_DEBUG(("get_disc_type returned 0x%02x", type));
		if (type != -1) {
			switch (type) {
			case 0x03: /* Magneto-Optical disk with sector erase capability */
			case 0x05: /* Advance Storage – Magneto-Optical */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "mo");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x04: /* Magneto Optical write once */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "mo");
				break;
			case 0x08: /* CD-ROM */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "cd_rom");
				break;
			case 0x09: /* CD-R */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "cd_r");
				break;
			case 0x0a: /* CD-RW */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "cd_rw");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x10: /* DVD-ROM */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_rom");
				break;
			case 0x11: /* DVD-R Sequential */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_r");
				break;
			case 0x12: /* DVD-RAM */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_ram");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x13: /* DVD-RW Restricted Overwrite */
			case 0x14: /* DVD-RW Sequential */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_rw");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x15: /* DVD-R Dual Layer Sequential */
			case 0x16: /* DVD-R Dual Layer Jump */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_r_dl");
				break;
			case 0x1A: /* DVD+RW */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_plus_rw");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x1B: /* DVD+R */
				libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_plus_r");
				break;
			case 0x2B: /* DVD+R Double Layer */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "dvd_plus_r_dl");
				break;
			case 0x40: /* BD-ROM  */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "bd_rom");
				break;
			case 0x41: /* BD-R Sequential */
			case 0x42: /* BD-R Random */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "bd_r");
				break;
			case 0x43: /* BD-RE */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "bd_re");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x50: /* HD DVD-ROM */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "hddvd_rom");
				break;
			case 0x51: /* HD DVD-R */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "hddvd_r");
				break;
			case 0x52: /* HD DVD-Rewritable */
                          	libhal_changeset_set_property_string (cs, "volume.disc.type", "hddvd_rw");
				libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
				break;
			default: 
				break;
			}
		}

		if (get_disc_capacity_for_type (fd, type, &capacity) == 0) {
			HAL_DEBUG(("volume.disc.capacity = %llu", capacity));
			libhal_changeset_set_property_uint64 (cs, "volume.disc.capacity", capacity);
		}

		/* On some hardware the get_disc_type call fails, so we use this as a backup */
		if (disc_is_rewritable (fd)) {
			libhal_changeset_set_property_bool (cs, "volume.disc.is_rewritable", TRUE);
		}
		
		if (disc_is_appendable (fd)) {
			libhal_changeset_set_property_bool (cs, "volume.disc.is_appendable", TRUE);
		}

		/* In November 2005, Kay wrote:
		 *
		 *  "This seems to cause problems on some drives with broken firmware,
		 *   comment it out until we really need multisession support."
		 *
		 * However, we really need this for
		 *
		 *  - supporting mixed CD's - we want to probe the data track which
		 *    may not be the first track; normally it's the last one...
		 *  - getting the right label for multi-session discs (fd.o bug #2860)
		 *
		 * So if there are still drives around with broken firmware we need
		 * to blacklist them.
		 */

		/* check if last track is a data track */
		if (ioctl (fd, CDROMREADTOCHDR, &toc_hdr) == 0) {
			struct cdrom_tocentry toc_entr;
			unsigned int vol_session_count = 0;

			vol_session_count = toc_hdr.cdth_trk1;
			HAL_DEBUG(("volume_session_count = %u", vol_session_count));

			/* read session header */
			memset (&toc_entr, 0x00, sizeof (toc_entr));
			toc_entr.cdte_track = vol_session_count;
			toc_entr.cdte_format = CDROM_LBA;
			if (ioctl (fd, CDROMREADTOCENTRY, &toc_entr) == 0) {
				if (toc_entr.cdte_ctrl & CDROM_DATA_TRACK) {
					HAL_DEBUG (("last session starts at block = %u", toc_entr.cdte_addr.lba));
					vol_probe_offset = toc_entr.cdte_addr.lba * block_size;
				}
			}
		}
		
		/* try again, to get last session that way */
		if (vol_probe_offset == 0) {
			struct cdrom_multisession ms_info;

			memset(&ms_info, 0x00, sizeof(ms_info));
			ms_info.addr_format = CDROM_LBA;
			if (ioctl(fd, CDROMMULTISESSION, &ms_info) == 0) {
				if (!ms_info.xa_flag) {
					vol_probe_offset = ms_info.addr.lba * block_size;
				}
			}
		}
	}

	if (fsusage != NULL && strlen(fsusage) > 0) {
		HAL_DEBUG(("have already information about fsusage from udev, no need to probe for filesystem"));
		should_probe_for_fs = FALSE;
	}

	if (should_probe_for_fs) {

		HAL_DEBUG(("start probing for filesystem ..."));

		if ((stordev_dev_file = libhal_device_get_property_string (
			     ctx, parent_udi, "block.device", &error)) == NULL) {
			goto out;
		}

		/* Optical discs have problems reporting the exact
		 * size so we should never look for data there since
		 * it causes problems with the broken ide-cd driver
		 */
		if (is_disc) {
			vol_size = 0;
		}

		/* probe for file system */
		pr = blkid_new_probe ();
		if (pr != NULL) {
			int bid_ret;

			blkid_probe_set_request (pr, BLKID_PROBREQ_LABEL | BLKID_PROBREQ_UUID |
						 BLKID_PROBREQ_TYPE | BLKID_PROBREQ_SECTYPE |
						 BLKID_PROBREQ_USAGE | BLKID_PROBREQ_VERSION);

			HAL_INFO (("invoking blkid_do_safeprobe, offset=%d, size=%d", vol_probe_offset, vol_size));
			bid_ret = blkid_probe_set_device (pr, fd, vol_probe_offset, vol_size);
			if (bid_ret == 0) {
				bid_ret = blkid_do_safeprobe (pr);
				HAL_INFO (("blkid_do_safeprobe returned %d", bid_ret));
			}

			if (bid_ret != 0 && is_disc && vol_probe_offset != 0) {
				/* Some cd-rom drives report the offset of the session in the cd's TOC
				 * wrong.  Fallback to probing at offset 0, just to be sure */
				HAL_INFO (("invoking blkid_do_safeprobe, offset=0, size=%d", vol_size));
				bid_ret = blkid_probe_set_device (pr, fd, 0, vol_size);
				if (bid_ret == 0)
					bid_ret = blkid_do_safeprobe (pr);
				HAL_INFO (("blkid_do_safeprobe returned %d", bid_ret));
			}

			if (bid_ret == 0) {
				set_blkid_values(cs, pr);
				if (disc_may_have_data) {
					libhal_changeset_set_property_bool (cs, "volume.disc.is_blank", FALSE);
					libhal_changeset_set_property_bool (cs, "volume.disc.has_data", TRUE);
				}
			} else {
				libhal_changeset_set_property_string (cs, "info.product", "Volume");
			}

			/* If we didn't detect anything, look whether it's a partition table (some Apple discs
			 * uses Apple Partition Map) and look at partitions
			 *
			 * (kind of a hack - ugh  - we ought to export all these as fakevolumes... but
			 *  this is good enough for now... the only discs I know of that does this
			 *  is in fact Apple's install disc.)
			 */
			if (bid_ret != 0 && is_disc) {
				PartitionTable *p;
				p = part_table_load_from_disk (stordev_dev_file);
				if (p != NULL) {
					int i;

					HAL_INFO (("Partition table with scheme '%s' on optical disc",
						   part_get_scheme_name (part_table_get_scheme (p))));

					for (i = 0; i < part_table_get_num_entries (p); i++) {
						char *part_type;

						part_type = part_table_entry_get_type (p, i);
						HAL_INFO ((" partition %d has type '%s'", i, part_type));
						if (strcmp (part_type, "Apple_HFS") == 0) {
							guint64 part_offset;

							part_offset = part_table_entry_get_offset (p, i);
							if (blkid_probe_set_device (pr, fd,
								vol_probe_offset + part_offset, 0) == 0 &&
							    blkid_do_safeprobe (pr) == 0)
								set_blkid_values(cs, pr);

							/* and we're done */
							break;
						}
						g_free (part_type);
					}
					

					HAL_INFO (("Done looking at part table"));
					part_table_free (p);
				}
			}

			blkid_free_probe (pr);
		}

		/* get partition type number, if we find a msdos partition table */
		if (partition_number_str != NULL && 
		    partition_number <= 256 && partition_number > 0 &&
		    partition_start > 0) {
			PartitionTable *p;

			HAL_INFO (("Loading part table"));
			p = part_table_load_from_disk (stordev_dev_file);
			if (p != NULL) {
				PartitionTable *p2;
				int entry;

				HAL_INFO (("Looking at part table"));
				part_table_find (p, partition_start, &p2, &entry);
				if (entry >= 0) {
					const char *scheme;
					char *type;
					char *label;
					char *uuid;
					char **flags;

					scheme = part_get_scheme_name (part_table_get_scheme (p2));
					type = part_table_entry_get_type (p2, entry);
					label = part_table_entry_get_label (p2, entry);
					uuid = part_table_entry_get_uuid (p2, entry);
					flags = part_table_entry_get_flags (p2, entry);

					if (type == NULL)
						type = g_strdup ("");
					if (label == NULL)
						label = g_strdup ("");
					if (uuid == NULL)
						uuid = g_strdup ("");
					if (flags == NULL) {
						flags = g_new0 (char *, 2);
						flags[0] = NULL;
					}

					libhal_changeset_set_property_string (cs, "volume.partition.scheme", scheme);
					libhal_changeset_set_property_string (cs, "volume.partition.type", type);
					libhal_changeset_set_property_string (cs, "volume.partition.label", label);
					libhal_changeset_set_property_string (cs, "volume.partition.uuid", uuid);
					libhal_changeset_set_property_strlist (cs, "volume.partition.flags", (const char **) flags);
					
					/* NOTE: We trust the type from the partition table
					 * if it explicitly got correct entries for RAID and
					 * LVM partitions.
					 *
					 * But in general it's not a good idea to trust the
					 * partition table type as many geek^Wexpert users use 
					 * FAT filesystems on type 0x83 which is Linux.
					 *
					 * For MBR, Linux RAID autodetect is 0xfd and Linux LVM is 0x8e
					 *
					 * For GPT, RAID is A19D880F-05FC-4D3B-A006-743F0F84911E and
					 * LVM is E6D6D379-F507-44C2-A23C-238F2A3DF928.
					 */
					if (
						((strcmp (scheme, "mbr") == 0 || strcmp (scheme, "embr") == 0) &&
						 (0xfd == atoi (type) || 0x8e == atoi (type)))
						||
						((strcmp (scheme, "gpt") == 0) &&
						 ((strcmp (type, "A19D880F-05FC-4D3B-A006-743F0F84911E") == 0) ||
						  (strcmp (type, "E6D6D379-F507-44C2-A23C-238F2A3DF928")) == 0)) ) {

						libhal_changeset_set_property_string (cs, "volume.fsusage", "raid");

					}

					/* see if this partition is an embedded partition table */
					if (part_table_entry_get_nested (p2, entry) != NULL) {

						libhal_changeset_set_property_string (cs, "volume.fsusage", "partitiontable");
						libhal_changeset_set_property_string (cs, "volume.fstype", "");
						libhal_changeset_set_property_string (cs, "volume.fsversion", "");
					}

					g_free (type);
					g_free (label);
					g_free (uuid);
					g_strfreev (flags);
				}

				part_table_free (p);
			}
			HAL_INFO (("Done looking at part table"));

			libhal_free_string (stordev_dev_file);
		}
	}

	/* good so far */
	ret = 0;

out:
	if (cs != NULL) {
		/* for testing...
		   char *values[4] = {"foo", "bar", "baz", NULL};
		   libhal_changeset_set_property_strlist (cs, "foo.bar", values);
		*/
		libhal_device_commit_changeset (ctx, cs, &error);
		libhal_device_free_changeset (cs);
	}


	if (fd >= 0)
		close (fd);

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return ret;

}
