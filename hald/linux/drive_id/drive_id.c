/*
 * drive_id - reads drive model and serial number
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 *
 * 	Try to get vendor, model, serial number, firmware version
 *	of ATA or SCSI devices.
 *	
 *	Note:	Native interface access is needed. There is no way to get
 *		these kind from a device behind a USB adapter. A good
 *		bridge reads these values from the device and provides them
 *		as USB config strings.
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
#include <ctype.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/stat.h>
#include <scsi/sg.h>
#include <linux/hdreg.h>
#include <asm/types.h>

#include "drive_id.h"

/*#define DEBUG*/
#ifdef DEBUG
#define dbg(format, arg...)						\
	do {								\
		printf("%s: " format "\n", __FUNCTION__ , ## arg);	\
	} while (0)
#else
#define dbg(format, arg...)	do {} while (0)
#endif /* DEBUG */

static inline void dump(unsigned char *buf, int len)
{
	int i;
	char *c = (char *) buf;
	for (i = 0; i < len; i++)
		printf("%c", c[i] );
}

static void set_str(char *to, const unsigned char *from, int count)
{
	int first;
	int last;

	/* remove leading whitespace */
	for (first = 0; isspace(from[first]) && (first < count); first++);

	memcpy(to, &from[first], count - first);

	/* remove trailing whitespace */
	last = strnlen(to, count);
	while (last--) {
		if (! isspace(to[last]))
			break;
	}
	to[last+1] = '\0';


}

#define INQUIRY_CMD_LEN		6
#define SENSE_BUF_LEN		32
#define INQUIRY			0x12
#define TIMEOUT			3000
static int scsi_inq(int fd,
		    unsigned char evpd, unsigned char page,
		    unsigned char *buf, int buf_len)
{
	unsigned char cmd[INQUIRY_CMD_LEN];
	unsigned char sense[SENSE_BUF_LEN];
	struct sg_io_hdr io_hdr;
	int rc;

	cmd[0] = INQUIRY;
	cmd[1] = evpd;
	cmd[2] = page;
	cmd[3] = 0;
	cmd[4] = buf_len;
	cmd[5] = 0;

	memset(&io_hdr, 0, sizeof(struct sg_io_hdr));
	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = sizeof(cmd);
	io_hdr.dxferp = buf;
	io_hdr.dxfer_len = buf_len;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.sbp = sense;
	io_hdr.mx_sb_len = sizeof(sense);
	io_hdr.timeout = TIMEOUT;

	rc = ioctl(fd, SG_IO, &io_hdr);
	if (rc != 0) {
		dbg("ioctl failed");
		return -1;
	}

	if (io_hdr.status != 0 ||
	    io_hdr.msg_status != 0 ||
	    io_hdr.host_status != 0 ||
	    io_hdr.driver_status != 0) {
		dbg("scsi status error");
		return -1;
	}

	if (buf[1] != page) {
		dbg("asked for page 0x%x, got page 0x%x", page, buf[1]);
		return -1;
	}

	return 0;
}

#define BUFFER_SIZE		256
static int probe_scsi(struct drive_id *id)
{
	unsigned char buf[BUFFER_SIZE];
	int len;
	int rc;

	/* get vendor product */
	memset(buf, 0, BUFFER_SIZE);
	rc = scsi_inq(id->fd, 0, 0x00, buf, 255);
	if (rc != 0) {
		dbg("inquiry page 0x00 failed");
		return -1;
	}

	set_str(id->vendor, &buf[8], 8);
	set_str(id->model, &buf[16], 16);
	set_str(id->revision, &buf[32], 12);

	/* get serial number from page 0x80 */
	memset(buf, 0, BUFFER_SIZE);
	rc = scsi_inq(id->fd, 1, 0x80, buf, 255);
	if (rc != 0) {
		dbg("inquiry page 0x80 failed");
		return -1;
	}
	len = buf[3];
	dbg("page 0x80 serial number length %i", len);
	if (len > DRIVE_ID_SERIAL_SIZE)
		len = DRIVE_ID_SERIAL_SIZE;
	set_str(id->serial, &buf[4], len);

	return 0;
}

static int probe_ata(struct drive_id *id)
{
	struct hd_driveid ata_id;

	if (ioctl(id->fd, HDIO_GET_IDENTITY, &ata_id) != 0)
		return -1;

	set_str(id->model, ata_id.model, 40);
	set_str(id->serial, ata_id.serial_no, 20);
	set_str(id->firmware, ata_id.fw_rev, 8);

	return 0;
}

/* probe drive for model/serial number */
int drive_id_probe(struct drive_id *id, enum drive_type type)
{
	int rc;

	if (id == NULL)
		return -1;

	switch(type) {
	case DRIVE_ID_ATA:
		rc = probe_ata(id);
		break;
	case DRIVE_ID_SCSI:
		rc = probe_scsi(id);
		break;
	case DRIVE_ID_ALL:
	default:
		rc = probe_ata(id);
		if (rc == 0)
			break;
		rc = probe_scsi(id);
		if (rc == 0)
			break;
	}

	return rc;
}

/* open drive by already open file descriptor */
struct drive_id *drive_id_open_fd(int fd)
{
	struct drive_id *id;

	id = malloc(sizeof(struct drive_id));
	if (id == NULL)
		return NULL;
	memset(id, 0x00, sizeof(struct drive_id));

	id->fd = fd;

	return id;
}

/* open drive by device node */
struct drive_id *drive_id_open_node(const char *path)
{
	struct drive_id *id;
	int fd;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		dbg("unable to open '%s'", path);
		return NULL;
	}

	id = drive_id_open_fd(fd);
	if (id == NULL)
		return NULL;

	/* close fd on device close */
	id->fd_close = 1;

	return id;
}

/* open drive by major/minor */
struct drive_id *drive_id_open_dev_t(dev_t devt)
{
	struct drive_id *id;
	__u8 tmp_node[DRIVE_ID_PATH_MAX];

	snprintf(tmp_node, DRIVE_ID_PATH_MAX,
		 "/tmp/drive-%u-%u-%u", getpid(), major(devt), minor(devt));
	tmp_node[DRIVE_ID_PATH_MAX] = '\0';

	/* create tempory node to open the block device */
	unlink(tmp_node);
	if (mknod(tmp_node, (S_IFBLK | 0600), devt) != 0)
		return NULL;

	id = drive_id_open_node(tmp_node);

	unlink(tmp_node);

	return id;
}

/* free allocated drive info */
void drive_id_close(struct drive_id *id)
{
	if (id == NULL)
		return;

	if (id->fd_close != 0)
		close(id->fd);

	free(id);
}
