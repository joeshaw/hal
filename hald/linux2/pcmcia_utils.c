/***************************************************************************
 * CVSID: $Id$
 *
 * PCMCIA Card utilities
 *
 * Copyright (C) 2003 Dan Williams <dcbw@redhat.com>
 *
 * Licensed under the Academic Free License version 2.0
 *
 * Some of this code was derived from pcmcia-cs code, originally
 * developed by David A. Hinds <dahinds@users.sourceforge.net>.
 * Portions created by David A. Hinds are Copyright (C) 1999 David A. Hinds.
 * All Rights Reserved.  It has been modified for integration into HAL
 * by Dan Williams and is covered by the GPL version 2 license.
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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "pcmcia_utils.h"

static char *pcmcia_card_types[] = {
	"multifunction", "memory", "serial", "parallel",
	"fixed disk", "video", "network", "AIMS", "SCSI"
};

static int pcmcia_major = -1;

static int pcmcia_lookup_dev(void)
{
	FILE *f;
	int n;
	char s[32], t[32];
    
	f = fopen("/proc/devices", "r");
	if (f == NULL)
		return -errno;
	while (fgets(s, 32, f) != NULL) {
		if (sscanf(s, "%d %s", &n, t) == 2)
			if (strcmp("pcmcia", t) == 0)
				break;
	}
	fclose(f);
	if (strcmp ("pcmcia", t) == 0)
		return n;
	else
		return -ENODEV;
}

int pcmcia_socket_open (int socket)
{
	static char *paths[] = {
		"/dev", "/var/run", "/tmp", NULL
	};
	int fd;
	char **p, fn[64];
	dev_t dev;

	if ((socket < 0) || (socket > PCMCIA_MAX_SOCKETS))
		return -1;

	if (pcmcia_major < 0)
	{
		pcmcia_major = pcmcia_lookup_dev();
		if (pcmcia_major < 0) {
			if (pcmcia_major == -ENODEV)
				fprintf (stderr, "no pcmcia driver in /proc/devices\n");
			else
				fprintf (stderr, "could not open /proc/devices");
		}
	}
	dev = (pcmcia_major<<8) + socket;

	for (p = paths; *p; p++) {
		sprintf(fn, "%s/hal-%d", *p, getpid());
		if (mknod(fn, (S_IFCHR | S_IREAD | S_IWRITE), dev) == 0) {
			fd = open(fn, O_RDONLY);
			unlink(fn);
			if (fd >= 0)
				return fd;
			if (errno == ENODEV)
				break;
		}
	}
	return -1;
}


int pcmcia_get_tuple(int fd, cisdata_t code, ds_ioctl_arg_t *arg)
{
	arg->tuple.DesiredTuple = code;
	arg->tuple.Attributes = TUPLE_RETURN_COMMON;
	arg->tuple.TupleOffset = 0;
	if (    (ioctl(fd, DS_GET_FIRST_TUPLE, arg) == 0)
		&& (ioctl(fd, DS_GET_TUPLE_DATA, arg) == 0)
		&& (ioctl(fd, DS_PARSE_TUPLE, arg) == 0))
		return 0;
	else
		return -1;
}


pcmcia_card_info *pcmcia_card_info_get (int socket)
{
	int fd;
	pcmcia_card_info *info = NULL;

	if ((socket < 0) || (socket > PCMCIA_MAX_SOCKETS))
		return NULL;

	fd = pcmcia_socket_open (socket);
	if (fd >= 0) {
		ds_ioctl_arg_t   arg;
		cistpl_vers_1_t *vers = &arg.tuple_parse.parse.version_1;
		cistpl_manfid_t *manfid = &arg.tuple_parse.parse.manfid;
		cistpl_funcid_t *funcid = &arg.tuple_parse.parse.funcid;
		config_info_t    config;

		/* Ignore CardBus cards and empty slots */
		if (ioctl(fd, DS_GET_CONFIGURATION_INFO, &config) == 0)
			goto out;
		if (config.IntType == INT_CARDBUS)
			goto out;

		if (pcmcia_get_tuple(fd, CISTPL_VERS_1, &arg) != 0)
			goto out;

		if (!(info = calloc (1, sizeof (pcmcia_card_info))))
			goto out;

		info->socket = socket;

		if (vers->ns >= 1)
			info->productid_1 = strdup (vers->str+vers->ofs[0]);
		if (vers->ns >= 2)
			info->productid_2 = strdup (vers->str+vers->ofs[1]);
		if (vers->ns >= 3)
			info->productid_3 = strdup (vers->str+vers->ofs[2]);
		if (vers->ns >= 4)
			info->productid_4 = strdup (vers->str+vers->ofs[3]);

		if (pcmcia_get_tuple(fd, CISTPL_FUNCID, &arg) == 0)
			info->type = funcid->func;

		if (pcmcia_get_tuple(fd, CISTPL_MANFID, &arg) == 0) {
			info->manfid_1 = manfid->manf;
			info->manfid_2 = manfid->card;
		}
		close (fd);
	}

out:
	return (info);
}


void pcmcia_card_info_free (pcmcia_card_info *info)
{
	if (!info) return;

	info->socket = -1;
	info->type = PCMCIA_TYPE_INVALID;
	free (info->productid_1);
	free (info->productid_2);
	free (info->productid_3);
	free (info->productid_4);
	info->manfid_1 = -1;
	info->manfid_2 = -1;
}

const char *pcmcia_card_type_string_from_type (const PCMCIA_card_type type)
{
	/* basically, ensure we don't go out of the array's bounds */
	if ((type < PCMCIA_TYPE_MULTIFUNCTION) || (type >= PCMCIA_TYPE_INVALID)) return NULL;

	return pcmcia_card_types[type];
}

PCMCIA_card_type pcmcia_card_type_from_type_string (const char *string)
{
	PCMCIA_card_type i;

	if (!string) return PCMCIA_TYPE_INVALID;

	for (i = PCMCIA_TYPE_MULTIFUNCTION; i < PCMCIA_TYPE_INVALID; i++) {
		if (!strcmp (string, pcmcia_card_types[i]))
			return i;
	}
	return PCMCIA_TYPE_INVALID;
}


static inline int whack_newline (char *buf)
{
	int len;

	if (!buf)
		return 0;

	len = strlen (buf);
	if ((buf[len-1] == '\n') || (buf[len-1] == '\r')) {
		buf[len-1] = '\0';
		len--;
	}

	return len;
}


pcmcia_stab_entry *pcmcia_stab_entry_get (int socket)
{
	FILE *f;
	pcmcia_stab_entry *entry = NULL;

	if ((socket < 0) || (socket > PCMCIA_MAX_SOCKETS))
		return NULL;

	if ((f = fopen (PCMCIA_STAB_FILE, "r"))) {
		char	buf[200];

		while (fgets (buf, 200, f) && !feof (f)) {
			char match[50];

			buf[199] = '\0';
			whack_newline (buf);

			snprintf (match, 49, "Socket %d", socket);
			if (!strncmp (buf, match, strlen (match))) {
				/* Ok, found our socket */
				if (fgets (buf, 200, f) && !feof (f)) {
					buf[199] = '\0';
					whack_newline (buf);
					if (strncmp (buf, "Socket", 6)) {
						/* Got our card */
						int s;
						char func[50];
						char driver[50];
						int g;
						char dev[50];

						if (sscanf (buf, "%d\t%s\t%s\t%d\t%s", &s, &func, &driver, &g, &dev) == 5) {
							PCMCIA_card_type t = pcmcia_card_type_from_type_string (func);
							if (t != PCMCIA_TYPE_INVALID) {
								entry = calloc (1, sizeof (pcmcia_stab_entry));
								entry->socket = s;
								entry->type = t;
								entry->driver = strdup (driver);
								entry->dev = strdup (dev);
							}
							break;
						}
					}
				}
			}
		}
		fclose (f);
	}

	return (entry);
}


void pcmcia_stab_entry_free (pcmcia_stab_entry *entry)
{
	if (!entry) return;

	entry->socket = -1;
	entry->type = PCMCIA_TYPE_INVALID;
	free (entry->driver);
	free (entry->dev);
}


pcmcia_stab_entry *pcmcia_get_stab_entry_for_device (const char *interface)
{
	pcmcia_stab_entry *entry = NULL;
	int i;

	if (!interface) return 0;

	for (i = 0; i < PCMCIA_MAX_SOCKETS; i++)
	{
		if ((entry = pcmcia_stab_entry_get (i)))
		{
			if (!strcmp (interface, entry->dev))
				break;
			pcmcia_stab_entry_free (entry);
		}
	}

	return (entry);
}

