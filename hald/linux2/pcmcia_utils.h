/***************************************************************************
 * CVSID: $Id$
 *
 * PCMCIA Card utilities
 *
 * Copyright (C) 2003 Dan Williams <dcbw@redhat.com>
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

#ifndef PCMCIA_UTILS_H
#define PCMCIA_UTILS_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdint.h>

#include "pcmcia_cs.h"

#define PCMCIA_MAX_SOCKETS 8


typedef enum PCMCIA_card_type {
	PCMCIA_TYPE_MULTIFUNCTION = 0,
	PCMCIA_TYPE_MEMORY,
	PCMCIA_TYPE_SERIAL,
	PCMCIA_TYPE_PARALLEL,
	PCMCIA_TYPE_FIXED_DISK,
	PCMCIA_TYPE_VIDEO,
	PCMCIA_TYPE_NETWORK,
	PCMCIA_TYPE_AIMS,
	PCMCIA_TYPE_SCSI,
	PCMCIA_TYPE_INVALID	/* should always be last */
} PCMCIA_card_type;


typedef struct pcmcia_stab_entry {
	int				socket;
	PCMCIA_card_type	type;
	char				*driver;
	char				*dev;
} pcmcia_stab_entry;

typedef struct pcmcia_card_info {
	int				socket;
	PCMCIA_card_type	type;
	char				*productid_1;
	char				*productid_2;
	char				*productid_3;
	char				*productid_4;
	int				manfid_1;
	int				manfid_2;
} pcmcia_card_info;


int pcmcia_socket_open (int socket);
int pcmcia_get_tuple(int fd, cisdata_t code, ds_ioctl_arg_t *arg);

const char *pcmcia_card_type_string_from_type (const PCMCIA_card_type type);
PCMCIA_card_type pcmcia_card_type_from_type_string (const char *string);

pcmcia_stab_entry *pcmcia_stab_entry_get (int socket);
void pcmcia_stab_entry_free (pcmcia_stab_entry *entry);
pcmcia_stab_entry *pcmcia_get_stab_entry_for_device (const char *interface);

pcmcia_card_info *pcmcia_card_info_get (int socket);
void pcmcia_card_info_free (pcmcia_card_info *info);

#endif
