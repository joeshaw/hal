/*
 * volume_id - reads filesystem label and uuid
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 *
 *	The superblock structs are taken from the linux kernel sources
 *	and the libblkid living inside the e2fsprogs. This is a simple
 *	straightforward implementation for reading the label strings of the
 *	most common filesystems.
 *
 *	This library is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU Lesser General Public
 *	License as published by the Free Software Foundation; either
 *	version 2.1 of the License, or (at your option) any later version.
 *
 *	This library is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *	Lesser General Public License for more details.
 *
 *	You should have received a copy of the GNU Lesser General Public
 *	License along with this library; if not, write to the Free Software
 *	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <asm/types.h>

#include "volume_id.h"

#ifdef DEBUG
#define dbg(format, arg...)						\
	do {								\
		printf("%s: " format "\n", __FUNCTION__ , ## arg);	\
	} while (0)
#else
#define dbg(format, arg...)	do {} while (0)
#endif /* DEBUG */

#define bswap16(x) (__u16)((((__u16)(x) & 0x00ffu) << 8) | \
			   (((__u32)(x) & 0xff00u) >> 8))

#define bswap32(x) (__u32)((((__u32)(x) & 0xff000000u) >> 24) | \
			   (((__u32)(x) & 0x00ff0000u) >>  8) | \
			   (((__u32)(x) & 0x0000ff00u) <<  8) | \
			   (((__u32)(x) & 0x000000ffu) << 24))

#define bswap64(x) (__u64)((((__u64)(x) & 0xff00000000000000u) >> 56) | \
			   (((__u64)(x) & 0x00ff000000000000u) >> 40) | \
			   (((__u64)(x) & 0x0000ff0000000000u) >> 24) | \
			   (((__u64)(x) & 0x000000ff00000000u) >>  8) | \
			   (((__u64)(x) & 0x00000000ff000000u) <<  8) | \
			   (((__u64)(x) & 0x0000000000ff0000u) << 24) | \
			   (((__u64)(x) & 0x000000000000ff00u) << 40) | \
			   (((__u64)(x) & 0x00000000000000ffu) << 56))

#if (__BYTE_ORDER == __LITTLE_ENDIAN) 
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)
#define be16_to_cpu(x) bswap16(x)
#define be32_to_cpu(x) bswap32(x)
#elif (__BYTE_ORDER == __BIG_ENDIAN)
#define le16_to_cpu(x) bswap16(x)
#define le32_to_cpu(x) bswap32(x)
#define le64_to_cpu(x) bswap64(x)
#define be16_to_cpu(x) (x)
#define be32_to_cpu(x) (x)
#endif

/* size of superblock buffer, reiser block is at 64k */
#define SB_BUFFER_SIZE				0x11000
/* size of seek buffer 4k */
#define SEEK_BUFFER_SIZE			0x1000


static void set_label_raw(struct volume_id *id,
			  const __u8 *buf, unsigned int count)
{
	memcpy(id->label_raw, buf, count);
	id->label_raw_len = count;
}

static void set_label_string(struct volume_id *id,
			     const __u8 *buf, unsigned int count)
{
	unsigned int i;

	memcpy(id->label, buf, count);

	/* remove trailing whitespace */
	i = strnlen(id->label, count);
	while (i--) {
		if (! isspace(id->label[i]))
			break;
	}
	id->label[i+1] = '\0';
}

#define LE		0
#define BE		1
static void set_label_unicode16(struct volume_id *id,
				const __u8 *buf,
				unsigned int endianess,
				unsigned int count)
{
	unsigned int i, j;
	__u16 c;

	j = 0;
	for (i = 0; i + 2 <= count; i += 2) {
		if (endianess == LE)
			c = (buf[i+1] << 8) | buf[i];
		else
			c = (buf[i] << 8) | buf[i+1];
		if (c == 0) {
			id->label[j] = '\0';
			break;
		} else if (c < 0x80) {
			id->label[j++] = (__u8) c;
		} else if (c < 0x800) {
			id->label[j++] = (__u8) (0xc0 | (c >> 6));
			id->label[j++] = (__u8) (0x80 | (c & 0x3f));
		} else {
			id->label[j++] = (__u8) (0xe0 | (c >> 12));
			id->label[j++] = (__u8) (0x80 | ((c >> 6) & 0x3f));
			id->label[j++] = (__u8) (0x80 | (c & 0x3f));
		}
	}
}

static void set_uuid(struct volume_id *id,
		     const __u8 *buf, unsigned int count)
{
	unsigned int i;

	memcpy(id->uuid_raw, buf, count);

	/* create string if uuid is set */
	for (i = 0; i < count; i++) 
		if (buf[i] != 0)
			goto set;
	return;

set:
	switch(count) {
	case 4:
		sprintf(id->uuid, "%02X%02X-%02X%02X",
			buf[3], buf[2], buf[1], buf[0]);
		break;
	case 16:
		sprintf(id->uuid,
			"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
			"%02x%02x%02x%02x%02x%02x",
			buf[0], buf[1], buf[2], buf[3],
			buf[4], buf[5],
			buf[6], buf[7],
			buf[8], buf[9],
			buf[10], buf[11], buf[12], buf[13], buf[14],buf[15]);
		break;
	}
}

static __u8 *get_buffer(struct volume_id *id, __u64 off, unsigned int len)
{
	unsigned int buf_len;

	dbg("get buffer off 0x%llx, len 0x%x", off, len);
	/* check if requested area fits in superblock buffer */
	if (off + len <= SB_BUFFER_SIZE) {
		if (id->sbbuf == NULL) {
			id->sbbuf = malloc(SB_BUFFER_SIZE);
			if (id->sbbuf == NULL)
				return NULL;
		}

		/* check if we need to read */
		if ((off + len) > id->sbbuf_len) {
			dbg("read sbbuf len:0x%llx", off + len);
			lseek(id->fd, 0, SEEK_SET);
			buf_len = read(id->fd, id->sbbuf, off + len);
			dbg("got 0x%x (%i) bytes", buf_len, buf_len);
			id->sbbuf_len = buf_len;
			if (buf_len < off + len)
				return NULL;
		}

		return &(id->sbbuf[off]);
	} else {
		if (len > SEEK_BUFFER_SIZE)
			len = SEEK_BUFFER_SIZE;

		/* get seek buffer */
		if (id->seekbuf == NULL) {
			id->seekbuf = malloc(SEEK_BUFFER_SIZE);
			if (id->seekbuf == NULL)
				return NULL;
		}

		/* check if we need to read */
		if ((off < id->seekbuf_off) ||
		    ((off + len) > (id->seekbuf_off + id->seekbuf_len))) {
			dbg("read seekbuf off:0x%llx len:0x%x", off, len);
			if (lseek(id->fd, off, SEEK_SET) == -1)
				return NULL;
			buf_len = read(id->fd, id->seekbuf, len);
			dbg("got 0x%x (%i) bytes", buf_len, buf_len);
			id->seekbuf_off = off;
			id->seekbuf_len = buf_len;
			if (buf_len < len)
				return NULL;
		}

		return &(id->seekbuf[off - id->seekbuf_off]);
	}
}

static void free_buffer(struct volume_id *id)
{
	if (id->sbbuf != NULL) {
		free(id->sbbuf);
		id->sbbuf = NULL;
		id->sbbuf_len = 0;
	}
	if (id->seekbuf != NULL) {
		free(id->seekbuf);
		id->seekbuf = NULL;
		id->seekbuf_len = 0;
	}
}

#define MSDOS_MAGIC		"\x55\xaa"
static int probe_msdos_part_table(struct volume_id *id, __u64 off)
{
	struct msdos_partition_entry {
		__u8	boot_ind;
		__u8	head;
		__u8	sector;
		__u8	cyl;
		__u8	sys_ind;
		__u8	end_head;
		__u8	end_sector;
		__u8	end_cyl;
		__u32	start_sect;
		__u32	nr_sects;
	} __attribute__((packed)) *part;

	const __u8 *buf;
	unsigned int bsize = 0x200;
	unsigned int part_count = 4;
	int i;

	buf = get_buffer(id, off, 0x200);
	if (buf == NULL)
		return -1;

	if (strncmp(&buf[510], MSDOS_MAGIC, 2) != 0)
		return -1;

	/* check flags on all entries to be sure to have partition table */
	part = (struct msdos_partition_entry*) &buf[0x1be];
	for (i = 0; i < 4; i++) {
		if (part[i].boot_ind != 0 &&
		    part[i].boot_ind != 0x80)
			return -1;
	}

	if (id->partitions != NULL)
		free(id->partitions);
	id->partitions =
		malloc(part_count * sizeof(struct volume_id_partition));
	if (id->partitions == NULL)
		return -1;
	memset(id->partitions, 0x00, sizeof(struct volume_id_partition));

	for (i = 0; i < 4; i++) {
		__u64 poff;
		__u64 plen;

		poff = (__u64) le32_to_cpu(part[i].start_sect) * bsize;
		plen = (__u64) le32_to_cpu(part[i].nr_sects) * bsize;

		dbg("found 0x%x partition entry at 0x%llx, len 0x%llx",
		    part[i].sys_ind, poff, plen);

		if (plen == 0)
			continue;

		/* FIXME: parse logical partition and add entries */

		id->partitions[i].off = poff;
		id->partitions[i].len = plen;
	}
	id->partition_count = part_count;

	id->type_id = VOLUME_ID_PARTITIONTABLE;
	id->format_id = VOLUME_ID_MSDOSPARTTABLE;
	id->format = "msdos_partition_table";

	return 0;
}

#define EXT3_FEATURE_COMPAT_HAS_JOURNAL		0x00000004
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV	0x00000008
#define EXT_SUPERBLOCK_OFFSET			0x400
static int probe_ext(struct volume_id *id, __u64 off)
{
	struct ext2_super_block {
		__u32	inodes_count;
		__u32	blocks_count;
		__u32	r_blocks_count;
		__u32	free_blocks_count;
		__u32	free_inodes_count;
		__u32	first_data_block;
		__u32	log_block_size;
		__u32	dummy3[7];
		__u8	magic[2];
		__u16	state;
		__u32	dummy5[8];
		__u32	feature_compat;
		__u32	feature_incompat;
		__u32	feature_ro_compat;
		__u8	uuid[16];
		__u8	volume_name[16];
	} __attribute__((__packed__)) *es;

	es = (struct ext2_super_block *)
	     get_buffer(id, off + EXT_SUPERBLOCK_OFFSET, 0x200);
	if (es == NULL)
		return -1;

	if (es->magic[0] != 0123 ||
	    es->magic[1] != 0357)
		return -1;

	set_label_raw(id, es->volume_name, 16);
	set_label_string(id, es->volume_name, 16);
	set_uuid(id, es->uuid, 16);

	if ((le32_to_cpu(es->feature_compat) &
	     EXT3_FEATURE_COMPAT_HAS_JOURNAL) != 0) {
		id->type_id = VOLUME_ID_FILESYSTEM;
		id->format_id = VOLUME_ID_EXT3;
		id->format = "ext3";
	} else {
		id->type_id = VOLUME_ID_FILESYSTEM;
		id->format_id = VOLUME_ID_EXT2;
		id->format = "ext2";
	}

	return 0;
}

#define REISER1_SUPERBLOCK_OFFSET		0x2000
#define REISER_SUPERBLOCK_OFFSET		0x10000
static int probe_reiser(struct volume_id *id, __u64 off)
{
	struct reiser_super_block {
		__u32	blocks_count;
		__u32	free_blocks;
		__u32	root_block;
		__u32	journal_block;
		__u32	journal_dev;
		__u32	orig_journal_size;
		__u32	dummy2[5];
		__u16	blocksize;
		__u16	dummy3[3];
		__u8	magic[12];
		__u32	dummy4[5];
		__u8	uuid[16];
		__u8	label[16];
	} __attribute__((__packed__)) *rs;

	rs = (struct reiser_super_block *)
	     get_buffer(id, off + REISER_SUPERBLOCK_OFFSET, 0x200);
	if (rs == NULL)
		return -1;

	if (strncmp(rs->magic, "ReIsEr2Fs", 9) == 0)
		goto found;
	if (strncmp(rs->magic, "ReIsEr3Fs", 9) == 0)
		goto found;

	rs = (struct reiser_super_block *)
	     get_buffer(id, off + REISER1_SUPERBLOCK_OFFSET, 0x200);
	if (rs == NULL)
		return -1;

	if (strncmp(rs->magic, "ReIsErFs", 8) == 0)
		goto found;

	return -1;

found:
	set_label_raw(id, rs->label, 16);
	set_label_string(id, rs->label, 16);
	set_uuid(id, rs->uuid, 16);

	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_REISER;
	id->format = "reiserfs";

	return 0;
}

static int probe_xfs(struct volume_id *id, __u64 off)
{
	struct xfs_super_block {
		__u8	magic[4];
		__u32	blocksize;
		__u64	dblocks;
		__u64	rblocks;
		__u32	dummy1[2];
		__u8	uuid[16];
		__u32	dummy2[15];
		__u8	fname[12];
		__u32	dummy3[2];
		__u64	icount;
		__u64	ifree;
		__u64	fdblocks;
	} __attribute__((__packed__)) *xs;

	xs = (struct xfs_super_block *) get_buffer(id, off, 0x200);
	if (xs == NULL)
		return -1;

	if (strncmp(xs->magic, "XFSB", 4) != 0)
		return -1;

	set_label_raw(id, xs->fname, 12);
	set_label_string(id, xs->fname, 12);
	set_uuid(id, xs->uuid, 16);

	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_XFS;
	id->format = "xfs";

	return 0;
}

#define JFS_SUPERBLOCK_OFFSET			0x8000
static int probe_jfs(struct volume_id *id, __u64 off)
{
	struct jfs_super_block {
		__u8	magic[4];
		__u32	version;
		__u64	size;
		__u32	bsize;
		__u32	dummy1;
		__u32	pbsize;
		__u32	dummy2[27];
		__u8	uuid[16];
		__u8	label[16];
		__u8	loguuid[16];
	} __attribute__((__packed__)) *js;

	js = (struct jfs_super_block *)
	     get_buffer(id, off + JFS_SUPERBLOCK_OFFSET, 0x200);
	if (js == NULL)
		return -1;

	if (strncmp(js->magic, "JFS1", 4) != 0)
		return -1;

	set_label_raw(id, js->label, 16);
	set_label_string(id, js->label, 16);
	set_uuid(id, js->uuid, 16);

	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_JFS;
	id->format = "jfs";

	return 0;
}

static int probe_vfat(struct volume_id *id, __u64 off)
{
	struct vfat_super_block {
		__u8	ignored[3];
		__u8	sysid[8];
		__u8	sector_size[2];
		__u8	cluster_size;
		__u16	reserved;
		__u8	fats;
		__u8	dir_entries[2];
		__u8	sectors[2];
		__u8	media;
		__u16	fat_length;
		__u16	secs_track;
		__u16	heads;
		__u32	hidden;
		__u32	total_sect;
		__u32	fat32_length;
		__u16	flags;
		__u8	version[2];
		__u32	root_cluster;
		__u16	insfo_sector;
		__u16	backup_boot;
		__u16	reserved2[6];
		__u8	unknown[3];
		__u8	serno[4];
		__u8	label[11];
		__u8	magic[8];
		__u8	dummy2[164];
		__u8	pmagic[2];
	} __attribute__((__packed__)) *vs;

	vs = (struct vfat_super_block *) get_buffer(id, off, 0x200);
	if (vs == NULL)
		return -1;

	if (strncmp(vs->magic, "MSWIN", 5) == 0)
		goto found;
	if (strncmp(vs->magic, "FAT32   ", 8) == 0)
		goto found;
	return -1;

found:
	set_label_raw(id, vs->label, 11);
	set_label_string(id, vs->label, 11);
	set_uuid(id, vs->serno, 4);

	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_VFAT;
	id->format = "vfat";

	return 0;
}

static int probe_msdos(struct volume_id *id, __u64 off)
{
	struct msdos_super_block {
		__u8	boot_jump[3];
		__u8	sysid[8];
		__u16	sector_size;
		__u8	cluster_size;
		__u16	reserved;
		__u8	fats;
		__u8	dir_entries[2];
		__u8	sectors[2];
		__u8	media;
		__u16	fat_length;
		__u16	secs_track;
		__u16	heads;
		__u32	hidden;
		__u32	total_sect;
		__u8	unknown[3];
		__u8	serno[4];
		__u8	label[11];
		__u8	magic[8];
		__u8	dummy2[192];
		__u8	pmagic[2];
	} __attribute__((__packed__)) *ms;

	unsigned int	sector_size;

	ms = (struct msdos_super_block *) get_buffer(id, off, 0x200);
	if (ms == NULL)
		return -1;

	if (strncmp(ms->magic, "MSDOS", 5) == 0)
		goto found;
	if (strncmp(ms->magic, "FAT16   ", 8) == 0)
		goto found;
	if (strncmp(ms->magic, "FAT12   ", 8) == 0)
		goto found;
	/*
	 * Bad, not finished. There are old floppies out there without a magic,
 	 * so we check for well known values and guess if it's a msdos volume
	 */

	/* boot jump address check */
	if ((ms->boot_jump[0] != 0xeb || ms->boot_jump[2] != 0x90) &&
	     ms->boot_jump[0] != 0xe9)
		return -1;
	/* heads check */
	if (ms->heads == 0)
		return -1;
	/* sector size check */
	sector_size = le16_to_cpu(ms->sector_size);
	if (sector_size != 0x200 && sector_size != 0x400 &&
	    sector_size != 0x800 && sector_size != 0x1000)
		return -1;
	/* cluster size check*/	
	if (ms->cluster_size == 0 || (ms->cluster_size & (ms->cluster_size-1)))
		return -1;
	/* media check */
	if (ms->media < 0xf8 && ms->media != 0xf0)
		return -1;
	/* fat count*/
	if (ms->fats != 2)
		return -1;

found:
	set_label_raw(id, ms->label, 11);
	set_label_string(id, ms->label, 11);
	set_uuid(id, ms->serno, 4);

	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_MSDOS;
	id->format = "msdos";

	return 0;
}

#define UDF_VSD_OFFSET			0x8000
static int probe_udf(struct volume_id *id, __u64 off)
{
	struct volume_descriptor {
		struct descriptor_tag {
			__u16	id;
			__u16	version;
			__u8	checksum;
			__u8	reserved;
			__u16	serial;
			__u16	crc;
			__u16	crc_len;
			__u32	location;
		} __attribute__((__packed__)) tag;
		union {
			struct anchor_descriptor {
				__u32	length;
				__u32	location;
			} __attribute__((__packed__)) anchor;
			struct primary_descriptor {
				__u32	seq_num;
				__u32	desc_num;
				struct dstring {
					__u8	clen;
					__u8	c[31];
				} __attribute__((__packed__)) ident;
			} __attribute__((__packed__)) primary;
		} __attribute__((__packed__)) type;
	} __attribute__((__packed__)) *vd;

	struct volume_structure_descriptor {
		__u8	type;
		__u8	id[5];
		__u8	version;
	} *vsd;

	unsigned int bs;
	unsigned int b;
	unsigned int type;
	unsigned int count;
	unsigned int loc;
	unsigned int clen;

	vsd = (struct volume_structure_descriptor *)
	      get_buffer(id, off + UDF_VSD_OFFSET, 0x200);
	if (vsd == NULL)
		return -1;

	if (strncmp(vsd->id, "NSR02", 5) == 0)
		goto blocksize;
	if (strncmp(vsd->id, "NSR03", 5) == 0)
		goto blocksize;
	if (strncmp(vsd->id, "BEA01", 5) == 0)
		goto blocksize;
	if (strncmp(vsd->id, "BOOT2", 5) == 0)
		goto blocksize;
	if (strncmp(vsd->id, "CD001", 5) == 0)
		goto blocksize;
	if (strncmp(vsd->id, "CDW02", 5) == 0)
		goto blocksize;
	if (strncmp(vsd->id, "TEA03", 5) == 0)
		goto blocksize;
	return -1;

blocksize:
	/* search the next VSD to get the logical block size of the volume */
	for (bs = 0x800; bs < 0x8000; bs += 0x800) {
		vsd = (struct volume_structure_descriptor *)
		      get_buffer(id, off + UDF_VSD_OFFSET + bs, 0x800);
		if (vsd == NULL)
			return -1;
		dbg("test for blocksize: 0x%x", bs);
		if (vsd->id[0] != '\0')
			goto nsr;
	}
	return -1;

nsr:
	/* search the list of VSDs for a NSR descriptor */
	for (b = 0; b < 64; b++) {
		vsd = (struct volume_structure_descriptor *)
		      get_buffer(id, off + UDF_VSD_OFFSET + (b * bs), 0x800);
		if (vsd == NULL)
			return -1;

		dbg("vsd: %c%c%c%c%c",
		    vsd->id[0], vsd->id[1], vsd->id[2], vsd->id[3], vsd->id[4]);

		if (vsd->id[0] == '\0')
			return -1;
		if (strncmp(vsd->id, "NSR02", 5) == 0)
			goto anchor;
		if (strncmp(vsd->id, "NSR03", 5) == 0)
			goto anchor;
	}
	return -1;

anchor:
	/* read anchor volume descriptor */
	vd = (struct volume_descriptor *)
		get_buffer(id, off + (256 * bs), 0x200);
	if (vd == NULL)
		return -1;

	type = le16_to_cpu(vd->tag.id);
	if (type != 2) /* TAG_ID_AVDP */
		goto found;

	/* get desriptor list address and block count */
	count = le32_to_cpu(vd->type.anchor.length) / bs;
	loc = le32_to_cpu(vd->type.anchor.location);
	dbg("0x%x descriptors starting at logical secor 0x%x", count, loc);

	/* pick the primary descriptor from the list */
	for (b = 0; b < count; b++) {
		vd = (struct volume_descriptor *)
		     get_buffer(id, off + ((loc + b) * bs), 0x200);
		if (vd == NULL)
			return -1;

		type = le16_to_cpu(vd->tag.id);
		dbg("descriptor type %i", type);

		/* check validity */
		if (type == 0)
			goto found;
		if (le32_to_cpu(vd->tag.location) != loc + b)
			goto found;

		if (type == 1) /* TAG_ID_PVD */
			goto pvd;
	}
	goto found;

pvd:
	set_label_raw(id, &(vd->type.primary.ident.clen), 32);

	clen = vd->type.primary.ident.clen;
	dbg("label string charsize=%i bit", clen);
	if (clen == 8) 
		set_label_string(id, vd->type.primary.ident.c, 31);
	else if (clen == 16)
		set_label_unicode16(id, vd->type.primary.ident.c, BE,31);

found:
	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_UDF;
	id->format = "udf";

	return 0;
}

#define ISO_SUPERBLOCK_OFFSET		0x8000
static int probe_iso9660(struct volume_id *id, __u64 off)
{
	union iso_super_block {
		struct iso_header {
			__u8	type;
			__u8	id[5];
			__u8	version;
			__u8	unused1;
			__u8		system_id[32];
			__u8		volume_id[32];
		} __attribute__((__packed__)) iso;
		struct hs_header {
			__u8	foo[8];
			__u8	type;
			__u8	id[4];
			__u8	version;
		} __attribute__((__packed__)) hs;
	} __attribute__((__packed__)) *is;

	is = (union iso_super_block *)
	     get_buffer(id, off + ISO_SUPERBLOCK_OFFSET, 0x200);
	if (is == NULL)
		return -1;

	if (strncmp(is->iso.id, "CD001", 5) == 0) {
		set_label_raw(id, is->iso.volume_id, 32);
		set_label_string(id, is->iso.volume_id, 32);
		goto found;
	}
	if (strncmp(is->hs.id, "CDROM", 5) == 0)
		goto found;
	return -1;

found:
	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_ISO9660;
	id->format = "iso9660";

	return 0;
}

#define UFS_MAGIC			0x00011954
#define UFS2_MAGIC			0x19540119
#define UFS_MAGIC_FEA			0x00195612
#define UFS_MAGIC_LFN			0x00095014


static int probe_ufs(struct volume_id *id, __u64 off)
{
	struct ufs_super_block {
		__u32	fs_link;
		__u32	fs_rlink;
		__u32	fs_sblkno;
		__u32	fs_cblkno;
		__u32	fs_iblkno;
		__u32	fs_dblkno;
		__u32	fs_cgoffset;
		__u32	fs_cgmask;
		__u32	fs_time;
		__u32	fs_size;
		__u32	fs_dsize;
		__u32	fs_ncg;	
		__u32	fs_bsize;
		__u32	fs_fsize;
		__u32	fs_frag;
		__u32	fs_minfree;
		__u32	fs_rotdelay;
		__u32	fs_rps;	
		__u32	fs_bmask;
		__u32	fs_fmask;
		__u32	fs_bshift;
		__u32	fs_fshift;
		__u32	fs_maxcontig;
		__u32	fs_maxbpg;
		__u32	fs_fragshift;
		__u32	fs_fsbtodb;
		__u32	fs_sbsize;
		__u32	fs_csmask;
		__u32	fs_csshift;
		__u32	fs_nindir;
		__u32	fs_inopb;
		__u32	fs_nspf;
		__u32	fs_optim;
		__u32	fs_npsect_state;
		__u32	fs_interleave;
		__u32	fs_trackskew;
		__u32	fs_id[2];
		__u32	fs_csaddr;
		__u32	fs_cssize;
		__u32	fs_cgsize;
		__u32	fs_ntrak;
		__u32	fs_nsect;
		__u32	fs_spc;	
		__u32	fs_ncyl;
		__u32	fs_cpg;
		__u32	fs_ipg;
		__u32	fs_fpg;
		struct ufs_csum {
			__u32	cs_ndir;
			__u32	cs_nbfree; 
			__u32	cs_nifree;
			__u32	cs_nffree;
		} __attribute__((__packed__)) fs_cstotal;
		__s8	fs_fmod;
		__s8	fs_clean;
		__s8	fs_ronly;
		__s8	fs_flags;
		union {
			struct {
				__s8	fs_fsmnt[512];
				__u32	fs_cgrotor;
				__u32	fs_csp[31];
				__u32	fs_maxcluster;
				__u32	fs_cpc;
				__u16	fs_opostbl[16][8];
			} __attribute__((__packed__)) fs_u1;
			struct {
				__s8  fs_fsmnt[468];
				__u8   fs_volname[32];
				__u64  fs_swuid;
				__s32  fs_pad;
				__u32   fs_cgrotor;
				__u32   fs_ocsp[28];
				__u32   fs_contigdirs;
				__u32   fs_csp;	
				__u32   fs_maxcluster;
				__u32   fs_active;
				__s32   fs_old_cpc;
				__s32   fs_maxbsize;
				__s64   fs_sparecon64[17];
				__s64   fs_sblockloc;
				struct  ufs2_csum_total {
					__u64	cs_ndir;
					__u64	cs_nbfree;
					__u64	cs_nifree;
					__u64	cs_nffree;
					__u64	cs_numclusters;
					__u64	cs_spare[3];
				} __attribute__((__packed__)) fs_cstotal;
				struct  ufs_timeval {
					__s32	tv_sec;
					__s32	tv_usec;
				} __attribute__((__packed__)) fs_time;
				__s64    fs_size;
				__s64    fs_dsize;
				__u64    fs_csaddr;
				__s64    fs_pendingblocks;
				__s32    fs_pendinginodes;
			} __attribute__((__packed__)) fs_u2;
		}  fs_u11;
		union {
			struct {
				__s32	fs_sparecon[53];
				__s32	fs_reclaim;
				__s32	fs_sparecon2[1];
				__s32	fs_state;
				__u32	fs_qbmask[2];
				__u32	fs_qfmask[2];
			} __attribute__((__packed__)) fs_sun;
			struct {
				__s32	fs_sparecon[53];
				__s32	fs_reclaim;
				__s32	fs_sparecon2[1];
				__u32	fs_npsect;
				__u32	fs_qbmask[2];
				__u32	fs_qfmask[2];
			} __attribute__((__packed__)) fs_sunx86;
			struct {
				__s32	fs_sparecon[50];
				__s32	fs_contigsumsize;
				__s32	fs_maxsymlinklen;
				__s32	fs_inodefmt;
				__u32	fs_maxfilesize[2];
				__u32	fs_qbmask[2];
				__u32	fs_qfmask[2];
				__s32	fs_state;
			} __attribute__((__packed__)) fs_44;
		} fs_u2;
		__s32	fs_postblformat;
		__s32	fs_nrpos;
		__s32	fs_postbloff;
		__s32	fs_rotbloff;
		__u32	fs_magic;
		__u8	fs_space[1];
	} __attribute__((__packed__)) *ufs;
	
	__u32	magic;
	int 	i;
	int	offsets[] = {0, 8, 64, 256, -1};

	for (i = 0; offsets[i] >= 0; i++) {	
		ufs = (struct ufs_super_block *)
			get_buffer(id, off + (offsets[i] * 0x400), 0x800);
		if (ufs == NULL)
			return -1;

		dbg("offset 0x%x", offsets[i] * 0x400);
		magic = be32_to_cpu(ufs->fs_magic);
		if ((magic == UFS_MAGIC) ||
		    (magic == UFS2_MAGIC) ||
		    (magic == UFS_MAGIC_FEA) ||
		    (magic == UFS_MAGIC_LFN)) {
			dbg("magic 0x%08x(be)", magic);
			goto found;
		}
		magic = le32_to_cpu(ufs->fs_magic);
		if ((magic == UFS_MAGIC) ||
		    (magic == UFS2_MAGIC) ||
		    (magic == UFS_MAGIC_FEA) ||
		    (magic == UFS_MAGIC_LFN)) {
			dbg("magic 0x%08x(le)", magic);
			goto found;
		}
	}
	return -1;

found:
	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_UFS;
	id->format = "ufs";

	return 0;
}

static int probe_mac_partition_map(struct volume_id *id, __u64 off)
{
	struct mac_driver_desc { 
		__u8	signature[2];
		__u16	block_size;
		__u32	block_count;
	} __attribute__((__packed__)) *driver;

	struct mac_partition {
		__u8	signature[2];
		__u16	res1;
		__u32	map_count;
		__u32	start_block;
		__u32	block_count;
		__u8	name[32];
		__u8	type[32];
	} __attribute__((__packed__)) *part;

	const __u8 *buf;

	buf = get_buffer(id, off, 0x200);
	if (buf == NULL)
		return -1;

	part = (struct mac_partition *) buf;
	if ((strncmp(part->signature, "PM", 2) == 0) &&
	    (strncmp(part->type, "Apple_partition_map", 19) == 0)) {
		/* linux creates a own subdevice for the map
		 * just return the type if the drive header is missing */
		id->type_id = VOLUME_ID_PARTITIONTABLE;
		id->format_id = VOLUME_ID_MACPARTMAP;
		id->format = "mac_partition_map";
		return 0;
	}

	driver = (struct mac_driver_desc *) buf;
	if (strncmp(driver->signature, "ER", 2) == 0) {
		/* we are on a main device, like a CD
		 * just try to probe the first partition from the map */
		unsigned int bsize = be16_to_cpu(driver->block_size);
		int part_count;
		int i;

		/* get first entry of partition table */
		buf = get_buffer(id, off +  bsize, 0x200);
		if (buf == NULL)
			return -1;

		part = (struct mac_partition *) buf;
		if (strncmp(part->signature, "PM", 2) != 0)
			return -1;

		part_count = be32_to_cpu(part->map_count);
		dbg("expecting %d partition entries", part_count);

		if (id->partitions != NULL)
			free(id->partitions);
		id->partitions =
			malloc(part_count * sizeof(struct volume_id_partition));
		if (id->partitions == NULL)
			return -1;
		memset(id->partitions, 0x00, sizeof(struct volume_id_partition));

		id->partition_count = part_count;

		for (i = 0; i < part_count; i++) {
			__u64 poff;
			__u64 plen;

			buf = get_buffer(id, off + ((i+1) * bsize), 0x200);
			if (buf == NULL)
				return -1;

			part = (struct mac_partition *) buf;
			if (strncmp(part->signature, "PM", 2) != 0)
				return -1;

			poff = be32_to_cpu(part->start_block) * bsize;
			plen = be32_to_cpu(part->block_count) * bsize;
			dbg("found '%s' partition entry at 0x%llx, len 0x%llx",
			    part->type, poff, plen);

			id->partitions[i].off = poff;
			id->partitions[i].len = plen;
		}
		id->type_id = VOLUME_ID_PARTITIONTABLE;
		id->format_id = VOLUME_ID_MACPARTMAP;
		id->format = "mac_partition_map";
		return 0;
	}

	return -1;
}

#define HFS_SUPERBLOCK_OFFSET		0x400
#define HFS_NODE_LEAF			0xff
#define HFSPLUS_POR_CNID		1
static int probe_hfs_hfsplus(struct volume_id *id, __u64 off)
{
	struct finder_info {
		__u32	boot_folder;
		__u32	start_app;
		__u32	open_folder;
		__u32	os9_folder;
		__u32	reserved;
		__u32	osx_folder;
		__u8	id[8];
	} __attribute__((__packed__));

	struct hfs_mdb {
		__u8	signature[2];
		__u32	cr_date;
		__u32	ls_Mod;
		__u16	atrb;
		__u16	nm_fls;
		__u16	vbm_st;
		__u16	alloc_ptr;
		__u16	nm_al_blks;
		__u32	al_blk_size;
		__u32	clp_size;
		__u16	al_bl_st;
		__u32	nxt_cnid;
		__u16	free_bks;
		__u8	label_len;
		__u8	label[27];
		__u32	vol_bkup;
		__u16	vol_seq_num;
		__u32	wr_cnt;
		__u32	xt_clump_size;
		__u32	ct_clump_size;
		__u16	num_root_dirs;
		__u32	file_count;
		__u32	dir_count;
		struct finder_info finfo;
		__u8	embed_sig[2];
		__u16	embed_startblock;
		__u16	embed_blockcount;
	} __attribute__((__packed__)) *hfs;

	struct hfsplus_bnode_descriptor {
		__u32	next;
		__u32	prev;
		__u8	type;
		__u8	height;
		__u16	num_recs;
		__u16	reserved;
	} __attribute__((__packed__));

	struct hfsplus_bheader_record {
		__u16	depth;
		__u32	root;
		__u32	leaf_count;
		__u32	leaf_head;
		__u32	leaf_tail;
		__u16	node_size;
	} __attribute__((__packed__));

	struct hfsplus_catalog_key {
		__u16	key_len;
		__u32	parent_id;
		__u16	unicode_len;
		__u8	unicode[255 * 2];
	} __attribute__((__packed__));

	struct hfsplus_extent {
		__u32 start_block;
		__u32 block_count;
	} __attribute__((__packed__));

	struct hfsplus_fork {
		__u64 total_size;
        	__u32 clump_size;
		__u32 total_blocks;
		struct hfsplus_extent extents[8];
	} __attribute__((__packed__));

	struct hfsplus_vol_header {
		__u8	signature[2];
		__u16	version;
		__u32	attributes;
		__u32	last_mount_vers;
		__u32	reserved;
		__u32	create_date;
		__u32	modify_date;
		__u32	backup_date;
		__u32	checked_date;
		__u32	file_count;
		__u32	folder_count;
		__u32	blocksize;
		__u32	total_blocks;
		__u32	free_blocks;
		__u32	next_alloc;
		__u32	rsrc_clump_sz;
		__u32	data_clump_sz;
		__u32	next_cnid;
		__u32	write_count;
		__u64	encodings_bmp;
		struct finder_info finfo;
		struct hfsplus_fork alloc_file;
		struct hfsplus_fork ext_file;
		struct hfsplus_fork cat_file;
		struct hfsplus_fork attr_file;
		struct hfsplus_fork start_file;
	} __attribute__((__packed__)) *hfsplus;

	unsigned int blocksize;
	unsigned int cat_block;
	unsigned int cat_block_count;
	unsigned int cat_off;
	unsigned int cat_len;
	unsigned int leaf_node_head;
	unsigned int leaf_node_size;
	unsigned int alloc_block_size;
	unsigned int alloc_first_block;
	unsigned int embed_first_block;
	struct hfsplus_bnode_descriptor *descr;
	struct hfsplus_bheader_record *bnode;
	struct hfsplus_catalog_key *key;
	unsigned int	label_len;
	const __u8 *buf;

	buf = get_buffer(id, off + HFS_SUPERBLOCK_OFFSET, 0x200);
	if (buf == NULL)
                return -1;

	hfs = (struct hfs_mdb *) buf;
	if (strncmp(hfs->signature, "BD", 2) != 0)
		goto checkplus;

	/* it may be just a hfs wrapper for hfs+ */
	if (strncmp(hfs->embed_sig, "H+", 2) == 0) {
		alloc_block_size = be32_to_cpu(hfs->al_blk_size);
		dbg("alloc_block_size 0x%x", alloc_block_size);

		alloc_first_block = be16_to_cpu(hfs->al_bl_st);
		dbg("alloc_first_block 0x%x", alloc_first_block);

		embed_first_block = be16_to_cpu(hfs->embed_startblock);
		dbg("embed_first_block 0x%x", embed_first_block);

		off += (alloc_first_block * 512) +
		       (embed_first_block * alloc_block_size);
		dbg("hfs wrapped hfs+ found at offset 0x%llx", off);

		buf = get_buffer(id, off + HFS_SUPERBLOCK_OFFSET, 0x200);
		if (buf == NULL)
			return -1;
		goto checkplus;
	}

	if (hfs->label_len > 0 && hfs->label_len < 28) {
		set_label_raw(id, hfs->label, hfs->label_len);
		set_label_string(id, hfs->label, hfs->label_len) ;
	}

	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_HFS;
	id->format = "hfs";

	return 0;

checkplus:
	hfsplus = (struct hfsplus_vol_header *) buf;
	if (strncmp(hfsplus->signature, "H+", 2) == 0)
		goto hfsplus;
	if (strncmp(hfsplus->signature, "HX", 2) == 0)
		goto hfsplus;
	return -1;

hfsplus:
	blocksize = be32_to_cpu(hfsplus->blocksize);
	cat_block = be32_to_cpu(hfsplus->cat_file.extents[0].start_block);
	cat_block_count = be32_to_cpu(hfsplus->cat_file.extents[0].block_count);
	cat_off = (cat_block * blocksize);
	cat_len = cat_block_count * blocksize;
	dbg("catalog start 0x%llx, len 0x%x", off + cat_off, cat_len);

	buf = get_buffer(id, off + cat_off, 0x2000);
	if (buf == NULL)
		goto found;

	bnode = (struct hfsplus_bheader_record *)
		&buf[sizeof(struct hfsplus_bnode_descriptor)];

	leaf_node_head = be32_to_cpu(bnode->leaf_head);
	leaf_node_size = be16_to_cpu(bnode->node_size);

	dbg("catalog leaf node 0x%x, size 0x%x",
	    leaf_node_head, leaf_node_size);

	buf = get_buffer(id, off + cat_off + (leaf_node_head * leaf_node_size),
			 leaf_node_size);
	if (buf == NULL)
		goto found;

	descr = (struct hfsplus_bnode_descriptor *) buf;
	dbg("descriptor type 0x%x", descr->type);
	if (descr->type != HFS_NODE_LEAF)
		goto found;
	
	key = (struct hfsplus_catalog_key *)
		&buf[sizeof(struct hfsplus_bnode_descriptor)];

	dbg("parent id 0x%x", be32_to_cpu(key->parent_id));
	if (be32_to_cpu(key->parent_id) != HFSPLUS_POR_CNID)
		goto found;

	label_len = be16_to_cpu(key->unicode_len) * 2;
	dbg("label unicode16 len %i", label_len);
	set_label_raw(id, key->unicode, label_len);
	set_label_unicode16(id, key->unicode, BE, label_len);

found:
	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_HFSPLUS;
	id->format = "hfsplus";

	return 0;
}

#define MFT_RECORD_VOLUME			3
#define MFT_RECORD_ATTR_VOLUME_NAME		0x60u
#define MFT_RECORD_ATTR_OBJECT_ID		0x40u
#define MFT_RECORD_ATTR_END			0xffffffffu
static int probe_ntfs(struct volume_id *id, __u64 off)
{
	struct ntfs_super_block {
		__u8	jump[3];
		__u8	oem_id[8];
		struct bios_param_block {
			__u16	bytes_per_sector;
			__u8	sectors_per_cluster;
			__u16	reserved_sectors;
			__u8	fats;
			__u16	root_entries;
			__u16	sectors;
			__u8	media_type;		/* 0xf8 = hard disk */
			__u16	sectors_per_fat;
			__u16	sectors_per_track;
			__u16	heads;
			__u32	hidden_sectors;
			__u32	large_sectors;
		} __attribute__((__packed__)) bpb;
		__u8 unused[4];
		__u64	number_of_sectors;
		__u64	mft_cluster_location;
		__u64	mft_mirror_cluster_location;
		__s8	cluster_per_mft_record;
	} __attribute__((__packed__)) *ns;

	struct master_file_table_record {
		__u8	magic[4];
		__u16	usa_ofs;
		__u16	usa_count;
		__u64	lsn;
		__u16	sequence_number;
		__u16	link_count;
		__u16	attrs_offset;
		__u16	flags;
		__u32	bytes_in_use;
		__u32	bytes_allocated;
	} __attribute__((__packed__)) *mftr;

	struct file_attribute {
		__u32	type;
		__u32	len;
		__u8	non_resident;
		__u8	name_len;
		__u16	name_offset;
		__u16	flags;
		__u16	instance;
		__u32	value_len;
		__u16	value_offset;
	} __attribute__((__packed__)) *attr;

	unsigned int sector_size;
	unsigned int cluster_size;
	__u64 mft_cluster;
	__u64 mft_off;
	unsigned int mft_record_size;
	unsigned int attr_type;
	unsigned int attr_off;
	unsigned int attr_len;
	unsigned int val_off;
	unsigned int val_len;
	const __u8 *buf;
	const __u8 *val;

	ns = (struct ntfs_super_block *) get_buffer(id, off, 0x200);
	if (ns == NULL)
		return -1;

	if (strncmp(ns->oem_id, "NTFS", 4) != 0)
		return -1;

	dbg("+++++++++ 1");

	sector_size = le16_to_cpu(ns->bpb.bytes_per_sector);
	cluster_size = ns->bpb.sectors_per_cluster * sector_size;
	mft_cluster = le64_to_cpu(ns->mft_cluster_location);
	mft_off = mft_cluster * cluster_size;

	if (ns->cluster_per_mft_record < 0)
		/* size = -log2(mft_record_size); normally 1024 Bytes */
		mft_record_size = 1 << -ns->cluster_per_mft_record;
	else
		mft_record_size = ns->cluster_per_mft_record * cluster_size;

	dbg("sectorsize  0x%x", sector_size);
	dbg("clustersize 0x%x", cluster_size);
	dbg("mftcluster  %lli", mft_cluster);
	dbg("mftoffset  0x%llx", mft_off);
	dbg("cluster per mft_record  %i", ns->cluster_per_mft_record);
	dbg("mft record size  %i", mft_record_size);

	buf = get_buffer(id, off + mft_off + (MFT_RECORD_VOLUME * mft_record_size),
			 mft_record_size);
	if (buf == NULL)
		goto found;

	mftr = (struct master_file_table_record*) buf;

	dbg("mftr->magic[0] = '%c' %03d, 0x%02x", mftr->magic[0], mftr->magic[0], mftr->magic[0]);
	dbg("mftr->magic[1] = '%c' %03d, 0x%02x", mftr->magic[1], mftr->magic[1], mftr->magic[1]);
	dbg("mftr->magic[2] = '%c' %03d, 0x%02x", mftr->magic[2], mftr->magic[2], mftr->magic[2]);
	dbg("mftr->magic[3] = '%c' %03d, 0x%02x", mftr->magic[3], mftr->magic[3], mftr->magic[3]);
	if (strncmp(mftr->magic, "FILE", 4) != 0)
		goto found;

	attr_off = le16_to_cpu(mftr->attrs_offset);
	dbg("file $Volume's attributes are at offset %i", attr_off);

	while (1) {
		attr = (struct file_attribute*) &buf[attr_off];
		attr_type = le32_to_cpu(attr->type);
		attr_len = le16_to_cpu(attr->len);
		val_off = le16_to_cpu(attr->value_offset);
		val_len = le32_to_cpu(attr->value_len);

		if (attr_type == MFT_RECORD_ATTR_END)
			break;

		dbg("found attribute type 0x%x, len %i, at offset %i",
		    attr_type, attr_len, attr_off);

		if (attr_type == MFT_RECORD_ATTR_VOLUME_NAME) {
			dbg("found label, len %i", val_len);
			if (val_len > VOLUME_ID_LABEL_SIZE)
				val_len = VOLUME_ID_LABEL_SIZE;

			val = &((__u8 *) attr)[val_off];
			set_label_raw(id, val, val_len);
			set_label_unicode16(id, val, LE, val_len);
		}

		if (attr_type == MFT_RECORD_ATTR_OBJECT_ID) {
			dbg("found uuid");
			/* Not sure about the on-disk format of MS's uuid's
			 * just assuming a byte stream, it should be unique
			 * anyway.
			 * Any authoritative information is welcome. */
			val = &((__u8 *) attr)[val_off];
			set_uuid(id, val, 16);
		}

		if (attr_len == 0)
			break;
		attr_off += attr_len;
		if (attr_off >= mft_record_size)
			break;
	}

found:
	id->type_id = VOLUME_ID_FILESYSTEM;
	id->format_id = VOLUME_ID_NTFS;
	id->format = "ntfs";

	return 0;
}

#define LARGEST_PAGESIZE			0x4000
static int probe_swap(struct volume_id *id, __u64 off)
{
	const __u8 *sig;
	unsigned int page;

	/* huhh, the swap signature is on the end of the PAGE_SIZE */
	for (page = 0x1000; page <= LARGEST_PAGESIZE; page <<= 1) {
			sig = get_buffer(id, off + page-10, 10);
			if (sig == NULL)
				return -1;

			if (strncmp(sig, "SWAP-SPACE", 10) == 0)
				goto found;
			if (strncmp(sig, "SWAPSPACE2", 10) == 0)
				goto found;
	}
	return -1;

found:
	id->type_id = VOLUME_ID_OTHER;
	id->format_id = VOLUME_ID_SWAP;
	id->format = "swap";

	return 0;
}

/* probe volume for filesystem type and try to read label+uuid */
int volume_id_probe(struct volume_id *id,
		    enum volume_id_type type, unsigned long long off)
{
	int rc;

	if (id == NULL)
		return -EINVAL;

	switch (type) {
	case VOLUME_ID_MSDOSPARTTABLE:
		rc = probe_msdos_part_table(id, off);
		break;
	case VOLUME_ID_EXT3:
	case VOLUME_ID_EXT2:
		rc = probe_ext(id, off);
		break;
	case VOLUME_ID_REISER:
		rc = probe_reiser(id, off);
		break;
	case VOLUME_ID_XFS:
		rc = probe_xfs(id, off);
		break;
	case VOLUME_ID_JFS:
		rc = probe_jfs(id, off);
		break;
	case VOLUME_ID_MSDOS:
		rc = probe_msdos(id, off);
		break;
	case VOLUME_ID_VFAT:
		rc = probe_vfat(id, off);
		break;
	case VOLUME_ID_UDF:
		rc = probe_udf(id, off);
		break;
	case VOLUME_ID_ISO9660:
		rc = probe_iso9660(id, off);
		break;
	case VOLUME_ID_MACPARTMAP:
		rc = probe_mac_partition_map(id, off);
		break;
	case VOLUME_ID_HFS:
	case VOLUME_ID_HFSPLUS:
		rc = probe_hfs_hfsplus(id, off);
		break;
	case VOLUME_ID_UFS:
		rc = probe_ufs(id, off);
		break;
	case VOLUME_ID_NTFS:
		rc = probe_ntfs(id, off);
		break;
	case VOLUME_ID_SWAP:
		rc = probe_swap(id, off);
		break;
	case VOLUME_ID_ALL:
	default:
		/* read only minimal buffer, cause of the slow floppies */
		rc = probe_vfat(id, off);
		if (rc == 0)
			break;
		rc = probe_msdos(id, off);
		if (rc == 0)
			break;
		rc = probe_msdos_part_table(id, off);
		if (rc == 0)
			break;

		/* fill buffer with maximum */
		get_buffer(id, 0, SB_BUFFER_SIZE);

		rc = probe_swap(id, off);
		if (rc == 0)
			break;
		rc = probe_ext(id, off);
		if (rc == 0)
			break;
		rc = probe_reiser(id, off);
		if (rc == 0)
			break;
		rc = probe_xfs(id, off);
		if (rc == 0)
			break;
		rc = probe_jfs(id, off);
		if (rc == 0)
			break;
		rc = probe_udf(id, off);
		if (rc == 0)
			break;
		rc = probe_iso9660(id, off);
		if (rc == 0)
			break;
		rc = probe_ntfs(id, off);
		if (rc == 0)
			break;
		rc = probe_mac_partition_map(id, off);
		if (rc == 0)
			break;
		rc = probe_hfs_hfsplus(id, off);
		if (rc == 0)
			break;
		rc = probe_ufs(id, off);
		if (rc == 0)
			break;
		rc = -1;
	}

	/* If the filestystem in recognized, we free the allocated buffers,
	   otherwise they will stay in place for the possible next probe call */
	if (rc == 0)
		free_buffer(id);

	return rc;
}

/* open volume by already open file descriptor */
struct volume_id *volume_id_open_fd(int fd)
{
	struct volume_id *id;

	id = malloc(sizeof(struct volume_id));
	if (id == NULL)
		return NULL;
	memset(id, 0x00, sizeof(struct volume_id));

	id->fd = fd;

	return id;
}

/* open volume by device node */
struct volume_id *volume_id_open_node(const char *path)
{
	struct volume_id *id;
	int fd;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		dbg("unable to open '%s'", path);
		return NULL;
	}

	id = volume_id_open_fd(fd);
	if (id == NULL)
		return NULL;

	/* close fd on device close */
	id->fd_close = 1;

	return id;
}

/* open volume by major/minor */
struct volume_id *volume_id_open_dev_t(dev_t devt)
{
	struct volume_id *id;
	__u8 tmp_node[VOLUME_ID_PATH_MAX];

	snprintf(tmp_node, VOLUME_ID_PATH_MAX,
		 "/tmp/volume-%u-%u-%u", getpid(), major(devt), minor(devt));
	tmp_node[VOLUME_ID_PATH_MAX] = '\0';

	/* create tempory node to open the block device */
	unlink(tmp_node);
	if (mknod(tmp_node, (S_IFBLK | 0600), devt) != 0)
		return NULL;

	id = volume_id_open_node(tmp_node);

	unlink(tmp_node);

	return id;
}

/* free allocated volume info */
void volume_id_close(struct volume_id *id)
{
	if (id == NULL)
		return;

	if (id->fd_close != 0)
		close(id->fd);

	free_buffer(id);

	if (id->partitions != NULL)
		free(id->partitions);

	free(id);
}
