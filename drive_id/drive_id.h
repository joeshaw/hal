/*
 * drive_id - reads drive model and serial number
 *
 * Copyright (C) 2005 Kay Sievers <kay.sievers@vrfy.org>
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

#ifndef _DRIVE_ID_H_
#define _DRIVE_ID_H_

#define DRIVE_ID_VERSION		4

#define DRIVE_ID_VENDOR_SIZE		8
#define DRIVE_ID_MODEL_SIZE		40
#define DRIVE_ID_SERIAL_SIZE		20
#define DRIVE_ID_FW_SIZE		9
#define DRIVE_ID_REV_SIZE		12
#define DRIVE_ID_PATH_MAX		256

struct drive_id {
	char *type;
	unsigned char	vendor[DRIVE_ID_VENDOR_SIZE+1];
	unsigned char	model[DRIVE_ID_MODEL_SIZE+1];
	unsigned char	serial[DRIVE_ID_SERIAL_SIZE+1];
	unsigned char	firmware[DRIVE_ID_FW_SIZE+1];
	unsigned char	revision[DRIVE_ID_REV_SIZE+1];
	int		fd;
	int		fd_close;
};

/* open drive by already open file descriptor */
extern struct drive_id *drive_id_open_fd(int fd);

/* open drive by device node */
extern struct drive_id *drive_id_open_node(const char *path);

/* open drive by major/minor */
extern struct drive_id *drive_id_open_dev_t(dev_t devt);

/* probe drive for model/serial number */
extern int drive_id_probe_all(struct drive_id *id);

/* free allocated device info */
extern void drive_id_close(struct drive_id *id);

#endif /* _DRIVE_ID_H_ */
