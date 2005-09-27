/*
 * drive_id - reads drive model and serial number
 *
 * Copyright (C) 2005 Kay Sievers <kay.sievers@vrfy.org>
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
#include <stdint.h>

#include "drive_id.h"
#include "logging.h"
#include "util.h"
#include "ata.h"

int drive_id_probe_ata(struct drive_id *id)
{
	struct hd_driveid ata_id;

	if (ioctl(id->fd, HDIO_GET_IDENTITY, &ata_id) != 0)
		return -1;

	set_str((char *) id->model, ata_id.model, 40);
	set_str((char *) id->serial, ata_id.serial_no, 20);
	set_str((char *) id->firmware, ata_id.fw_rev, 8);

	return 0;
}
