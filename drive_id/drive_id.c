/*
 * drive_id - reads drive model and serial number
 *
 * Copyright (C) 2005 Kay Sievers <kay.sievers@vrfy.org>
 *
 * 	Get vendor, model, serial number, firmware version of
 * 	ATA or SCSI devices.
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
#define _GNU_SOURCE 1
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
#include "logging.h"
#include "util.h"

#include "scsi.h"
#include "ata.h"

int drive_id_probe_all(struct drive_id *id)
{
	if (id == NULL)
		return -1;

	if (drive_id_probe_ata(id) == 0)
		goto exit;
	if (drive_id_probe_scsi(id) == 0)
		goto exit;

	return -1;
	
exit:
	return 0;
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
		 "/dev/.drive_id-%u-%u-%u", getpid(), major(devt), minor(devt));
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
