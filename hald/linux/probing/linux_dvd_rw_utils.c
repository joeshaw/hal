/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-
 *
 * This is part of dvd+rw-tools by Andy Polyakov <appro@fy.chalmers.se>
 *
 * Use-it-on-your-own-risk, GPL bless...
 *
 * For further details see http://fy.chalmers.se/~appro/linux/DVD+RW/
*/

#include <glib.h>

#define CREAM_ON_ERRNO(s)	do {				\
    switch ((s)[2]&0x0F)					\
    {	case 2:	if ((s)[12]==4) errno=EAGAIN;	break;		\
	case 5:	errno=EINVAL;					\
		if ((s)[13]==0)					\
		{   if ((s)[12]==0x21)		errno=ENOSPC;	\
		    else if ((s)[12]==0x20)	errno=ENODEV;	\
		}						\
		break;						\
    }								\
} while(0)
#define ERRCODE(s)	((((s)[2]&0x0F)<<16)|((s)[12]<<8)|((s)[13]))
#define	SK(errcode)	(((errcode)>>16)&0xF)
#define	ASC(errcode)	(((errcode)>>8)&0xFF)
#define ASCQ(errcode)	((errcode)&0xFF)

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <errno.h>
#include <string.h>
#include <mntent.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <poll.h>
#include <sys/time.h>

#include "linux_dvd_rw_utils.h"

#if !defined(SG_FLAG_LUN_INHIBIT)
# if defined(SG_FLAG_UNUSED_LUN_INHIBIT)
#  define SG_FLAG_LUN_INHIBIT SG_FLAG_UNUSED_LUN_INHIBIT
# else
#  define SG_FLAG_LUN_INHIBIT 0
# endif
#endif

typedef enum {
	NONE = CGC_DATA_NONE,	// 3
	READ = CGC_DATA_READ,	// 2
	WRITE = CGC_DATA_WRITE	// 1
} Direction;

typedef struct ScsiCommand ScsiCommand;

struct ScsiCommand {
	int fd;
	int autoclose;
	char *filename;
	struct cdrom_generic_command cgc;
	union {
		struct request_sense s;
		unsigned char u[18];
	} _sense;
	struct sg_io_hdr sg_io;
};

#define DIRECTION(i) (Dir_xlate[i]);

/* 1,CGC_DATA_WRITE
 * 2,CGC_DATA_READ
 * 3,CGC_DATA_NONE
 */
const int Dir_xlate[4] = {
	0,			// implementation-dependent...
	SG_DXFER_TO_DEV,	// 1,CGC_DATA_WRITE
	SG_DXFER_FROM_DEV,	// 2,CGC_DATA_READ
	SG_DXFER_NONE		// 3,CGC_DATA_NONE
};

static ScsiCommand *
scsi_command_new (void)
{
	ScsiCommand *cmd;

	cmd = (ScsiCommand *) malloc (sizeof (ScsiCommand));
	memset (cmd, 0, sizeof (ScsiCommand));
	cmd->fd = -1;
	cmd->filename = NULL;
	cmd->autoclose = 1;

	return cmd;
}

static ScsiCommand *
scsi_command_new_from_fd (int f)
{
	ScsiCommand *cmd;

	cmd = scsi_command_new ();
	cmd->fd = f;
	cmd->autoclose = 0;

	return cmd;
}

static void
scsi_command_free (ScsiCommand * cmd)
{
	if (cmd->fd >= 0 && cmd->autoclose) {
		close (cmd->fd);
		cmd->fd = -1;
	}
	if (cmd->filename) {
		free (cmd->filename);
		cmd->filename = NULL;
	}

	free (cmd);
}

static int
scsi_command_transport (ScsiCommand * cmd, Direction dir, void *buf,
			size_t sz)
{
	int ret = 0;

	cmd->sg_io.dxferp = buf;
	cmd->sg_io.dxfer_len = sz;
	cmd->sg_io.dxfer_direction = DIRECTION (dir);

	if (ioctl (cmd->fd, SG_IO, &cmd->sg_io))
		return -1;

	if ((cmd->sg_io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
		errno = EIO;
		ret = -1;
		if (cmd->sg_io.masked_status & CHECK_CONDITION) {
			CREAM_ON_ERRNO ((unsigned char*)cmd->sg_io.sbp);
			ret = ERRCODE ((unsigned char*)cmd->sg_io.sbp);
			if (ret == 0)
				ret = -1;
		}
	}

	return ret;
}

static void
scsi_command_init (ScsiCommand * cmd, size_t i, int arg)
{
	if (i == 0) {
		memset (&cmd->cgc, 0, sizeof (cmd->cgc));
		memset (&cmd->_sense, 0, sizeof (cmd->_sense));
		cmd->cgc.quiet = 1;
		cmd->cgc.sense = &cmd->_sense.s;
		memset (&cmd->sg_io, 0, sizeof (cmd->sg_io));
		cmd->sg_io.interface_id = 'S';
		cmd->sg_io.mx_sb_len = sizeof (cmd->_sense);
		cmd->sg_io.cmdp = cmd->cgc.cmd;
		cmd->sg_io.sbp = cmd->_sense.u;
		cmd->sg_io.flags = SG_FLAG_LUN_INHIBIT | SG_FLAG_DIRECT_IO;
	}
	cmd->sg_io.cmd_len = i + 1;
	cmd->cgc.cmd[i] = arg;
}

int
get_dvd_r_rw_profile (int fd)
{
	ScsiCommand *cmd;
	int retval = 0;
	unsigned char page[20];
	unsigned char *list;
	int i, len;

	cmd = scsi_command_new_from_fd (fd);

	scsi_command_init (cmd, 0, 0x46);
	scsi_command_init (cmd, 1, 2);
	scsi_command_init (cmd, 8, 8);
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, page, 8)) {
		/* GET CONFIGURATION failed */
		scsi_command_free (cmd);
		return -1;
	}

	/* See if it's 2 gen drive by checking if DVD+R profile is an option */
	len = 4 + (page[0] << 24 | page[1] << 16 | page[2] << 8 | page[3]);
	if (len > 264) {
		scsi_command_free (cmd);
		/* insane profile list length */
		return -1;
	}

	list = (unsigned char *) malloc (len);

	scsi_command_init (cmd, 0, 0x46);
	scsi_command_init (cmd, 1, 2);
	scsi_command_init (cmd, 7, len >> 8);
	scsi_command_init (cmd, 8, len);
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, list, len)) {
		/* GET CONFIGURATION failed */
		scsi_command_free (cmd);
		free (list);
		return -1;
	}

	for (i = 12; i < list[11]; i += 4) {
		int profile = (list[i] << 8 | list[i + 1]);
		/* 0x13: DVD-RW Restricted Overwrite
		 * 0x14: DVD-RW Sequential
		 * 0x15: DVD-R Dual Layer Sequential
		 * 0x16: DVD-R Dual Layer Jump
		 * 0x1A: DVD+RW  
		 * 0x1B: DVD+R 
		 * 0x2A: DVD+RW DL
		 * 0x2B: DVD+R DL 
		 * 0x40: BD-ROM
		 * 0x41: BD-R SRM
		 * 0x42: BR-R RRM
		 * 0x43: BD-RE
		 * 0x50: HD DVD-ROM
		 * 0x51: HD DVD-R 
		 * 0x52: HD DVD-Rewritable 
		 */

		switch (profile) {
			case 0x13:
			case 0x14:
				retval |= DRIVE_CDROM_CAPS_DVDRW;
				break;
			case 0x15:
			case 0x16:
				retval |= DRIVE_CDROM_CAPS_DVDRDL;
				break;
			case 0x1B:
				retval |= DRIVE_CDROM_CAPS_DVDPLUSR;
				break;
			case 0x1A:
				retval |= DRIVE_CDROM_CAPS_DVDPLUSRW;
				break;
			case 0x2A:
				retval |= DRIVE_CDROM_CAPS_DVDPLUSRWDL;
				break;
			case 0x2B:
				retval |= DRIVE_CDROM_CAPS_DVDPLUSRDL;
				break;
			case 0x40:
				retval |= DRIVE_CDROM_CAPS_BDROM;
				break;
			case 0x41:
			case 0x42:
				retval |= DRIVE_CDROM_CAPS_BDR;
				break;
			case 0x43:
				retval |= DRIVE_CDROM_CAPS_BDRE;
				break;
			case 0x50:
				retval |= DRIVE_CDROM_CAPS_HDDVDROM;
				break;
			case 0x51:
				retval |= DRIVE_CDROM_CAPS_HDDVDR;
				break;
			case 0x52:
				retval |= DRIVE_CDROM_CAPS_HDDVDRW;
				break;
			default:
				break;
		}
	}

	scsi_command_free (cmd);
	free (list);
	
	return retval;
	
}

static unsigned char *
pull_page2a_from_fd (int fd)
{
	ScsiCommand *cmd;
	unsigned char header[12], *page2A;
	unsigned int len, bdlen;

	cmd = scsi_command_new_from_fd (fd);

	scsi_command_init (cmd, 0, 0x5A);	/* MODE SENSE */
	scsi_command_init (cmd, 1, 0x08);	/* Disable Block Descriptors */
	scsi_command_init (cmd, 2, 0x2A);	/* Capabilities and Mechanical Status */
	scsi_command_init (cmd, 8, sizeof (header));	/* header only to start with */
	scsi_command_init (cmd, 9, 0);

	if (scsi_command_transport (cmd, READ, header, sizeof (header))) {
		/* MODE SENSE failed */
		scsi_command_free (cmd);
		return NULL;
	}

	len = (header[0] << 8 | header[1]) + 2;
	bdlen = header[6] << 8 | header[7];

	/* should never happen as we set "DBD" above */
	if (bdlen) {
		if (len < (8 + bdlen + 30)) {
			/* LUN impossible to bear with */
			scsi_command_free (cmd);
			return NULL;
		}
	} else if (len < (8 + 2 + (unsigned int) header[9])) {
		/* SANYO does this. */
		len = 8 + 2 + header[9];
	}

	page2A = (unsigned char *) malloc (len);
	if (page2A == NULL) {
		/* ENOMEM */
		scsi_command_free (cmd);
		return NULL;
	}

	scsi_command_init (cmd, 0, 0x5A);	/* MODE SENSE */
	scsi_command_init (cmd, 1, 0x08);	/* Disable Block Descriptors */
	scsi_command_init (cmd, 2, 0x2A);	/* Capabilities and Mechanical Status */
	scsi_command_init (cmd, 7, len >> 8);
	scsi_command_init (cmd, 8, len);	/* Real length */
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, page2A, len)) {
		/* MODE SENSE failed */
		scsi_command_free (cmd);
		free (page2A);
		return NULL;
	}

	scsi_command_free (cmd);

	len -= 2;
	/* paranoia */
	if (len < ((unsigned int) page2A[0] << 8 | page2A[1])) {
		page2A[0] = len >> 8;
		page2A[1] = len;
	}

	return page2A;
}

static int
int_compare (const void *a, const void *b)
{
	/* descending order */
	return *((int *) b) - *((int *) a);
}

/* gets the list of supported write speeds.  in the event
 * that anything goes wrong, returns NULL.
 */
static char *
get_write_speeds (const unsigned char *p, int length, int max_speed)
{
	char *result, *str;
	int nr_records;
	int *tmpspeeds;
	int i, j;

	result = NULL;

	/* paranoia */
	if (length < 32)
		return NULL;

	nr_records = p[30] << 8 | p[31];

	/* paranoia */
	if (length < 32 + 4 * nr_records)
		return NULL;

	tmpspeeds = malloc (nr_records * sizeof (int));

	for (i = 0; i < nr_records; i++)
	{
		tmpspeeds[i] = p[4*i + 34] << 8 | p[4*i + 35];

		/* i'm not sure how likely this is to show up, but it's
		 * definitely wrong.  if we see it, abort.
		 */
		if (tmpspeeds[i] == 0)
			goto free_tmpspeeds;
	}

	/* sort */
	qsort (tmpspeeds, nr_records, sizeof (int), int_compare);

	/* uniq */
	for (i = j = 0; i < nr_records; i++)
	{
		tmpspeeds[j] = tmpspeeds[i];

		/* make sure we don't look past the end of the array */
		if (i >= (nr_records - 1) || tmpspeeds[i+1] != tmpspeeds[i])
			j++;
	}

	/* j is now the number of unique entries in the array */
	if (j == 0)
		/* no entries?  this isn't right. */
		goto free_tmpspeeds;

	/* sanity check: the first item in the descending order
	 * list ought to be the highest speed as detected through
	 * other means
	 */
	if (tmpspeeds[0] != max_speed)
		/* sanity check failed. */
		goto free_tmpspeeds;

	/* our values are 16-bit.  8 bytes per value
	 * is more than enough including space for
	 * ',' and '\0'.  we know j is not zero.
	 */
	result = str = malloc (8 * j);

	for (i = 0; i < j; i++)
	{
		if (i > 0)
			*(str++) = ',';

		str += sprintf (str, "%d", tmpspeeds[i]);
	}

free_tmpspeeds:
	free (tmpspeeds);

	return result;
}

int
get_read_write_speed (int fd, int *read_speed, int *write_speed, char **write_speeds)
{
	unsigned char *page2A;
	int len, hlen;
	unsigned char *p;

	*read_speed = 0;
	*write_speed = 0;
	*write_speeds = NULL;

	page2A = pull_page2a_from_fd (fd);
	if (page2A == NULL) {
		printf ("Failed to get Page 2A\n");
		/* Failed to get Page 2A */
		return -1;
	}

	len = (page2A[0] << 8 | page2A[1]) + 2;
	hlen = 8 + (page2A[6] << 8 | page2A[7]);
	p = page2A + hlen;

	/* Values guessed from the cd_mode_page_2A struct
	 * in cdrecord's libscg/scg/scsireg.h */
	if (len < (hlen + 30) || p[1] < (30 - 2)) {
		/* no MMC-3 "Current Write Speed" present,
		 * try to use the MMC-2 one */
		if (len < (hlen + 20) || p[1] < (20 - 2))
			*write_speed = 0;
		else
			*write_speed = p[18] << 8 | p[19];
	} else {
		*write_speed = p[28] << 8 | p[29];
	}

	if (len >= hlen+9)
	    *read_speed = p[8] << 8 | p[9];
	else
	    *read_speed = 0;

	*write_speeds = get_write_speeds (p, len, *write_speed);

	free (page2A);

	return 0;
}


static int
get_disc_capacity_cd (int fd,
		      guint64 *size)
{
	ScsiCommand *cmd;
	int retval;
	guint64 block_size;
	guint64 num_blocks;
	unsigned char header [8];

	retval = -1;

	cmd = scsi_command_new_from_fd (fd);
	scsi_command_init (cmd, 0, 0x25);
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, header, 8)) {
		/* READ CDROM CAPACITY failed */
		goto done;
	}

	num_blocks = (header [0] << 24) | (header [1] << 16) | (header [2] << 8) | header [3];
	num_blocks++;
	block_size = header [4] << 24 | header [5] << 16 | header [6] << 8 | header [7];

	if (size) {
		*size = num_blocks * block_size;
	}
	retval = 0;

 done:
	scsi_command_free (cmd);

	return retval;
}

static int
get_disc_capacity_cdr (int fd,
		       guint64 *size)
{
	ScsiCommand *cmd;
	int retval;
	guint64 secs;
	unsigned char toc [8];
	unsigned char *atip;
	int len;

	retval = -1;

	cmd = scsi_command_new_from_fd (fd);
	/* READ_TOC */
	scsi_command_init (cmd, 0, 0x43);
	/* FMT_ATIP */
	scsi_command_init (cmd, 2, 4 & 0x0F);
	scsi_command_init (cmd, 6, 0);
	scsi_command_init (cmd, 8, 4);
	scsi_command_init (cmd, 9, 0);

	if (scsi_command_transport (cmd, READ, toc, 4)) {
		/* READ TOC failed */
		goto done;
	}

	len = 2 + (toc [0] << 8 | toc [1]);

	atip = (unsigned char *) malloc (len);

	scsi_command_init (cmd, 0, 0x43);
	scsi_command_init (cmd, 2, 4 & 0x0F);
	scsi_command_init (cmd, 6, 0);
	scsi_command_init (cmd, 7, len >> 8);
	scsi_command_init (cmd, 8, len);
	scsi_command_init (cmd, 9, 0);

	if (scsi_command_transport (cmd, READ, atip, len)) {
		/* READ TOC failed */
		free (atip);
		goto done;
	}

	secs = atip [12] * 60 + atip [13] + (atip [14] / 75 + 1);

	if (size) {
		*size = (1 + secs * 7 / 48) * 1024 * 1024;
	}
	retval = 0;

	free (atip);
 done:
	scsi_command_free (cmd);

	return retval;
}

static int
get_disc_capacity_dvdr_from_type (int fd,
				  int type,
				  guint64 *size)
{
	ScsiCommand *cmd;
	unsigned char formats [260];
	unsigned char buf [32];
	guint64 blocks;
	guint64 nwa;
	int i;
	int len;
	int obligatory;
	int retval;
	int next_track;

	retval = -1;
	blocks = 0;
	next_track = 1;

	cmd = scsi_command_new_from_fd (fd);

 retry:
	if (type == 0x1A || type == 0x14 || type == 0x13 || type == 0x12) {

		/* READ FORMAT CAPACITIES */
		scsi_command_init (cmd, 0, 0x23);
		scsi_command_init (cmd, 8, 12);
		scsi_command_init (cmd, 9, 0);
		if (scsi_command_transport (cmd, READ, formats, 12)) {
			/* READ FORMAT CAPACITIES failed */
			goto done;
		}

		len = formats [3];
		if (len & 7 || len < 16) {
			/* Length isn't sane */
			goto done;
		}

		scsi_command_init (cmd, 0, 0x23);
		scsi_command_init (cmd, 7, (4 + len) >> 8);
		scsi_command_init (cmd, 8, (4 + len) & 0xFF);
		scsi_command_init (cmd, 9, 0);
		if (scsi_command_transport (cmd, READ, formats, 4 + len)) {
			/* READ FORMAT CAPACITIES failed */
			goto done;
		}

		if (len != formats [3]) {
			/* Parameter length inconsistency */
			goto done;
		}
	}

	obligatory = 0x00;

	switch (type) {
    	case 0x1A:		/* DVD+RW */
		obligatory = 0x26;
	case 0x13:		/* DVD-RW Restricted Overwrite */
	case 0x14:		/* DVD-RW Sequential */
		for (i = 8, len = formats [3]; i < len; i += 8) {
			if ((formats [4 + i + 4] >> 2) == obligatory) {
				break;
			}
		}

		if (i == len) {
			/* Can't find obligatory format descriptor */
			goto done;
		}

		blocks  = formats [4 + i + 0] << 24;
		blocks |= formats [4 + i + 1] << 16;
		blocks |= formats [4 + i + 2] << 8;
		blocks |= formats [4 + i + 3];
		nwa = formats [4 + 5] << 16 | formats [4 + 6] << 8 | formats [4 + 7];
		if (nwa > 2048) {
			blocks *= nwa / 2048;
		} else if (nwa < 2048) {
			blocks /= 2048 / nwa;
		}

		retval = 0;
		break;

	case 0x12:		/* DVD-RAM */

		blocks  = formats [4 + 0] << 24;
		blocks |= formats [4 + 1] << 16;
		blocks |= formats [4 + 2] << 8;
		blocks |= formats [4 + 3];
		nwa = formats [4 + 5] << 16 | formats [4 + 6] << 8 | formats [4 + 7];
		if (nwa > 2048) {
			blocks *= nwa / 2048;
		} else if (nwa < 2048) {
			blocks /= 2048 / nwa;
		}

		retval = 0;
		break;

	case 0x11:		/* DVD-R */
	case 0x15:		/* DVD-R Dual Layer Sequential */
	case 0x16:		/* DVD-R Dual Layer Jump */
	case 0x1B:		/* DVD+R */
	case 0x2B:		/* DVD+R Double Layer */
	case 0x41:		/* BD-R SRM */

		/* READ TRACK INFORMATION */
		scsi_command_init (cmd, 0, 0x52);
		scsi_command_init (cmd, 1, 1);
		scsi_command_init (cmd, 4, next_track >> 8);
		scsi_command_init (cmd, 5, next_track & 0xFF);
		scsi_command_init (cmd, 8, sizeof (buf));
		scsi_command_init (cmd, 9, 0);
		if (scsi_command_transport (cmd, READ, buf, sizeof (buf))) {
			/* READ TRACK INFORMATION failed */
			if (next_track > 0) {
				goto done;
			} else {
				next_track = 1;
				goto retry;
			}
		}

		blocks = buf [24] << 24;
		blocks |= buf [25] << 16;
		blocks |= buf [26] << 8;
		blocks |= buf [27];

		retval = 0;
		break;
	case 0x43:		/* BD-RE */
		/* Pull the formatted capacity */
		blocks  = formats [4 + 0] << 24;
		blocks |= formats [4 + 1] << 16;
		blocks |= formats [4 + 2] << 8;
		blocks |= formats [4 + 3];
		break;
	default:
		blocks = 0;
		break;
	}

 done:
	scsi_command_free (cmd);

	if (size) {
		*size = blocks * 2048;
	}

	return retval;
}

int
get_disc_capacity_for_type (int fd,
			    int type,
			    guint64 *size)
{
	int retval;

	retval = -1;

	switch (type) {
	case 0x8:
		retval = get_disc_capacity_cd (fd, size);
		break;
	case 0x9:
	case 0xa:
		retval = get_disc_capacity_cdr (fd, size);
		break;
	case 0x10:
		retval = get_disc_capacity_cd (fd, size);
		break;
	case 0x11:
	case 0x13:
	case 0x14:
	case 0x1B:
	case 0x2B:
	case 0x1A:
	case 0x12:
	case 0x41:
	case 0x43:
		retval = get_disc_capacity_dvdr_from_type (fd, type, size);
		break;
	default:
		retval = -1;
	}

	return retval;
}


static int
get_mmc_profile (int fd, int * isblank)
{
	ScsiCommand * cmd;
	int retval = -1;
	unsigned char formats[260],disc_info[32];
	unsigned char page[20];
	unsigned char buf[8],inq[128];
	int profile=0,once=1,blank=0,err,erasable=0;
	unsigned int len;
	
	cmd = scsi_command_new_from_fd (fd);

	/* For valgrind */
	memset (&page, 0, sizeof (page));
	memset (&buf, 0, sizeof (buf));
	memset (&inq, 0, sizeof (inq));
	memset (&disc_info, 0, sizeof (disc_info));
	memset (&formats, 0, sizeof (formats));
	
	/*
	  INQUIRY is considered to be "non-intrusive" in a sense that it
	  won't interfere with any other operation nor clear sense data,
	  which might be important to retain for security reasons.
	*/
	
	scsi_command_init(cmd,0,0x12);	/* INQUIRY */
	scsi_command_init(cmd,4,36);
	scsi_command_init(cmd,5,0);
	if ((err=scsi_command_transport(cmd,READ,inq,36))) {
		/*sperror ("INQUIRY",err);*/
		goto bail;
	}
	
	/* make sure we're talking to MMC unit, for security reasons... */
	if ((inq[0]&0x1F) != 5) {
		fprintf (stderr, "Could not determine drive profile: not an MMC unit!\n");
		goto bail;
	}
	
	do {
		scsi_command_init(cmd,0,0x46);
		scsi_command_init(cmd,8,sizeof(buf));
		scsi_command_init(cmd,9,0);
		if ((err=scsi_command_transport(cmd,READ,buf,sizeof(buf)))) {
			/*sperror ("GET CONFIGURATION",err);*/
			/* this is not a fatal error -- some older drives support MMC-1
			 * but don't support the GET CONFIGURATION command (which is part
			 * of the MMC-2 spec). */
		} else {
			if ((profile = buf[6]<<8|buf[7]) || !once) break;
		}
		
		// no media?
		scsi_command_init(cmd,0,0);	// TEST UNIT READY
		scsi_command_init(cmd,5,0);
		if ((scsi_command_transport(cmd,READ,buf,sizeof(buf))&0xFFF00) != 0x23A00) break;
		
		// try to load tray...
		scsi_command_init(cmd,0,0x1B);	// START/STOP UNIT
		scsi_command_init(cmd,4,0x3);	// "Load"
		scsi_command_init(cmd,5,0);
		if ((err=scsi_command_transport(cmd,READ,buf,sizeof(buf)))) {
			/*sperror ("LOAD TRAY",err);*/
			fprintf (stderr, "Could not determine drive profile: Error loading drive tray\n");
			goto bail;
		}
		
		/*    wait_for_unit (cmd);*/
	} while (once--);
	
	scsi_command_init(cmd,0,0x51);	// READ DISC INFORMATION
	scsi_command_init(cmd,8,sizeof(disc_info));
	scsi_command_init(cmd,9,0);
	if ((err=scsi_command_transport(cmd,READ,disc_info,sizeof(disc_info)))) {
		/*sperror ("READ DISC INFORMATION",err);*/
		fprintf (stderr, "Could not fully determine drive profile %x: Error reading disc information\n", profile);
		goto bail;
	}

	// see if it's blank media
	if ((disc_info[2]&3) == 0)	blank=1;
	
	if (!profile ) {
		/* if the profile has not yet been set, we're dealing with an older
		 * CD-R or CD-RW burner (which doesn't know the GET CONFIGURATION
		 * command.  Do some digging into the disc type to figure out what's
		 * in the drive */
		erasable = ((disc_info[2] & 16));
		if (blank && !erasable) {
			profile = 0x09;  /* CD-R                        */
		} else if (erasable) {
			profile = 0x0a;  /* CD-RW                       */
		} else if (disc_info[8] == 0x00) {
			profile = 0x08;  /* Commercial CDs and Audio CD */
		} else {
			fprintf (stderr, "Could not determine profile or type of media\n");
			goto bail;
		}
	}
	
	if ((profile != 0x1A && profile != 0x13 && profile != 0x12)) {
		retval = profile;
	}
	else {
		scsi_command_init(cmd,0,0x23);	// READ FORMAT CAPACITIES
		scsi_command_init(cmd,8,12);
		scsi_command_init(cmd,9,0);
		if ((err=scsi_command_transport(cmd,READ,formats,12))) {
			/*sperror ("READ FORMAT CAPACITIES",err);*/
			fprintf (stderr, "Could not determine drive profile: "
				 "Error reading format capacities\n");
			goto bail;
		}
		
		len = formats[3];
		if (len&7 || len<16) {
			fprintf (stderr, "Could not determine drive profile: "
				 "Format allocation length isn't sane\n");
			goto bail;
		}
		
		scsi_command_init(cmd,0,0x23);	// READ FORMAT CAPACITIES
		scsi_command_init(cmd,7,(4+len)>>8);
		scsi_command_init(cmd,8,(4+len)&0xFF);
		scsi_command_init(cmd,9,0);
		if ((err=scsi_command_transport(cmd,READ,formats,4+len))) {
			/*sperror ("READ FORMAT CAPACITIES",err);*/
			fprintf (stderr, "Could not determine drive profile: "
				 "Error reading format capacities (2)\n");
			goto bail;
		}
		
		if (len != formats[3]) {
			fprintf (stderr, "Could not determine drive profile: "
				 "parameter length inconsistency\n");
			goto bail;
		}
		
		// see if it's not formatted
		if ((formats[8]&3) != 2) blank = 1;
		
		retval = profile;
	}
	
	/* Only touch isblank if we have read the profile correctly. */
	if ( retval != -1 ) {
		*isblank = blank;
	}
bail:
	  return retval;
}

int
get_disc_type (int fd)
{
	ScsiCommand *cmd;
	int retval = -1;
	unsigned char header[8];

	cmd = scsi_command_new_from_fd (fd);

	scsi_command_init (cmd, 0, 0x46);
	scsi_command_init (cmd, 1, 1);
	scsi_command_init (cmd, 8, 8);
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, header, 8)) {
		int isblank;
		/* GET CONFIGURATION failed */
		scsi_command_free (cmd);
		/* Try alternative method */
		retval = get_mmc_profile(fd,&isblank);
		return retval;	/* get_mmc_profile returns -1 on error too. */
	}
	
	retval = (header[6]<<8)|(header[7]);

	scsi_command_free (cmd);
	return retval;
}


int
disc_is_appendable (int fd)
{
	ScsiCommand *cmd;
	int retval = -1;
	unsigned char header[32];

	cmd = scsi_command_new_from_fd (fd);

	/* see section 5.19 of MMC-3 from http://www.t10.org/drafts.htm#mmc3 */
	scsi_command_init (cmd, 0, 0x51); /* READ_DISC_INFORMATION */
	scsi_command_init (cmd, 8, 32);
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, header, 32)) {
		/* READ_DISC_INFORMATION failed */
		scsi_command_free (cmd);
		return 0;
	}
	
	retval = ((header[2]&0x03) == 0x01);

	scsi_command_free (cmd);
	return retval;
}

int
disc_is_rewritable (int fd)
{
	ScsiCommand *cmd;
	int retval = -1;
	unsigned char header[32];

	cmd = scsi_command_new_from_fd (fd);

	/* see section 5.19 of MMC-3 from http://www.t10.org/drafts.htm#mmc3 */
	scsi_command_init (cmd, 0, 0x51); /* READ_DISC_INFORMATION */
	scsi_command_init (cmd, 8, 32);
	scsi_command_init (cmd, 9, 0);
	if (scsi_command_transport (cmd, READ, header, 32)) {
		/* READ_DISC_INFORMATION failed */
		scsi_command_free (cmd);
		return 0;
	}
	
	retval = ((header[2]&0x10) != 0);

	scsi_command_free (cmd);
	return retval;
}
