/***************************************************************************
 * CVSID: $Id$
 *
 * hald_conf.c : Global configuration for hal daemon
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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
#include "logger.h"

#define HALD_CONF_FILE PACKAGE_SYSCONF_DIR "/hal/hald.conf"

static HaldConf hald_conf = {
	TRUE, /* storage.media_check_enabled */
	TRUE, /* storage.automount_enabled */
	TRUE  /* storage.cdrom.eject_check_enabled */
};

HaldConf *
hald_get_conf (void)
{
	return &hald_conf;
}

/* @todo this code is one big HACK - please rewrite it properly */

/** Maximum amount of CDATA */
#define CDATA_BUF_SIZE  1024

/** Max length of property key */
#define ELEM_BUF_SIZE  256

typedef struct {
	XML_Parser parser;

	dbus_bool_t failed;

	char elem[ELEM_BUF_SIZE];

	char cdata_buf[CDATA_BUF_SIZE];

	int cdata_buf_len;

} ParsingContext;


static void
parsing_abort (ParsingContext * pc)
{
	/* Grr, expat can't abort parsing */
	HAL_ERROR (("Aborting parsing of document"));
	pc->failed = TRUE;
}

static void
start (ParsingContext * pc, const char *el, const char **attr)
{
	if (pc->failed)
		return;

	strncpy (pc->elem, el, ELEM_BUF_SIZE);

	pc->cdata_buf_len = 0;
}

static void
end (ParsingContext * pc, const char *el)
{
	char *key;
	char *value;
	if (pc->failed)
		return;

	pc->cdata_buf[pc->cdata_buf_len] = '\0';

	key = pc->elem;
	value = pc->cdata_buf;

	if ((strcmp (key, "storage_media_check_enabled") == 0) &&
	    (strcmp (value, "false") == 0)) {
		hald_conf.storage_media_check_enabled = FALSE;
	} else if ((strcmp (key, "storage_automount_enabled") == 0) &&
		   (strcmp (value, "false") == 0)) {
		hald_conf.storage_automount_enabled = FALSE;
	} else if ((strcmp (key, "storage_cdrom_eject_check_enabled") == 0) &&
		   (strcmp (value, "false") == 0)) {
		hald_conf.storage_cdrom_eject_check_enabled = FALSE;
	}

	pc->elem[0] = '\0';
	pc->cdata_buf[0] = '\0';
	pc->cdata_buf_len = 0;
}

static void
cdata (ParsingContext * pc, const char *s, int len)
{
	int bytes_left;
	int bytes_to_copy;

	if (pc->failed)
		return;

	bytes_left = CDATA_BUF_SIZE - pc->cdata_buf_len;
	if (len > bytes_left) {
		HAL_ERROR (("CDATA in element larger than %d",
			    CDATA_BUF_SIZE));
		parsing_abort (pc);
		return;
	}

	bytes_to_copy = len;
	if (bytes_to_copy > bytes_left)
		bytes_to_copy = bytes_left;

	if (bytes_to_copy > 0)
		memcpy (pc->cdata_buf + pc->cdata_buf_len, s,
			bytes_to_copy);

	pc->cdata_buf_len += bytes_to_copy;
}


void
hald_read_conf_file (void)
{
	int rc;
	FILE *file;
	int filesize;
	char *filebuf;
	XML_Parser parser;
	ParsingContext *parsing_context;

	/*HAL_INFO(("analysing file %s", buf)); */

	/* open file and read it into a buffer; it's a small file... */
	file = fopen (HALD_CONF_FILE, "r");
	if (file == NULL) {
		HAL_INFO (("Couldn't open " HALD_CONF_FILE));
		goto out;;
	}

	fseek (file, 0L, SEEK_END);
	filesize = (int) ftell (file);
	rewind (file);
	filebuf = (char *) malloc (filesize);
	if (filebuf == NULL) {
		perror ("malloc");
		goto out1;
	}
	fread (filebuf, sizeof (char), filesize, file);

	parser = XML_ParserCreate (NULL);

	/* initialize parsing context */
	parsing_context =
	    (ParsingContext *) malloc (sizeof (ParsingContext));
	if (parsing_context == NULL) {
		perror ("malloc");
		goto out2;
	}
	parsing_context->failed = FALSE;
	parsing_context->cdata_buf_len = 0;

	XML_SetElementHandler (parser,
			       (XML_StartElementHandler) start,
			       (XML_EndElementHandler) end);
	XML_SetCharacterDataHandler (parser,
				     (XML_CharacterDataHandler) cdata);
	XML_SetUserData (parser, parsing_context);

	rc = XML_Parse (parser, filebuf, filesize, 1);

	if (rc == 0) {
		/* error parsing document */
		HAL_ERROR (("Error parsing XML document " HALD_CONF_FILE " at line %d, "
			    "column %d : %s", 
			    XML_GetCurrentLineNumber (parser), 
			    XML_GetCurrentColumnNumber (parser), 
			    XML_ErrorString (XML_GetErrorCode (parser))));
	}

	free (parsing_context);
out2:
	free (filebuf);
out1:
	fclose (file);
out:
}
