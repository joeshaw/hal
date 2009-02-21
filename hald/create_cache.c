/***************************************************************************
 * CVSID: $Id$
 *
 * create_cache.c : create FDI cache.
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2006 Kay Sievers, <kay.sievers@vrfy.org>
 * Copyright (C) 2006 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2007 Mikhail Kshevetskiy <mikhail.kshevetskiy@gmail.com>
 * Copyright (C) 2007 Sergey Lapin <slapinid@gmail.com>
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


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <syslog.h>
#include <expat.h>
#include <glib.h>
#include <config.h>

#include "logger.h"
#include "rule.h"

static XML_Parser parser;
static char s_error[256];
static int haldc_verbose = 0;

/* ctx of the current fdi file used for parsing */
struct fdi_context {
	int		depth;
	u_int32_t	match_at_depth[HAL_MAX_INDENT_DEPTH];
	struct rule	rule;
	off_t		position;
	int		cache_fd;
};

/* modified alphasort to count downwards */
static int
#ifdef __GLIBC__
_alphasort(const void *a, const void *b)
#else
_alphasort(const struct dirent **a, const struct dirent **b)
#endif
{
	return -alphasort (a, b);
}


static rule_type
get_rule_type (const char *str)
{
	if (strcmp (str, "match") == 0)
		return RULE_MATCH;
	if (strcmp (str, "merge") == 0)
		return RULE_MERGE;
	if (strcmp (str, "append") == 0)
		return RULE_APPEND;
	if (strcmp (str, "prepend") == 0)
		return RULE_PREPEND;
	if (strcmp (str, "addset") == 0)
		return RULE_ADDSET;
	if (strcmp (str, "remove") == 0)
		return RULE_REMOVE;
	if (strcmp (str, "clear") == 0)
		return RULE_CLEAR;
	if (strcmp (str, "spawn") == 0)
		return RULE_SPAWN;
	return RULE_UNKNOWN;
}

static match_type
get_match_type(const char *str)
{
	if (strcmp (str, "string") == 0)
		return MATCH_STRING;
	if (strcmp (str, "int") == 0)
		return MATCH_INT;
	if (strcmp (str, "uint64") == 0)
		return MATCH_UINT64;
	if (strcmp (str, "bool") == 0)
		return MATCH_BOOL;
	if (strcmp (str, "exists") == 0)
		return MATCH_EXISTS;
	if (strcmp (str, "empty") == 0)
		return MATCH_EMPTY;
	if (strcmp (str, "is_ascii") == 0)
		return MATCH_ISASCII;
	if (strcmp (str, "is_absolute_path") == 0)
		return MATCH_IS_ABS_PATH;
	if (strcmp (str, "sibling_contains") == 0)
		return MATCH_SIBLING_CONTAINS;
	if (strcmp (str, "contains") == 0)
		return MATCH_CONTAINS;
	if (strcmp (str, "contains_ncase") == 0)
		return MATCH_CONTAINS_NCASE;
	if (strcmp (str, "prefix") == 0)
		return MATCH_PREFIX;
	if (strcmp (str, "prefix_ncase") == 0)
		return MATCH_PREFIX_NCASE;
	if (strcmp (str, "suffix") == 0)
		return MATCH_SUFFIX;
	if (strcmp (str, "suffix_ncase") == 0)
		return MATCH_SUFFIX_NCASE;
	if (strcmp (str, "compare_lt") == 0)
		return MATCH_COMPARE_LT;
	if (strcmp (str, "compare_le") == 0)
		return MATCH_COMPARE_LE;
	if (strcmp (str, "compare_gt") == 0)
		return MATCH_COMPARE_GT;
	if (strcmp (str, "compare_ge") == 0)
		return MATCH_COMPARE_GE;
	if (strcmp (str, "compare_ne") == 0)
		return MATCH_COMPARE_NE;
	if (strcmp (str, "contains_not") == 0)
		return MATCH_CONTAINS_NOT;
	if (strcmp (str, "contains_outof") == 0)
		return MATCH_CONTAINS_OUTOF;
	if (strcmp (str, "int_outof") == 0)
		return MATCH_INT_OUTOF;
	if (strcmp (str, "prefix_outof") == 0)
		return MATCH_PREFIX_OUTOF;
	if (strcmp (str, "string_outof") == 0)
		return MATCH_STRING_OUTOF;
	return MATCH_UNKNOWN;
}

static merge_type
get_merge_type (const char *str)
{
	if (strcmp (str, "string") == 0)
		return MERGE_STRING;
	if (strcmp (str, "bool") == 0)
		return MERGE_BOOLEAN;
	if (strcmp (str, "int") == 0)
		return MERGE_INT32;
	if (strcmp (str, "uint64") == 0)
		return MERGE_UINT64;
	if (strcmp (str, "double") == 0)
		return MERGE_DOUBLE;
	if (strcmp (str, "strlist") == 0)
		return MERGE_STRLIST;
	if (strcmp (str, "copy_property") == 0)
		return MERGE_COPY_PROPERTY;
	if (strcmp (str, "remove") == 0)
		return MERGE_REMOVE;
	return MERGE_UNKNOWN;
}

#define ROUND(len,align) ((len + align - 1) & -align)
#define ROUND32(len) ROUND(len, 4)
#define RULES_ROUND(off) ROUND(off, __alignof__(struct rule))

static void pad32_write(int fd, off_t offset, void *data, size_t len)
{
	ssize_t		result;
	static char	pad[3] = {0, 0, 0};

	lseek(fd, offset, SEEK_SET);
	result = write(fd, data, len);

	if (result != (ssize_t) len)
		DIE(("Disk write error"));

	if ((offset + len) & 0x03) {
		result = write(fd, pad, 4 - ((offset + len) & 0x03));

		if (result != (ssize_t) (4 - ((offset + len) & 0x03)))
			DIE(("Disk write error"));
	}
}

static void init_rule_struct(struct rule *rule)
{
	memset(rule, 0, sizeof(struct rule));
	rule->rule_size = sizeof(struct rule);
	rule->rtype = RULE_UNKNOWN;
	rule->type_match = MATCH_UNKNOWN;
	rule->type_merge = MERGE_UNKNOWN;
	rule->value_offset = offsetof(struct cache_header, empty_string);
}

/* stores key string to data file which we'll mmap next */
static void store_key(struct fdi_context *fdi_ctx, const char *key)
{
	if (fdi_ctx->rule.rtype == RULE_UNKNOWN)
		DIE(("I refuse to store garbage"));

	if ((key == NULL) || (*key == '\0'))
		DIE(("Key is not valid"));

	fdi_ctx->rule.key_len = strlen(key) + 1;

	pad32_write(fdi_ctx->cache_fd, fdi_ctx->position + sizeof(struct rule),
	    (void*)key, fdi_ctx->rule.key_len);

	if (haldc_verbose)
		HAL_INFO(("Storing key '%s' at rule=%08lx", key, fdi_ctx->position));
}

/* stores value string to data file which we'll mmap next */
static void store_value(struct fdi_context *fdi_ctx, const char *value, size_t value_len)
{
	off_t	offset;
	char * p;

	if (fdi_ctx->rule.rtype == RULE_UNKNOWN)
		DIE(("I refuse to store garbage"));

	if (fdi_ctx->rule.key_len < 2)
		DIE(("Key length is too small to be true"));

	if ((value_len == 0) || (value == NULL) || (*value == '\0')) return;

	offset = fdi_ctx->position + sizeof(struct rule) + ROUND32(fdi_ctx->rule.key_len);

	if (fdi_ctx->rule.value_len == 0) {
		fdi_ctx->rule.value_offset = offset;
		fdi_ctx->rule.value_len = value_len + 1;
	}
	else {
		offset += fdi_ctx->rule.value_len - 1;
		fdi_ctx->rule.value_len += value_len;
	}

	pad32_write(fdi_ctx->cache_fd, offset, (void*)value, value_len);
	pad32_write(fdi_ctx->cache_fd, offset + value_len, "", 1);

	p = malloc(value_len + 1);

	if(!p)
	    DIE(("Could not allocate %lu bytes", (unsigned long) value_len + 1));

	memcpy(p, value, value_len);
	p[value_len] = '\0';

	if (haldc_verbose)
		HAL_INFO(("Storing value '%s', value_len=%d, at rule=%08lx, offset=%08lx",
		  p, value_len, fdi_ctx->position, offset));

	free(p);
}

static void store_rule(struct fdi_context *fdi_ctx)
{
	if (fdi_ctx->rule.rtype == RULE_UNKNOWN)
		DIE(("I refuse to store garbage"));

	fdi_ctx->rule.rule_size =
	  RULES_ROUND(sizeof(struct rule) +
		      ROUND32(fdi_ctx->rule.key_len) +
		      ROUND32(fdi_ctx->rule.value_len));

	pad32_write(fdi_ctx->cache_fd, fdi_ctx->position,
		&fdi_ctx->rule, sizeof(struct rule));

	if (haldc_verbose) {
		HAL_INFO(("rule=%08lx, rule_size=%d, rtype=%d",
		fdi_ctx->position, fdi_ctx->rule.rule_size, fdi_ctx->rule.rtype));

		HAL_INFO(("  jump_position=%08lx", fdi_ctx->rule.jump_position));

		HAL_INFO(("  key_len=%d, key_offset=%08lx",
			fdi_ctx->rule.key_len, fdi_ctx->position + offsetof(struct rule, key)));

		HAL_INFO(("  value_len=%d, value_offset=%08lx",
			fdi_ctx->rule.value_len, fdi_ctx->rule.value_offset));
	}

	init_rule_struct(&fdi_ctx->rule);
}

static void remember_jump_position(struct fdi_context *fdi_ctx)
{
	if (fdi_ctx->depth >= HAL_MAX_INDENT_DEPTH)
		DIE(("Rule depth overflow"));
	fdi_ctx->match_at_depth[fdi_ctx->depth++] = fdi_ctx->position;
}

static void set_jump_position(struct fdi_context *fdi_ctx)
{
	off_t	offset;
	u_int32_t offset32;

	if (fdi_ctx->depth <= 0)
		DIE(("Rule depth underrun"));

	fdi_ctx->depth--;
	offset = RULES_ROUND(lseek(fdi_ctx->cache_fd, 0, SEEK_END));
	offset32 = (u_int32_t)offset;
	pad32_write(fdi_ctx->cache_fd,
		fdi_ctx->match_at_depth[fdi_ctx->depth] + offsetof(struct rule, jump_position),
		&offset32, sizeof(fdi_ctx->rule.jump_position));

	if (haldc_verbose)
		HAL_INFO(("modify rule=0x%08x, set jump to 0x%08x",
			fdi_ctx->match_at_depth[fdi_ctx->depth], offset));

}

/* expat cb for start, e.g. <match foo=bar */
static void
start (void *data, const char *el, const char **attr)
{
	struct fdi_context	*fdi_ctx = data;
	int			i;

	/* we found a new tag, but old rule was not saved yet */
	if (fdi_ctx->rule.rtype != RULE_UNKNOWN)
		store_rule(fdi_ctx);

	init_rule_struct(&fdi_ctx->rule);
	fdi_ctx->rule.rtype = get_rule_type(el);
	fdi_ctx->position = RULES_ROUND(lseek(fdi_ctx->cache_fd, 0, SEEK_END));
	if (fdi_ctx->rule.rtype == RULE_UNKNOWN) return;

	/* get key and attribute for current rule */
	for (i = 0; attr[i] != NULL; i+=2) {
		if (strcmp (attr[i], "key") == 0) {
			if (fdi_ctx->rule.key_len > 0) {
				snprintf (s_error, sizeof (s_error), "Bad rule: key already defined");
				XML_StopParser (parser, FALSE);
				return;
			}

			store_key(fdi_ctx, attr[i + 1]);
			continue;
		}
		if (fdi_ctx->rule.rtype == RULE_SPAWN) {
			if (strcmp(attr[i], "udi") == 0) {
				if (fdi_ctx->rule.key_len > 0) {
					snprintf (s_error, sizeof (s_error), "Bad rule: key already defined");
					XML_StopParser (parser, FALSE);
					return;
				}

				store_key(fdi_ctx, attr[i + 1]);
				continue;
			}
			/* TODO: is it error ??? */
			continue;
		} else if (fdi_ctx->rule.rtype == RULE_MATCH) {

			if (fdi_ctx->rule.key_len == 0) {
				snprintf (s_error, sizeof (s_error), "Bad rule: value without a key");
				XML_StopParser (parser, FALSE);
				return;
			}

			fdi_ctx->rule.type_match = get_match_type(attr[i]);

			if (fdi_ctx->rule.type_match == MATCH_UNKNOWN) {
				snprintf (s_error, sizeof (s_error), "Bad rule: unknown type_match");
				XML_StopParser (parser, FALSE);
				return;
			}

			store_value(fdi_ctx, attr[i + 1], strlen(attr[i + 1]));
			continue;
		} else {
			if (strcmp(attr[i], "type") != 0){
				/* TODO: is it error ??? */
				continue;
			}
			fdi_ctx->rule.type_merge = get_merge_type(attr[i + 1]);

			if (fdi_ctx->rule.type_merge == MERGE_UNKNOWN) {
				snprintf (s_error, sizeof (s_error), "Bad rule: unknown type_merge");
				XML_StopParser (parser, FALSE);
				return;
			}

			continue;
		}
	}

	if (fdi_ctx->rule.key_len == 0) {
		snprintf (s_error, sizeof (s_error), "Bad rule: key not found");
		XML_StopParser (parser, FALSE);
		return;
	}

	/* match rules remember the current nesting and
	   the label to jump to if not matching */
	if (fdi_ctx->rule.rtype == RULE_MATCH)
		remember_jump_position(fdi_ctx);

	return;
}

/* expat cb for character data, i.e. the text data in between tags */
static void
cdata (void *data, const char *s, int len){
	struct fdi_context *fdi_ctx = data;

	if (fdi_ctx->rule.rtype != RULE_MERGE &&
	    fdi_ctx->rule.rtype != RULE_PREPEND &&
	    fdi_ctx->rule.rtype != RULE_ADDSET &&
	    fdi_ctx->rule.rtype != RULE_APPEND &&
	    fdi_ctx->rule.rtype != RULE_REMOVE &&
	    fdi_ctx->rule.rtype != RULE_SPAWN)
		return;

	store_value(fdi_ctx, s, len);
}

/* expat cb for end, e.g. </device> */
static void
end (void *data, const char *el){
	struct fdi_context *fdi_ctx = data;
	rule_type rtype = get_rule_type(el);

	if (rtype == RULE_UNKNOWN) return;
	if (rtype == RULE_MATCH){
		if (fdi_ctx->rule.rtype == RULE_MATCH) {
			/* the match rule wasn't stored yet, store it now. So it's stored
			* _before_ jump_position is written into the cache */
			store_rule(fdi_ctx);
		}
		set_jump_position(fdi_ctx);
		return;
	}

	/* only valid if element is in the current context */
	if (fdi_ctx->rule.rtype != rtype) {
		snprintf (s_error, sizeof (s_error), "Unexpected tag '%s'", el);
		XML_StopParser (parser, FALSE);
		return;
	}
	store_rule(fdi_ctx);
}

/* decompile an fdi file into a list of rules as this is quicker than opening then each time we want to search */
static int
rules_add_fdi_file (const char *filename, int fd)
{
	struct fdi_context *fdi_ctx;
	char *buf;
	gsize buflen;
	int rc;
	int ret;

	ret = -1;

	if (!g_file_get_contents (filename, &buf, &buflen, NULL))
		goto out;

	/* create new context */
	fdi_ctx = g_new0 (struct fdi_context, 1);
	memset(fdi_ctx, 0, sizeof(struct fdi_context));
	init_rule_struct(&fdi_ctx->rule);
	fdi_ctx->cache_fd = fd;

	parser = XML_ParserCreate (NULL);
	if (parser == NULL) {
		HAL_ERROR (("Couldn't allocate memory for parser"));
		g_free (fdi_ctx);
		goto out;
	}
	XML_SetUserData (parser, fdi_ctx);
	XML_SetElementHandler (parser, start, end);
	XML_SetCharacterDataHandler (parser, cdata);
	rc = XML_Parse (parser, buf, buflen, 1);
	if (rc == 0) {
		if (XML_GetErrorCode (parser) == XML_ERROR_ABORTED) {
			HAL_ERROR (("%s:%d: semantic error: %s",
				    filename,
				    (int) XML_GetCurrentLineNumber (parser),
				    s_error));
			syslog (LOG_ERR,
				"error in fdi file %s:%d: %s",
				filename,
				(int) XML_GetCurrentLineNumber (parser),
				s_error);

		} else {
			HAL_ERROR (("%s:%d: XML parse error: %s",
				    filename,
				    (int) XML_GetCurrentLineNumber (parser),
				    XML_ErrorString (XML_GetErrorCode (parser))));
			syslog (LOG_ERR,
				"error in fdi file %s:%d: %s",
				filename,
				(int) XML_GetCurrentLineNumber (parser),
				XML_ErrorString (XML_GetErrorCode (parser)));
		}

		XML_ParserFree (parser);
		g_free (buf);
		g_free (fdi_ctx);
		goto out;
	}
	XML_ParserFree (parser);
	g_free (buf);

	/* insert last dummy rule into list */
	init_rule_struct(&fdi_ctx->rule);
	fdi_ctx->rule.rtype = RULE_EOF;
	fdi_ctx->position = RULES_ROUND(lseek(fdi_ctx->cache_fd, 0, SEEK_END));
	store_key(fdi_ctx, filename);
	store_value(fdi_ctx, "", 0);
	store_rule(fdi_ctx);
	g_free (fdi_ctx);
	ret = lseek(fd, 0, SEEK_END);
out:
	return ret;

}


/* recurse a directory tree, searching and adding fdi files - returns
 * number of skipped fdi files or -1 on unrecoverable errors
 */
static int
rules_search_and_add_fdi_files (const char *dir, int fd)
{
	int i;
	int num_entries;
	struct dirent **name_list;
	int num_skipped_fdi_files;

	num_skipped_fdi_files = 0;

	num_entries = scandir (dir, &name_list, NULL, _alphasort);
	if (num_entries == -1) {
		HAL_ERROR (("Cannot scan '%s': %s", dir, strerror (errno)));
		goto error;
	}

	for (i = num_entries - 1; i >= 0; i--) {
		int len;
		char *filename;
		gchar *full_path;

		filename = name_list[i]->d_name;
		len = strlen (filename);
		full_path = g_strdup_printf ("%s/%s", dir, filename);
		if (g_file_test (full_path, (G_FILE_TEST_IS_REGULAR))) {
			if (len >= 5 && strcmp(&filename[len - 4], ".fdi") == 0) {
				int fdi_rules_size;
				off_t offset_before;
				offset_before = lseek (fd, 0, SEEK_CUR);
				fdi_rules_size = rules_add_fdi_file (full_path, fd);
				if (fdi_rules_size < 0) {
					HAL_ERROR (("error processing fdi file '%s'", full_path));
					/* try to just skip this file */
					if (ftruncate (fd, offset_before) != 0) {
						HAL_ERROR (("Cannot truncate rules fdi"));
						goto error;
					}
					lseek (fd, 0, SEEK_END);
					HAL_INFO (("skipped fdi file '%s'", full_path));
					num_skipped_fdi_files++;
				}
			}
		} else if (g_file_test (full_path, (G_FILE_TEST_IS_DIR)) && filename[0] != '.') {
			int num_bytes;
			char *dirname;
			int ret;

			num_bytes = len + strlen (dir) + 1 + 1;
			dirname = (char *) malloc (num_bytes);
			if (dirname == NULL)
				break;

			snprintf (dirname, num_bytes, "%s/%s", dir, filename);
			ret = rules_search_and_add_fdi_files (dirname, fd);
			if (ret == -1)
				goto error;
			num_skipped_fdi_files += ret;
			free (dirname);
		}
		g_free (full_path);
		free (name_list[i]);
	}

	for (; i >= 0; i--) {
		free (name_list[i]);
	}
	free (name_list);

	return num_skipped_fdi_files;
error:
	return -1;
}


/* returns number of skipped fdi files or -1 on unrecoverable errors */
static int
di_rules_init (void)
{
	char * cachename;
	int fd = -1;
	struct cache_header header;
	gchar *cachename_temp;
	char *hal_fdi_source_preprobe = getenv ("HAL_FDI_SOURCE_PREPROBE");
	char *hal_fdi_source_information = getenv ("HAL_FDI_SOURCE_INFORMATION");
	char *hal_fdi_source_policy = getenv ("HAL_FDI_SOURCE_POLICY");
	int n;
	int num_skipped_fdi_files;

	num_skipped_fdi_files = 0;

	cachename = getenv ("HAL_FDI_CACHE_NAME");
	if(cachename == NULL)
		cachename = HALD_CACHE_FILE;
	if (haldc_verbose)
		HAL_INFO (("Loading rules"));

	cachename_temp = g_strconcat (cachename, "~", NULL);

	fd = open(cachename_temp, O_CREAT|O_RDWR|O_TRUNC, 0644);
	if(fd < 0) {
		HAL_ERROR (("Unable to open fdi cache '%s' file for writing: %s", cachename_temp, strerror(errno)));
		goto error;
	}

	memset(&header, 0, sizeof(struct cache_header));
	pad32_write(fd, 0, &header, sizeof(struct cache_header));

	header.fdi_rules_preprobe = RULES_ROUND(lseek(fd, 0, SEEK_END));
	if (hal_fdi_source_preprobe != NULL) {
		if ((n = rules_search_and_add_fdi_files (hal_fdi_source_preprobe, fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
	} else {
		if ((n = rules_search_and_add_fdi_files (PACKAGE_DATA_DIR "/hal/fdi/preprobe", fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
		if ((n = rules_search_and_add_fdi_files (PACKAGE_SYSCONF_DIR "/hal/fdi/preprobe", fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
	}

	header.fdi_rules_information = RULES_ROUND(lseek(fd, 0, SEEK_END));
	if (hal_fdi_source_information != NULL) {
		if ((n = rules_search_and_add_fdi_files (hal_fdi_source_information, fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
	} else {
		if ((n = rules_search_and_add_fdi_files (PACKAGE_DATA_DIR "/hal/fdi/information", fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
		if ((n = rules_search_and_add_fdi_files (PACKAGE_SYSCONF_DIR "/hal/fdi/information", fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
	}

	header.fdi_rules_policy = RULES_ROUND(lseek(fd, 0, SEEK_END));
	if (hal_fdi_source_policy != NULL) {
		if ((n = rules_search_and_add_fdi_files (hal_fdi_source_policy, fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
	} else {
		if ((n = rules_search_and_add_fdi_files (PACKAGE_DATA_DIR "/hal/fdi/policy", fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
		if ((n = rules_search_and_add_fdi_files (PACKAGE_SYSCONF_DIR "/hal/fdi/policy", fd)) == -1)
			goto error;
		num_skipped_fdi_files += n;
	}

	header.all_rules_size = lseek(fd, 0, SEEK_END);
	pad32_write(fd, 0, &header, sizeof(struct cache_header));
	close(fd);
	if (rename (cachename_temp, cachename) != 0) {
		HAL_ERROR (("Cannot rename '%s' to '%s': %s", cachename_temp, cachename, strerror (errno)));
		goto error;
	}

	if (haldc_verbose){
		HAL_INFO(("preprobe: offset=%08lx, size=%d", header.fdi_rules_preprobe,
			header.fdi_rules_information - header.fdi_rules_preprobe));
		HAL_INFO(("information: offset=%08lx, size=%d", header.fdi_rules_information,
			header.fdi_rules_policy - header.fdi_rules_information));
		HAL_INFO(("policy: offset=%08lx, size=%d", header.fdi_rules_policy,
			header.all_rules_size - header.fdi_rules_policy));
		HAL_INFO (("Generating rules done (occupying %d bytes)", header.all_rules_size));
	}

	g_free (cachename_temp);
	return num_skipped_fdi_files;
error:
	HAL_ERROR (("Error generating fdi cache"));
	if (fd >= 0)
		close (fd);

	unlink (cachename_temp);
	g_free (cachename_temp);
	return -1;
}

/**
 * usage:
 *
 * Print out program usage.
 *
 */
static void
usage (void)
{
	fprintf (stderr, "\n" "usage : hald-generate-fdi-cache [OPTION]\n");
	fprintf (stderr,
		 "\n"
		 "	--help		Show this information and exit.\n"
		 "	--verbose	Show verbose rule processing output.\n"
		 "	--version	Output version information and exit.\n"
		 "\n"
		 "hald-generate-fdi-cache is a tool to generate binary cache from FDI files.\n"
		 "\n"
		 "For more information visit http://freedesktop.org/Software/hal\n"
		 "\n");
}


int main(int argc, char * argv[])
{
	int num_skipped_fdi_files;
	openlog ("hald", LOG_PID, LOG_DAEMON);

	while (1) {
		int c;
		int option_index = 0;
		const char *opt;
		static struct option long_options[] = {
			{"help", 0, NULL, 0},
			{"version", 0, NULL, 0},
			{"verbose", 0, NULL, 0},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long (argc, argv, "",
				 long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			opt = long_options[option_index].name;

			if (strcmp (opt, "help") == 0) {
				usage ();
				return 0;
			} else if (strcmp (opt, "version") == 0) {
				fprintf (stderr, "HAL package version: " PACKAGE_VERSION "\n");
				return 0;
			} else if (strcmp (opt, "verbose") == 0) {
				haldc_verbose = 1;
			}
			break;

		default:
			usage ();
			return 1;
			break;
		}
	}

	num_skipped_fdi_files = di_rules_init();
	if (num_skipped_fdi_files == 0) {
		/* no skipped fdi files */
		return 0;
	} else if (num_skipped_fdi_files == -1) {
		/* error */
		return 1;
	} else {
		/* skipped some fdi files */
		fprintf (stderr, "Skipped %d fdi files\n", num_skipped_fdi_files);
		return 2;
	}
}
