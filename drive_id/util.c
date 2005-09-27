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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

#include "drive_id.h"
#include "logging.h"
#include "util.h"

void set_str(char *to, const unsigned char *from, int count)
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

