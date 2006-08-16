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

#include <glib.h>
#include <libvolume_id.h>

#include "libhal/libhal.h"
#include "linux_dvd_rw_utils.h"
#include "shared.h"

static void vid_log(int priority, const char *file, int line, const char *format, ...)
{
	char log_str[1024];
	va_list args;

	va_start(args, format);
	vsnprintf(log_str, sizeof(log_str), format, args);
	dbg("%s:%i %s", file, line, log_str);
	va_end(args);
}

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

/* probe_msdos_part_table: return array of partiton type numbers */
#define BSIZE				0x200
#define MSDOS_MAGIC			"\x55\xaa"
#define MSDOS_PARTTABLE_OFFSET		0x1be
#define MSDOS_SIG_OFF			0x1fe
#define DOS_EXTENDED_PARTITION		0x05
#define LINUX_EXTENDED_PARTITION	0x85
#define WIN98_EXTENDED_PARTITION	0x0f
#define is_extended(type) \
	(type == DOS_EXTENDED_PARTITION ||	\
	 type == WIN98_EXTENDED_PARTITION ||	\
	 type == LINUX_EXTENDED_PARTITION)

static unsigned char *probe_msdos_part_table(int fd)
{
	static unsigned char partition_id_index[256];
	unsigned int partition_count;
	const uint8_t buf[BSIZE];
	int i;
	uint64_t poff;
	uint64_t plen;
	uint64_t extended = 0;
	uint64_t next;
	int limit;
	int empty = 1;
	struct msdos_partition_entry {
		uint8_t		boot_ind;
		uint8_t		head;
		uint8_t		sector;
		uint8_t		cyl;
		uint8_t		sys_ind;
		uint8_t		end_head;
		uint8_t		end_sector;
		uint8_t		end_cyl;
		uint32_t	start_sect;
		uint32_t	nr_sects;
	} __attribute__((packed)) *part;

	if (lseek(fd, 0, SEEK_SET) < 0) {
		dbg("lseek failed (%s)", strerror(errno));
		return NULL;
	}
	if (read(fd, &buf, BSIZE) < BSIZE) {
		dbg("read failed (%s)", strerror(errno));
		return NULL;
	}
	if (memcmp(&buf[MSDOS_SIG_OFF], MSDOS_MAGIC, 2) != 0)
		return NULL;

	part = (struct msdos_partition_entry*) &buf[MSDOS_PARTTABLE_OFFSET];
	/* check flags on all entries for a valid partition table */
	for (i = 0; i < 4; i++) {
		if (part[i].boot_ind != 0 &&
		    part[i].boot_ind != 0x80)
			return NULL;

		if (GINT32_FROM_LE(part[i].nr_sects) != 0)
			empty = 0;
	}
	if (empty == 1)
		return NULL;

	memset(partition_id_index, 0x00, sizeof(partition_id_index));

	for (i = 0; i < 4; i++) {
		poff = (uint64_t) GINT32_FROM_LE(part[i].start_sect) * BSIZE;
		plen = (uint64_t) GINT32_FROM_LE(part[i].nr_sects) * BSIZE;

		if (plen == 0)
			continue;

		partition_id_index[i] = part[i].sys_ind;

		if (is_extended(part[i].sys_ind)) {
			dbg("found extended partition at 0x%llx", (unsigned long long) poff);
			if (extended == 0)
				extended = poff;
		} else {
			dbg("found 0x%x primary data partition at 0x%llx, len 0x%llx",
			    part[i].sys_ind, (unsigned long long) poff, (unsigned long long) plen);
		}
	}

	/* follow extended partition chain and add data partitions */
	partition_count = 4;
	limit = 255;
	next = extended;
	while (next != 0) {
		if (limit-- == 0) {
			dbg("extended chain limit reached");
			break;
		}

		dbg("read 0x%llx (%llu)", next, next);
		if (lseek(fd, next, SEEK_SET) < 0) {
			dbg("lseek failed (%s)", strerror(errno));
			return NULL;
		}
		if (read(fd, &buf, BSIZE) < BSIZE) {
			dbg("read failed (%s)", strerror(errno));
			return NULL;
		}
		if (memcmp(&buf[MSDOS_SIG_OFF], MSDOS_MAGIC, 2) != 0)
			break;

		next = 0;

		part = (struct msdos_partition_entry*) &buf[MSDOS_PARTTABLE_OFFSET];
		for (i = 0; i < 4; i++) {
			poff = (uint64_t) GINT32_FROM_LE(part[i].start_sect) * BSIZE;
			plen = (uint64_t) GINT32_FROM_LE(part[i].nr_sects) * BSIZE;

			if (plen == 0)
				continue;

			if (is_extended(part[i].sys_ind)) {
				dbg("found extended partition (chain) at 0x%llx", (unsigned long long) poff);
				if (next == 0)
					next = extended + poff;
			} else {
				dbg("found 0x%x logical data partition at 0x%llx, len 0x%llx",
					part[i].sys_ind, (unsigned long long) poff, (unsigned long long) plen);

				partition_id_index[partition_count] = part[i].sys_ind;
				partition_count++;
			}
		}
	}

	return partition_id_index;
}

static void
set_volume_id_values (LibHalContext *ctx, const char *udi, LibHalChangeSet *changes, struct volume_id *vid)
{
	char buf[256];
	const char *usage;
	char *volume_label;
	DBusError error;

	dbus_error_init (&error);

	switch (vid->usage_id) {
	case VOLUME_ID_FILESYSTEM:
		usage = "filesystem";
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
		libhal_changeset_set_property_string (changes, "info.product", "Volume (unused)");
		usage = "unused";
		return;
	default:
		usage = "";
	}

	libhal_changeset_set_property_string (changes, "volume.fsusage", usage);
	dbg ("volume.fsusage = '%s'", usage);

	libhal_changeset_set_property_string (changes, "volume.fstype", vid->type);
	dbg ("volume.fstype = '%s'", vid->type);
	if (vid->type_version[0] != '\0') {
		libhal_changeset_set_property_string (changes, "volume.fsversion", vid->type_version);
		dbg ("volume.fsversion = '%s'", vid->type_version);
	}
	libhal_changeset_set_property_string (changes, "volume.uuid", vid->uuid);
	dbg ("volume.uuid = '%s'", vid->uuid);

	/* we need to be sure for a utf8 valid label, because dbus accept only utf8 valid strings */
	volume_label = strdup_valid_utf8 (vid->label);
	if( volume_label != NULL ) {
		libhal_changeset_set_property_string (changes, "volume.label", volume_label);
		dbg ("volume.label = '%s'", volume_label);
		
		if (strlen(volume_label) > 0) {	
			libhal_changeset_set_property_string (changes, "info.product", volume_label);
		}
		else {
			snprintf (buf, sizeof (buf), "Volume (%s)", vid->type);
			libhal_changeset_set_property_string (changes, "info.product", buf);
		}
		g_free(volume_label);
	} else {
		snprintf (buf, sizeof (buf), "Volume (%s)", vid->type);
		libhal_changeset_set_property_string (changes, "info.product", buf);
	}
}

static void
advanced_disc_detect (LibHalContext *ctx, const char *udi, LibHalChangeSet *changes,
                      int fd, const char *device_file)
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
	libhal_changeset_set_property_bool (changes, "volume.disc.is_videodvd", FALSE);
	libhal_changeset_set_property_bool (changes, "volume.disc.is_vcd", FALSE);
	libhal_changeset_set_property_bool (changes, "volume.disc.is_svcd", FALSE);
	
	/* read the block size */
	lseek (fd, 0x8080, SEEK_CUR);
	if (read (fd, &bs, 2) != 2)
	{
		dbg ("Advanced probing on %s failed while reading block size", device_file);
		goto out;
	}

	/* read in size of path table */
	lseek (fd, 2, SEEK_CUR);
	if (read (fd, &ts, 2) != 2)
	{
		dbg ("Advanced probing on %s failed while reading path table size", device_file);
		goto out;
	}

	/* read in which block path table is in */
	lseek (fd, 6, SEEK_CUR);
	if (read (fd, &tl, 4) != 4)
	{
		dbg ("Advanced probing on %s failed while reading path table block", device_file);
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
			dbg ("Advanced probing on %s failed, cannot read more entries", device_file);
			break;
		}

		/* get the record number of this entry's parent
		   i'm pretty sure that the 1st entry is always the top directory */
		lseek (fd, 5, SEEK_CUR);
		if (read (fd, &parent, 2) != 2)
		{
			dbg ("Advanced probing on %s failed, couldn't read parent entry", device_file);
			break;
		}
		
		/* read the name */
		if (read (fd, dirname, len_di) != len_di)
		{
			dbg ("Advanced probing on %s failed, couldn't read the entry name", device_file);
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
				libhal_changeset_set_property_bool (changes, "volume.disc.is_videodvd", TRUE);
				dbg ("Disc in %s is a Video DVD", device_file);
				break;
			}
			else if (!strcmp (dirname, "VCD"))
			{
				libhal_changeset_set_property_bool (changes, "volume.disc.is_vcd", TRUE);
				dbg ("Disc in %s is a Video CD", device_file);
				break;
			}
			else if (!strcmp (dirname, "SVCD"))
			{
				libhal_changeset_set_property_bool (changes, "volume.disc.is_svcd", TRUE);
				dbg ("Disc in %s is a Super Video CD", device_file);
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
	LibHalChangeSet *changeset;
	fd = -1;

	/* hook in our debug into libvolume_id */
	volume_id_log_fn = vid_log;

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

	_set_debug ();

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	changeset = libhal_device_new_changeset (udi);
	if (changeset == NULL) {
		dbg ("Cannot initialize changeset");
		goto out;
	}

	dbg ("Doing probe-volume for %s\n", device_file);

	fd = open (device_file, O_RDONLY);
	if (fd < 0)
		goto out;

	/* block size and total size */
	if (ioctl (fd, BLKSSZGET, &block_size) == 0) {
		dbg ("volume.block_size = %d", block_size);
		libhal_changeset_set_property_int (changeset, "volume.block_size", block_size);
	}
	if (ioctl (fd, BLKGETSIZE64, &vol_size) == 0) {
		dbg ("volume.size = %llu", vol_size);
		libhal_changeset_set_property_uint64 (changeset, "volume.size", vol_size);
	} else
		vol_size = 0;

	should_probe_for_fs = TRUE;

	if (is_disc) {
		int type;
		guint64 capacity;
		struct cdrom_tochdr; /* toc_hdr; */

		/* defaults */
		libhal_changeset_set_property_string (changeset, "volume.disc.type", "unknown");
		libhal_changeset_set_property_bool (changeset, "volume.disc.has_audio", FALSE);
		libhal_changeset_set_property_bool (changeset, "volume.disc.has_data", FALSE);
		libhal_changeset_set_property_bool (changeset, "volume.disc.is_blank", FALSE);
		libhal_changeset_set_property_bool (changeset, "volume.disc.is_appendable", FALSE);
		libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", FALSE);

		/* Suggested by Alex Larsson to get rid of log spewage
		 * on Alan's cd changer (RH bug 130649) */
		if (ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) != CDS_DISC_OK) {
			goto out;
		}

		/* check for audio/data/blank */
		type = ioctl (fd, CDROM_DISC_STATUS, CDSL_CURRENT);
		switch (type) {
		case CDS_AUDIO:		/* audio CD */
			libhal_changeset_set_property_bool (changeset, "volume.disc.has_audio", TRUE);
			dbg ("Disc in %s has audio", device_file);
			should_probe_for_fs = FALSE;
			break;
		case CDS_MIXED:		/* mixed mode CD */
			libhal_changeset_set_property_bool (changeset, "volume.disc.has_audio", TRUE);
			libhal_changeset_set_property_bool (changeset, "volume.disc.has_data", TRUE);
			dbg ("Disc in %s has audio+data", device_file);
			break;
		case CDS_DATA_1:	/* data CD */
		case CDS_DATA_2:
		case CDS_XA_2_1:
		case CDS_XA_2_2:
			libhal_changeset_set_property_bool (changeset, "volume.disc.has_data", TRUE);
			dbg ("Disc in %s has data", device_file);
			advanced_disc_detect (ctx, udi, changeset, fd, device_file);
			break;
		case CDS_NO_INFO:	/* blank or invalid CD */
			libhal_changeset_set_property_bool (changeset, "volume.disc.is_blank", TRUE);
			/* set the volume size to 0 if disc is blank and not as 4 from BLKGETSIZE64 */
			libhal_changeset_set_property_int (changeset, "volume.block_size", 0);
			dbg ("Disc in %s is blank", device_file);
			should_probe_for_fs = FALSE;
			break;
			
		default:		/* should never see this */
			libhal_changeset_set_property_string (changeset, "volume.disc_type", "unknown");
			dbg ("Disc in %s returned unknown CDROM_DISC_STATUS", device_file);
			should_probe_for_fs = FALSE;
			break;
		}
		
		/* see table 87 - Profile List in MMC-5 for details on disc type
		 * http://www.t10.org/drafts.htm#mmc5
		 */
		type = get_disc_type (fd);
		dbg ("get_disc_type returned 0x%02x", type);
		if (type != -1) {
			switch (type) {
			case 0x08: /* CD-ROM */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "cd_rom");
				break;
			case 0x09: /* CD-R */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "cd_r");
				break;
			case 0x0a: /* CD-RW */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "cd_rw");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x10: /* DVD-ROM */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_rom");
				break;
			case 0x11: /* DVD-R Sequential */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_r");
				break;
			case 0x12: /* DVD-RAM */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_ram");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x13: /* DVD-RW Restricted Overwrite */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_rw");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x14: /* DVD-RW Sequential */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_rw");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x1A: /* DVD+RW */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_plus_rw");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x1B: /* DVD+R */
				libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_plus_r");
				break;
			case 0x2B: /* DVD+R Double Layer */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "dvd_plus_r_dl");
				break;
			case 0x40: /* BD-ROM  */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "bd_rom");
				break;
			case 0x41: /* BD-R Sequential */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "bd_r");
				break;
			case 0x42: /* BD-R Random */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "bd_r");
				break;
			case 0x43: /* BD-RE */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "bd_re");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			case 0x50: /* HD DVD-ROM */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "hddvd_rom");
				break;
			case 0x51: /* HD DVD-R */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "hddvd_r");
				break;
			case 0x52: /* HD DVD-Rewritable */
                          	libhal_changeset_set_property_string (changeset, "volume.disc.type", "hddvd_rw");
				libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
				break;
			default: 
				break;
			}
		}

		if (get_disc_capacity_for_type (fd, type, &capacity) == 0) {
			dbg ("volume.disc.capacity = %llu", capacity);
			libhal_changeset_set_property_uint64 (changeset, "volume.disc.capacity", capacity);
		}

		/* On some hardware the get_disc_type call fails, so we use this as a backup */
		if (disc_is_rewritable (fd)) {
			libhal_changeset_set_property_bool (changeset, "volume.disc.is_rewritable", TRUE);
		}
		
		if (disc_is_appendable (fd)) {
			libhal_changeset_set_property_bool (changeset, "volume.disc.is_appendable", TRUE);
		}

#if 0
		/* This seems to cause problems on some drives with broken firmware,
		 * comment it out until we really need multisession support */

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
#endif

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
				set_volume_id_values(ctx, udi, changeset, vid);
			} else {
				libhal_changeset_set_property_string (changeset, "info.product", "Volume");
			}
			volume_id_close(vid);
		}

		/* get partition type number, if we find a msdos partition table */
		if (partition_number_str != NULL) {
			unsigned char *idx;
			int fd;

			if ((stordev_dev_file = libhal_device_get_property_string (
					ctx, parent_udi, "block.device", &error)) == NULL) {
				goto out;
			}
			fd = open(stordev_dev_file, O_RDONLY);
			if (fd >= 0) {
				idx = probe_msdos_part_table(fd);
				if (idx != NULL) {
					unsigned char type;

					type = idx[partition_number - 1];
					if (type > 0) {
						libhal_changeset_set_property_int (
							changeset, "volume.partition.msdos_part_table_type", type);

						/* NOTE: We trust the type from the partition table
						 * if it explicitly got correct entries for RAID and
						 * LVM partitions.
						 *
						 * But in general it's not a good idea to trust the
						 * partition table type as many geek^Wexpert users use 
						 * FAT filesystems on type 0x83 which is Linux.
						 *
						 * Linux RAID autodetect is 0xfd and Linux LVM is 0x8e
						 */
						if (type == 0xfd || type == 0x8e ) {
							libhal_changeset_set_property_string (
								changeset, "volume.fsusage", "raid");
						}
					}
				}
				close (fd);
			}
			libhal_free_string (stordev_dev_file);
		}
	}

	/* good so far */
	ret = 0;

	/* for testing...
	  char *values[4] = {"foo", "bar", "baz", NULL};
	  libhal_changeset_set_property_strlist (changeset, "foo.bar", values);
	*/

	libhal_device_commit_changeset (ctx, changeset, &error);
	libhal_device_free_changeset (changeset);

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
