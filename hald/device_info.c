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
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "logger.h"
#include "device_info.h"

/**
 * @defgroup DeviceInfo Device Info File Parsing
 * @ingroup HalDaemon
 * @brief Parsing of device info files
 * @{
 */


/** Maximum nesting depth */
#define MAX_DEPTH 32

/** Maximum amount of CDATA */
#define CDATA_BUF_SIZE  1024

/** Max length of property key */
#define MAX_KEY_SIZE 128

/** Possible elements the parser can process */
enum {
	/** Not processing a known tag */
	CURELEM_UNKNOWN = -1,

	/** Processing a deviceinfo element */
	CURELEM_DEVICE_INFO = 0,

	/** Processing a device element */
	CURELEM_DEVICE = 1,

	/** Processing a match element */
	CURELEM_MATCH = 2,

	/** Processing a merge element */
	CURELEM_MERGE = 3,
};

/** Parsing Context
 */
typedef struct {
	/** Name of file being parsed */
	char *file;

	/** Parser object */
	XML_Parser parser;

	/** Device we are trying to match*/
	HalDevice *device;

	/** Buffer to put CDATA in */
	char cdata_buf[CDATA_BUF_SIZE];

	/** Current length of CDATA buffer */
	int cdata_buf_len;
	
	/** Current depth we are parsing at */
	int depth;

	/** Element currently being processed */
	int curelem;

	/** Stack of elements being processed */
	int curelem_stack[MAX_DEPTH];

	/** #TRUE if parsing of document have been aborted */
	dbus_bool_t aborted;


	/** Depth of match-fail */
	int match_depth_first_fail;

	/** #TRUE if all matches on prior depths have been OK */
	dbus_bool_t match_ok;



	/** When merging, the key to store the value in */
	char merge_key[MAX_KEY_SIZE];

	/** Type to merge*/
	int merge_type;

	/** Set to #TRUE if a device is matched */
	dbus_bool_t device_matched;

} ParsingContext;

/** Called when the match element begins.
 *
 *  @param  pc                  Parsing context
 *  @param  attr                Attribute key/value pairs
 *  @return                     #FALSE if the device in question didn't
 *                              match the data in the attributes
 */
static dbus_bool_t
handle_match (ParsingContext * pc, const char **attr)
{
	const char *key;
	int num_attrib;

	for (num_attrib = 0; attr[num_attrib] != NULL; num_attrib++);

	if (num_attrib != 4)
		return FALSE;

	if (strcmp (attr[0], "key") != 0)
		return FALSE;
	key = attr[1];

	if (strcmp (attr[2], "string") == 0) {
		const char *value;

		/* match string property */

		value = attr[3];

		/*HAL_INFO(("Checking that key='%s' is a string that "
		  "equals '%s'", key, value)); */

		if (hal_device_property_get_type (pc->device, key) != DBUS_TYPE_STRING)
			return FALSE;

		if (strcmp (hal_device_property_get_string (pc->device, key),
			    value) != 0)
			return FALSE;

		HAL_INFO (("*** string match for key %s", key));
		return TRUE;
	} else if (strcmp (attr[2], "int") == 0) {
		dbus_int32_t value;

		/* match integer property */
		value = strtol (attr[3], NULL, 0);
		
		/** @todo Check error condition */

		HAL_INFO (("Checking that key='%s' is a int that equals %d", 
			   key, value));

		if (hal_device_property_get_type (pc->device, key) != DBUS_TYPE_INT32)
			return FALSE;

		if (hal_device_property_get_int (pc->device, key) != value) {
			return FALSE;
		}

		return TRUE;
	} else if (strcmp (attr[2], "bool") == 0) {
		dbus_bool_t value;

		/* match string property */

		if (strcmp (attr[3], "false") == 0)
			value = FALSE;
		else if (strcmp (attr[3], "true") == 0)
			value = TRUE;
		else
			return FALSE;

		HAL_INFO (("Checking that key='%s' is a bool that equals %s", 
			   key, value ? "TRUE" : "FALSE"));

		if (hal_device_property_get_type (pc->device, key) != 
		    DBUS_TYPE_BOOLEAN)
			return FALSE;

		if (hal_device_property_get_bool (pc->device, key) != value)
			return FALSE;

		HAL_INFO (("*** bool match for key %s", key));
		return TRUE;
	}

	return FALSE;
}


/** Called when the merge element begins.
 *
 *  @param  pc                  Parsing context
 *  @param  attr                Attribute key/value pairs
 */
static void
handle_merge (ParsingContext * pc, const char **attr)
{
	int num_attrib;

	for (num_attrib = 0; attr[num_attrib] != NULL; num_attrib++) {
		;
	}

	if (num_attrib != 4)
		return;

	if (strcmp (attr[0], "key") != 0)
		return;
	strncpy (pc->merge_key, attr[1], MAX_KEY_SIZE);

	if (strcmp (attr[2], "type") != 0)
		return;

	if (strcmp (attr[3], "string") == 0) {
		/* match string property */
		pc->merge_type = DBUS_TYPE_STRING;
		return;
	} else if (strcmp (attr[3], "bool") == 0) {
		/* match string property */
		pc->merge_type = DBUS_TYPE_BOOLEAN;
		return;
	} else if (strcmp (attr[3], "int") == 0) {
		/* match string property */
		pc->merge_type = DBUS_TYPE_INT32;
		return;
	} else if (strcmp (attr[3], "double") == 0) {
		/* match string property */
		pc->merge_type = DBUS_TYPE_DOUBLE;
		return;
	}

	return;
}

/** Abort parsing of document
 *
 *  @param  pc                  Parsing context
 */
static void
parsing_abort (ParsingContext * pc)
{
	/* Grr, expat can't abort parsing */
	HAL_ERROR (("Aborting parsing of document"));
	pc->aborted = TRUE;
}

/** Called by expat when an element begins.
 *
 *  @param  pc                  Parsing context
 *  @param  el                  Element name
 *  @param  attr                Attribute key/value pairs
 */
static void
start (ParsingContext * pc, const char *el, const char **attr)
{
	if (pc->aborted)
		return;

	pc->cdata_buf_len = 0;

/*
    for (i = 0; i < pc->depth; i++)
        printf("  ");
    
    printf("%s", el);
    
    for (i = 0; attr[i]; i += 2) {
        printf(" %s='%s'", attr[i], attr[i + 1]);
    }

    printf("   curelem=%d\n", pc->curelem);
*/

	if (strcmp (el, "match") == 0) {
		if (pc->curelem != CURELEM_DEVICE
		    && pc->curelem != CURELEM_MATCH) {
			HAL_ERROR (("%s:%d:%d: Element <match> can only be "
				    "inside <device> and <match>", 
				    pc->file, 
				    XML_GetCurrentLineNumber (pc->parser), 
				    XML_GetCurrentColumnNumber (pc->parser)));
			parsing_abort (pc);
		}

		pc->curelem = CURELEM_MATCH;
		/* don't bother checking if matching at lower depths failed */
		if (pc->match_ok) {
			if (!handle_match (pc, attr)) {
				/* No match */
				pc->match_depth_first_fail = pc->depth;
				pc->match_ok = FALSE;
			}
		}
	} else if (strcmp (el, "merge") == 0) {
		if (pc->curelem != CURELEM_DEVICE
		    && pc->curelem != CURELEM_MATCH) {
			HAL_ERROR (("%s:%d:%d: Element <merge> can only be "
				    "inside <device> and <match>", 
				    pc->file, 
				    XML_GetCurrentLineNumber (pc->parser), 
				    XML_GetCurrentColumnNumber (pc->parser)));
			parsing_abort (pc);
		}

		pc->curelem = CURELEM_MERGE;
		if (pc->match_ok) {
			handle_merge (pc, attr);
		} else {
			/*HAL_INFO(("No merge!")); */
		}
	} else if (strcmp (el, "device") == 0) {
		if (pc->curelem != CURELEM_DEVICE_INFO) {
			HAL_ERROR (("%s:%d:%d: Element <device> can only be "
				    "inside <deviceinfo>", 
				    pc->file, 
				    XML_GetCurrentLineNumber (pc->parser), 
				    XML_GetCurrentColumnNumber (pc->parser)));
			parsing_abort (pc);
		}
		pc->curelem = CURELEM_DEVICE;
	} else if (strcmp (el, "deviceinfo") == 0) {
		if (pc->curelem != CURELEM_UNKNOWN) {
			HAL_ERROR (("%s:%d:%d: Element <deviceinfo> must be "
				    "a top-level element", 
				    pc->file, 
				    XML_GetCurrentLineNumber (pc->parser), 
				    XML_GetCurrentColumnNumber (pc->parser)));
			parsing_abort (pc);
		}
		pc->curelem = CURELEM_DEVICE_INFO;
	} else {
		HAL_ERROR (("%s:%d:%d: Unknown element <%s>",
			    pc->file,
			    XML_GetCurrentLineNumber (pc->parser),
			    XML_GetCurrentColumnNumber (pc->parser), el));
		parsing_abort (pc);
	}

	/* Nasty hack */
	assert (pc->depth < MAX_DEPTH);

	pc->depth++;

	/* store depth */
	pc->curelem_stack[pc->depth] = pc->curelem;

}

/** Called by expat when an element ends.
 *
 *  @param  pc                  Parsing context
 *  @param  el                  Element name
 */
static void
end (ParsingContext * pc, const char *el)
{
	if (pc->aborted)
		return;

	pc->cdata_buf[pc->cdata_buf_len] = '\0';

/*    printf("   curelem=%d\n", pc->curelem);*/

	if (pc->curelem == CURELEM_MERGE && pc->match_ok) {
		/* As soon as we are merging, we have matched the device... */
		pc->device_matched = TRUE;

		switch (pc->merge_type) {
		case DBUS_TYPE_STRING:
			hal_device_property_set_string (pc->device, pc->merge_key,
						pc->cdata_buf);
			break;


		case DBUS_TYPE_INT32:
			{
				dbus_int32_t value;

				/* match integer property */
				value = strtol (pc->cdata_buf, NULL, 0);

				/** @todo FIXME: Check error condition */

				hal_device_property_set_int (pc->device,
						     pc->merge_key, value);
				break;
			}

		case DBUS_TYPE_BOOLEAN:
			hal_device_property_set_bool (pc->device, pc->merge_key,
					      (strcmp (pc->cdata_buf,
						       "true") == 0) 
					      ? TRUE : FALSE);
			break;

		case DBUS_TYPE_DOUBLE:
			hal_device_property_set_double (pc->device, pc->merge_key,
						atof (pc->cdata_buf));
			break;

		default:
			HAL_ERROR (("Unknown merge_type=%d='%c'",
				    pc->merge_type, pc->merge_type));
			break;
		}
	}


	pc->cdata_buf_len = 0;
	pc->depth--;

	/* maintain curelem */
	pc->curelem = pc->curelem_stack[pc->depth];

	/* maintain pc->match_ok */
	if (pc->depth < pc->match_depth_first_fail)
		pc->match_ok = TRUE;
}

/** Called when there is CDATA 
 *
 *  @param  pc                  Parsing context
 *  @param  s                   Pointer to data
 *  @param  len                 Length of data
 */
static void
cdata (ParsingContext * pc, const char *s, int len)
{
	int bytes_left;
	int bytes_to_copy;

	if (pc->aborted)
		return;

	bytes_left = CDATA_BUF_SIZE - pc->cdata_buf_len;
	if (len > bytes_left) {
		HAL_ERROR (("CDATA in element larger than %d",
			    CDATA_BUF_SIZE));
	}

	bytes_to_copy = len;
	if (bytes_to_copy > bytes_left)
		bytes_to_copy = bytes_left;

	if (bytes_to_copy > 0)
		memcpy (pc->cdata_buf + pc->cdata_buf_len, s,
			bytes_to_copy);

	pc->cdata_buf_len += bytes_to_copy;
}


/** Process a device information info file.
 *
 *  @param  dir                 Directory file resides in
 *  @param  filename            File name
 *  @param  device              Device to match on
 *  @return                     #TRUE if file matched device and information
 *                              was merged
 */
static dbus_bool_t
process_fdi_file (const char *dir, const char *filename,
		  HalDevice * device)
{
	int rc;
	char buf[512];
	FILE *file;
	int filesize;
	char *filebuf;
	dbus_bool_t device_matched;
	XML_Parser parser;
	ParsingContext *parsing_context;

	snprintf (buf, 511, "%s/%s", dir, filename);

	/*HAL_INFO(("analysing file %s", buf)); */

	/* open file and read it into a buffer; it's a small file... */
	file = fopen (buf, "r");
	if (file == NULL) {
		perror ("fopen");
		return FALSE;
	}

	fseek (file, 0L, SEEK_END);
	filesize = (int) ftell (file);
	rewind (file);
	filebuf = (char *) malloc (filesize);
	if (filebuf == NULL) {
		perror ("malloc");
		fclose (file);
		return FALSE;
	}
	fread (filebuf, sizeof (char), filesize, file);


	/* ok, now parse the file (should probably reuse parser and say we are
	 * not thread safe 
	 */
	parser = XML_ParserCreate (NULL);

	/* initialize parsing context */
	parsing_context =
	    (ParsingContext *) malloc (sizeof (ParsingContext));
	if (parsing_context == NULL) {
		perror ("malloc");
		return FALSE;
	}
	parsing_context->depth = 0;
	parsing_context->device_matched = FALSE;
	parsing_context->match_ok = TRUE;
	parsing_context->curelem = CURELEM_UNKNOWN;
	parsing_context->aborted = FALSE;
	parsing_context->file = buf;
	parsing_context->parser = parser;
	parsing_context->device = device;


	XML_SetElementHandler (parser,
			       (XML_StartElementHandler) start,
			       (XML_EndElementHandler) end);
	XML_SetCharacterDataHandler (parser,
				     (XML_CharacterDataHandler) cdata);
	XML_SetUserData (parser, parsing_context);

	rc = XML_Parse (parser, filebuf, filesize, 1);
	/*printf("XML_Parse rc=%d\r\n", rc); */

	if (rc == 0) {
		/* error parsing document */
		HAL_ERROR (("Error parsing XML document %s at line %d, "
			    "column %d : %s", 
			    buf, 
			    XML_GetCurrentLineNumber (parser), 
			    XML_GetCurrentColumnNumber (parser), 
			    XML_ErrorString (XML_GetErrorCode (parser))));
		device_matched = FALSE;
	} else {
		/* document parsed ok */
		device_matched = parsing_context->device_matched;
	}

	free (filebuf);
	fclose (file);
	free (parsing_context);

	return device_matched;
}



/** Scan all directories and subdirectories in the given directory and
 *  process each *.fdi file
 *
 *  @param  d                   Device to merge information into
 *  @return                     #TRUE if information was merged
 */
static int
scan_fdi_files (const char *dir, HalDevice * d)
{
	int i;
	int num_entries;
	dbus_bool_t found_fdi_file;
	struct dirent **name_list;

	found_fdi_file = 0;

	/*HAL_INFO(("scan_fdi_files: Processing dir '%s'", dir)); */

	num_entries = scandir (dir, &name_list, 0, alphasort);
	if (num_entries == -1) {
		perror ("scandir");
		return FALSE;
	}

	for (i = num_entries - 1; i >= 0; i--) {
		int len;
		char *filename;

		filename = name_list[i]->d_name;
		len = strlen (filename);

		if (name_list[i]->d_type == DT_REG) {
			/* regular file */

			if (len >= 5 &&
			    filename[len - 4] == '.' &&
			    filename[len - 3] == 'f' &&
			    filename[len - 2] == 'd' &&
			    filename[len - 1] == 'i') {
				HAL_INFO (("scan_fdi_files: Processing "
					   "file '%s'", filename));
				found_fdi_file =
				    process_fdi_file (dir, filename, d);
				if (found_fdi_file) {
					HAL_INFO (("*** Matched file %s/%s", 
						   dir, filename));
					break;
				}
			}

		} else if (name_list[i]->d_type == DT_DIR &&
			   strcmp (filename, ".") != 0
			   && strcmp (filename, "..") != 0) {
			int num_bytes;
			char *dirname;

			/* Directory; do the recursion thingy but not 
			 * for . and ..
			 */

			num_bytes = len + strlen (dir) + 1 + 1;
			dirname = (char *) malloc (num_bytes);
			if (dirname == NULL) {
				HAL_ERROR (("couldn't allocated %d bytes",
					    num_bytes));
				break;
			}

			snprintf (dirname, num_bytes, "%s/%s", dir,
				  filename);
			found_fdi_file = scan_fdi_files (dirname, d);
			free (dirname);
			if (found_fdi_file)
				break;
		}

		free (name_list[i]);
	}

	for (; i >= 0; i--) {
		free (name_list[i]);
	}

	free (name_list);

	return found_fdi_file;
}


/** Search the device info file repository for a .fdi file to merge
 *  more information into the device object.
 *
 *  @param  d                   Device to merge information into
 *  @return                     #TRUE if information was merged
 */
dbus_bool_t
di_search_and_merge (HalDevice *d)
{
	return scan_fdi_files (PACKAGE_DATA_DIR "/hal/fdi", d);
}

/** @} */
