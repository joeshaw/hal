/***************************************************************************
 * CVSID: $Id$
 *
 * device_store.c : Search for .fdi files and merge on match
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.0
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <expat.h>
#include <assert.h>

#include "hald_conf.h"

static HaldConf hald_conf = {
	TRUE, /* storage.media_check_enabled */
	TRUE, /* storage.automount_enabled */
	TRUE  /* storage.cdrom.eject_check_enabled */
};

HaldConf* 
hald_get_conf (void)
{
	return &hald_conf;
}
