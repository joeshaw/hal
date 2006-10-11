/***************************************************************************
 * CVSID: $Id$
 *
 * device_store.c : Parse .fdi files and match/merge device properties.
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2006 Kay Sievers, <kay.sievers@vrfy.org>
 * Copyright (C) 2006 Richard Hughes <richard@hughsie.com>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <expat.h>
#include <assert.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <sys/stat.h>
#include <math.h>

#include "hald.h"
#include "logger.h"
#include "device_info.h"
#include "device_store.h"
#include "util.h"

#define MAX_INDENT_DEPTH		64

/* pre-parsed rules to keep in memory */
static GSList *fdi_rules_preprobe;
static GSList *fdi_rules_information;
static GSList *fdi_rules_policy;

/* rule type to process */
enum rule_type {
	RULE_UNKNOWN,
	RULE_MATCH,
	RULE_MERGE,
	RULE_APPEND,
	RULE_PREPEND,
	RULE_REMOVE,
	RULE_CLEAR,
	RULE_SPAWN,
	RULE_EOF,
};

/* type of merge command */
enum merge_type {
	MERGE_UNKNOWN,
	MERGE_STRING,
	MERGE_BOOLEAN,
	MERGE_INT32,
	MERGE_UINT64,
	MERGE_DOUBLE,
	MERGE_COPY_PROPERTY,
	MERGE_STRLIST,
	MERGE_REMOVE,
};

/* type of match command */
enum
match_type {
	MATCH_UNKNOWN,
	MATCH_STRING,
	MATCH_INT,
	MATCH_UINT64,
	MATCH_BOOL,
	MATCH_EXISTS,
	MATCH_EMPTY,
	MATCH_ISASCII,
	MATCH_IS_ABS_PATH,
	MATCH_CONTAINS,
	MATCH_CONTAINS_NCASE,
	MATCH_COMPARE_LT,
	MATCH_COMPARE_LE,
	MATCH_COMPARE_GT,
	MATCH_COMPARE_GE,
};

/* a "rule" structure that is a generic node of the fdi file */
struct rule {
	/* typ of tule in the list */
	enum rule_type rtype;

	/* all rules have a key */
	char *key;

	/* "match" or "merge" rule */
	enum match_type type_match;
	enum merge_type type_merge;

	char *value;
	int value_len;

	/* if rule does not match, skip to this rule */
	struct rule *next_rule;
};

/* ctx of the current fdi file used for parsing */
struct fdi_context {
	int depth;
	struct rule *match_at_depth[MAX_INDENT_DEPTH];

	/* current rule */
	struct rule *rule;

	int fdi_rule_size;

	/* all rules */
	GSList* rules;
};

static enum
rule_type get_rule_type (const char *str)
{
	if (strcmp (str, "match") == 0)
		return RULE_MATCH;
	if (strcmp (str, "merge") == 0)
		return RULE_MERGE;
	if (strcmp (str, "append") == 0)
		return RULE_APPEND;
	if (strcmp (str, "prepend") == 0)
		return RULE_PREPEND;
	if (strcmp (str, "remove") == 0)
		return RULE_REMOVE;
	if (strcmp (str, "clear") == 0)
		return RULE_CLEAR;
	if (strcmp (str, "spawn") == 0)
		return RULE_SPAWN;
	return RULE_UNKNOWN;
}

/*
static char *
get_rule_type_str (enum rule_type type)
{
	switch (type) {
	case RULE_MATCH:
		return "match";
	case RULE_MERGE:
		return "merge";
	case RULE_APPEND:
		return "append";
	case RULE_PREPEND:
		return "prepend";
	case RULE_REMOVE:
		return "remove";
	case RULE_CLEAR:
		return "clear";
	case RULE_SPAWN:
		return "spawn";
	case RULE_EOF:
		return "eof";
	case RULE_UNKNOWN:
		return "unknown rule type";
	}
	return "invalid rule type";
}
*/

static enum
merge_type get_merge_type (const char *str)
{
	if (strcmp (str, "string") == 0)
		return MERGE_STRING;
	if (strcmp (str, "bool") == 0)
		return MERGE_BOOLEAN;
	if (strcmp (str, "int") == 0)
		return MERGE_INT32;
	if (strcmp (str, "unint64") == 0)
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

/*
static char *
get_merge_type_str (enum merge_type type)
{
	switch (type) {
	case MERGE_STRING:
		return "string";
	case MERGE_BOOLEAN:
		return "bool";
	case MERGE_INT32:
		return "int";
	case MERGE_UINT64:
		return "unint64";
	case MERGE_DOUBLE:
		return "double";
	case MERGE_STRLIST:
		return "strlist";
	case MERGE_COPY_PROPERTY:
		return "copy_property";
	case MERGE_REMOVE:
		return "remove";
	case MERGE_UNKNOWN:
		return "unknown merge type";
	}
	return "invalid merge type";
}
*/

static enum
match_type get_match_type(const char *str)
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
	if (strcmp (str, "contains") == 0)
		return MATCH_CONTAINS;
	if (strcmp (str, "contains_ncase") == 0)
		return MATCH_CONTAINS_NCASE;
	if (strcmp (str, "compare_lt") == 0)
		return MATCH_COMPARE_LT;
	if (strcmp (str, "compare_le") == 0)
		return MATCH_COMPARE_LE;
	if (strcmp (str, "compare_gt") == 0)
		return MATCH_COMPARE_GT;
	if (strcmp (str, "compare_ge") == 0)
		return MATCH_COMPARE_GE;
	return MATCH_UNKNOWN;
}

/*
static char *
get_match_type_str (enum match_type type)
{
	switch (type) {
	case MATCH_STRING:
		return "string";
	case MATCH_INT:
		return "int";
	case MATCH_UINT64:
		return "uint64";
	case MATCH_BOOL:
		return "bool";
	case MATCH_EXISTS:
		return "exists";
	case MATCH_EMPTY:
		return "empty";
	case MATCH_ISASCII:
		return "is_ascii";
	case MATCH_IS_ABS_PATH:
		return "is_absolute_path";
	case MATCH_CONTAINS:
		return "contains";
	case MATCH_CONTAINS_NCASE:
		return "contains_ncase";
	case MATCH_COMPARE_LT:
		return "compare_lt";
	case MATCH_COMPARE_LE:
		return "compare_le";
	case MATCH_COMPARE_GT:
		return "compare_gt";
	case MATCH_COMPARE_GE:
		return "compare_ge";
	case MATCH_UNKNOWN:
		return "unknown match type";
	}
	return "invalid match type";
}
*/

/** Resolve a udi-property path as used in .fdi files.
 *
 *  Examples of udi-property paths:
 *
 *   info.udi
 *   /org/freedesktop/Hal/devices/computer:kernel.name
 *   @block.storage_device:storage.bus
 *   @block.storage_device:@storage.physical_device:ide.channel
 *
 *  @param  source_udi          UDI of source device
 *  @param  path                The given path
 *  @param  udi_result          Where to store the resulting UDI
 *  @param  udi_result_size     Size of UDI string
 *  @param  prop_result         Where to store the resulting property name
 *  @param  prop_result_size    Size of property string
 *  @return                     TRUE if and only if the path resolved.
 */
static gboolean
resolve_udiprop_path (const char *path, const char *source_udi,
		      char *udi_result, size_t udi_result_size,
		      char *prop_result, size_t prop_result_size)
{
	int i;
	gchar **tokens = NULL;
	gboolean rc = FALSE;

	/* Split up path into ':' tokens */
	tokens = g_strsplit (path, ":", 64);

	/* Detect trivial property access, e.g. path='foo.bar'   */
	if (tokens == NULL || tokens[0] == NULL || tokens[1] == NULL) {
		strncpy (udi_result, source_udi, udi_result_size);
		strncpy (prop_result, path, prop_result_size);
		rc = TRUE;
		goto out;
	}

	/* Start with the source udi */
	strncpy (udi_result, source_udi, udi_result_size);

	for (i = 0; tokens[i] != NULL; i++) {
		HalDevice *d;
		gchar *curtoken;

		/*HAL_INFO (("tokens[%d] = '%s'", i, tokens[i]));*/

		d = hal_device_store_find (hald_get_gdl (), udi_result);
		if (d == NULL)
			d = hal_device_store_find (hald_get_tdl (), udi_result);
		if (d == NULL)
			goto out;

		curtoken = tokens[i];

		/* process all but the last tokens as UDI paths */
		if (tokens[i+1] == NULL) {
			strncpy (prop_result, curtoken, prop_result_size);
			rc = TRUE;
			goto out;
		}


		/* Check for indirection */
		if (curtoken[0] == '@') {
			const char *udiprop;
			const char *newudi;

			udiprop = curtoken + 1;

			newudi = hal_device_property_get_string (d, udiprop);
			if (newudi == NULL)
				goto out;

			strncpy (udi_result, newudi, udi_result_size);
		} else {
			strncpy (udi_result, curtoken, udi_result_size);
		}

	}

out:
	g_strfreev (tokens);
	return rc;
}

/* Compare the value of a property on a hal device object against a string value
 * and return the result. Note that this works for several types, e.g. both strings
 * and integers - in the latter case the given right side string will be interpreted
 * as a number.
 *
 * The comparison might not make sense if you are comparing a property which is an integer
 * against a string in which case this function returns FALSE. Also, if the property doesn't
 * exist this function will also return FALSE.
 *
 * @param  d                    hal device object
 * @param  key                  Key of the property to compare
 * @param  right_side           Value to compare against
 * @param  result               Pointer to where to store result
 * @return                      TRUE if, and only if, the comparison could take place
 */
static gboolean
match_compare_property (HalDevice *d, const char *key, const char *right_side, dbus_int64_t *result)
{
	gboolean rc;
	int proptype;

	rc = FALSE;

	if (!hal_device_has_property (d, key))
		goto out;

	proptype = hal_device_property_get_type (d, key);
	switch (proptype) {
	case HAL_PROPERTY_TYPE_STRING:
		*result = (dbus_int64_t) strcmp (hal_device_property_get_string (d, key), right_side);
		rc = TRUE;
		break;

	case HAL_PROPERTY_TYPE_INT32:
		*result = ((dbus_int64_t) hal_device_property_get_int (d, key)) - strtoll (right_side, NULL, 0);
		rc = TRUE;
		break;

	case HAL_PROPERTY_TYPE_UINT64:
		*result = ((dbus_int64_t) hal_device_property_get_uint64 (d, key)) - ((dbus_int64_t) strtoll (right_side, NULL, 0));
		rc = TRUE;
		break;

	case HAL_PROPERTY_TYPE_DOUBLE:
		*result = (dbus_int64_t) ceil (hal_device_property_get_double (d, key) - atof (right_side));
		rc = TRUE;
		break;

	default:
		/* explicit fallthrough */
	case HAL_PROPERTY_TYPE_BOOLEAN:
		/* explicit blank since this doesn't make sense */
		break;
	}

out:
	return rc;
}

static gboolean
handle_match (struct rule *rule, HalDevice *d)
{
	char udi_to_check[HAL_PATH_MAX];
	char prop_to_check[HAL_PATH_MAX];
	const char *key = rule->key;
	const char *value = rule->value;

	/* Resolve key paths like 'someudi/foo/bar/baz:prop.name' '@prop.here.is.an.udi:with.prop.name' */
	if (!resolve_udiprop_path (key,
				   hal_device_get_udi (d),
				   udi_to_check, sizeof (udi_to_check),
				   prop_to_check, sizeof (prop_to_check))) {
		HAL_ERROR (("Could not resolve keypath '%s' on udi '%s'", key, value));
		return FALSE;
	}

	d = hal_device_store_find (hald_get_gdl (), udi_to_check);
	if (d == NULL)
		d = hal_device_store_find (hald_get_tdl (), udi_to_check);
	if (d == NULL) {
		HAL_ERROR (("Could not find device with udi '%s'", udi_to_check));
		return FALSE;
	}

	switch (rule->type_match) {
	case MATCH_STRING:
	{
		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_STRING)
			return FALSE;
		if (strcmp (hal_device_property_get_string (d, prop_to_check), value) != 0)
			return FALSE;
		return TRUE;
	}

	case MATCH_INT:
	{
		int val = strtol (value, NULL, 0);

		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_INT32)
			return FALSE;
		if (hal_device_property_get_int (d, prop_to_check) != val)
			return FALSE;
		return TRUE;
	}

	case MATCH_UINT64:
	{
		dbus_uint64_t val = strtol (value, NULL, 0);

		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_UINT64)
			return FALSE;
		if (hal_device_property_get_uint64 (d, prop_to_check) != val)
			return FALSE;
		return TRUE;
	}

	case MATCH_BOOL:
	{
		dbus_bool_t val;

		if (strcmp (value, "false") == 0)
			val = FALSE;
		else if (strcmp (value, "true") == 0)
			val = TRUE;
		else
			return FALSE;

		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_BOOLEAN)
			return FALSE;
		if (hal_device_property_get_bool (d, prop_to_check) != val)
			return FALSE;
		return TRUE;
	}

	case MATCH_EXISTS:
	{
		dbus_bool_t should_exist = TRUE;

		if (strcmp (value, "false") == 0)
			should_exist = FALSE;

		if (should_exist) {
			if (hal_device_has_property (d, prop_to_check))
				return TRUE;
			else
				return FALSE;
		} else {
			if (hal_device_has_property (d, prop_to_check))
				return FALSE;
			else
				return TRUE;
		}
	}

	case MATCH_EMPTY:
	{
		dbus_bool_t is_empty = TRUE;
		dbus_bool_t should_be_empty = TRUE;

		if (strcmp (value, "false") == 0)
			should_be_empty = FALSE;
		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_STRING)
			return FALSE;
		if (hal_device_has_property (d, prop_to_check))
			if (strlen (hal_device_property_get_string (d, prop_to_check)) > 0)
				is_empty = FALSE;

		if (should_be_empty) {
			if (is_empty)
				return TRUE;
			else
				return FALSE;
		} else {
			if (is_empty)
				return FALSE;
			else
				return TRUE;
		}
	}

	case MATCH_ISASCII:
	{
		dbus_bool_t is_ascii = TRUE;
		dbus_bool_t should_be_ascii = TRUE;
		unsigned int i;
		const char *str;

		if (strcmp (value, "false") == 0)
			should_be_ascii = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_STRING)
			return FALSE;

		is_ascii = TRUE;

		str = hal_device_property_get_string (d, prop_to_check);
		for (i = 0; str[i] != '\0'; i++) {
			if (((unsigned char) str[i]) > 0x7f)
				is_ascii = FALSE;
		}

		if (should_be_ascii) {
			if (is_ascii)
				return TRUE;
			else
				return FALSE;
		} else {
			if (is_ascii)
				return FALSE;
			else
				return TRUE;
		}
	}

	case MATCH_IS_ABS_PATH:
	{
		const char *path = NULL;
		dbus_bool_t is_absolute_path = FALSE;
		dbus_bool_t should_be_absolute_path = TRUE;

		if (strcmp (value, "false") == 0)
			should_be_absolute_path = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_STRING)
			return FALSE;

		if (hal_device_has_property (d, prop_to_check)) {
			path = hal_device_property_get_string (d, prop_to_check);
			if (g_path_is_absolute (path))
				is_absolute_path = TRUE;
		}

		if (should_be_absolute_path) {
			if (is_absolute_path)
				return TRUE;
			else
				return FALSE;
		} else {
			if (is_absolute_path)
				return FALSE;
			else
				return TRUE;
		}
	}

	case MATCH_CONTAINS:
	{
		dbus_bool_t contains = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
			if (hal_device_has_property (d, prop_to_check)) {
				const char *haystack;

				haystack = hal_device_property_get_string (d, prop_to_check);
				if (value != NULL && haystack != NULL && strstr (haystack, value))
					contains = TRUE;
			}
		} else if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRLIST && value != NULL) {
			HalDeviceStrListIter iter;
			for (hal_device_property_strlist_iter_init (d, prop_to_check, &iter);
			     hal_device_property_strlist_iter_is_valid (&iter);
			     hal_device_property_strlist_iter_next (&iter)) {
				const char *str = hal_device_property_strlist_iter_get_value (&iter);
				if (strcmp (str, value) == 0) {
					contains = TRUE;
					break;
				}
			}
		} else {
			return FALSE;
		}

		return contains;
	}

	case MATCH_CONTAINS_NCASE:
	{
		dbus_bool_t contains_ncase = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
			if (hal_device_has_property (d, prop_to_check)) {
				char *value_lowercase;
				char *haystack_lowercase;

				value_lowercase   = g_utf8_strdown (value, -1);
				haystack_lowercase = g_utf8_strdown (hal_device_property_get_string (d, prop_to_check), -1);
				if (value_lowercase != NULL && haystack_lowercase != NULL &&
				    strstr (haystack_lowercase, value_lowercase))
					contains_ncase = TRUE;

				g_free (value_lowercase);
				g_free (haystack_lowercase);
			}
		} else if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRLIST && value != NULL) {
			HalDeviceStrListIter iter;
			for (hal_device_property_strlist_iter_init (d, prop_to_check, &iter);
			     hal_device_property_strlist_iter_is_valid (&iter);
			     hal_device_property_strlist_iter_next (&iter)) {
				const char *str = hal_device_property_strlist_iter_get_value (&iter);
				if (g_ascii_strcasecmp (str, value) == 0) {
					contains_ncase = TRUE;
					break;
				}
			}
		} else
			return FALSE;
		return contains_ncase;
	}

	case MATCH_COMPARE_LT:
	{
		dbus_int64_t result;

		if (!match_compare_property (d, prop_to_check, value, &result))
			return FALSE;
		else
			return result < 0;
	}

	case MATCH_COMPARE_LE:
	{
		dbus_int64_t result;

		if (!match_compare_property (d, prop_to_check, value, &result))
			return FALSE;
		else
			return result <= 0;
	}

	case MATCH_COMPARE_GT:
	{
		dbus_int64_t result;

		if (!match_compare_property (d, prop_to_check, value, &result))
			return FALSE;
		else
			return result > 0;
	}

	case MATCH_COMPARE_GE:
	{
		dbus_int64_t result;

		if (!match_compare_property (d, prop_to_check, value, &result))
			return FALSE;
		else
			return result >= 0;
	}

	default:
		HAL_INFO(("match ERROR"));
		return FALSE;
	}

	return FALSE;
}

/* we have finished the callouts for a device, now add it to the gdl */
static void
spawned_device_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);
}

/* for this device, process the rule */
static gboolean
handle_merge (struct rule *rule, HalDevice *d)
{
	const char *key = rule->key;
	const char *value = rule->value;

	if (rule->rtype == RULE_MERGE) {

		if (rule->type_merge == MERGE_STRING) {
			hal_device_property_set_string (d, key, value);

		} else if (rule->type_merge == MERGE_STRLIST) {
			int type = hal_device_property_get_type (d, key);

			if (type == HAL_PROPERTY_TYPE_STRLIST || type == HAL_PROPERTY_TYPE_INVALID) {
				hal_device_property_remove (d, key);
				hal_device_property_strlist_append (d, key, value);
			}

		} else if (rule->type_merge == MERGE_INT32) {
			dbus_int32_t val = strtol (value, NULL, 0);
			hal_device_property_set_int (d, key, val);

		} else if (rule->type_merge == MERGE_UINT64) {
			dbus_uint64_t val = strtoull (value, NULL, 0);
			hal_device_property_set_uint64 (d, key, val);

		} else if (rule->type_merge == MERGE_BOOLEAN) {
			hal_device_property_set_bool (d, key, (strcmp (value, "true") == 0) ? TRUE : FALSE);

		} else if (rule->type_merge == MERGE_DOUBLE) {
			hal_device_property_set_double (d, key, atof (value));

		} else if (rule->type_merge == MERGE_COPY_PROPERTY) {

			char udi_to_merge_from[HAL_PATH_MAX];
			char prop_to_merge[HAL_PATH_MAX];

			/* Resolve key paths like 'someudi/foo/bar/baz:prop.name'
			 * '@prop.here.is.an.udi:with.prop.name'
			 */
			if (!resolve_udiprop_path (value,
						   hal_device_get_udi (d),
						   udi_to_merge_from, sizeof (udi_to_merge_from),
						   prop_to_merge, sizeof (prop_to_merge))) {
				HAL_ERROR (("Could not resolve keypath '%s' on udi '%s'", value, hal_device_get_udi (d)));
			} else {
				HalDevice *d;

				d = hal_device_store_find (hald_get_gdl (), udi_to_merge_from);
				if (d == NULL) {
					d = hal_device_store_find (hald_get_tdl (), udi_to_merge_from);
				}
				if (d == NULL) {
					HAL_ERROR (("Could not find device with udi '%s'", udi_to_merge_from));
				} else {
					hal_device_copy_property (d, prop_to_merge, d, key);
				}
			}

		} else {
			HAL_ERROR (("unknown merge type (%u)", rule->type_merge));
		}

	} else if (rule->rtype == RULE_APPEND) {
		char buf[HAL_PATH_MAX];
		char buf2[HAL_PATH_MAX];

		if (hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRING &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRLIST &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_INVALID) {
			HAL_ERROR (("invalid key type"));
			return FALSE;
		}

		if (rule->type_merge == MERGE_STRLIST) {
			hal_device_property_strlist_append (d, key, value);
		} else {
			const char *existing_string;

			switch (rule->type_merge) {
			case MERGE_STRING:
				strncpy (buf, value, sizeof (buf));
				break;

			case MERGE_COPY_PROPERTY:
				hal_device_property_get_as_string (d, value, buf, sizeof (buf));
				break;

			default:
				break;
			}

			existing_string = hal_device_property_get_string (d, key);
			if (existing_string != NULL) {
				strncpy (buf2, existing_string, sizeof (buf2));
				strncat (buf2, buf, sizeof (buf2) - strlen(buf2));
			} else
				strncpy (buf2, buf, sizeof (buf2));
			hal_device_property_set_string (d, key, buf2);
		}

	} else if (rule->rtype == RULE_PREPEND) {
		char buf[HAL_PATH_MAX];
		char buf2[HAL_PATH_MAX];

		if (hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRING &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRLIST &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_INVALID) {
			HAL_ERROR (("invalid key type"));
			return FALSE;
		}

		if (rule->type_merge == MERGE_STRLIST) {
			hal_device_property_strlist_prepend (d, key, value);
		} else {
			const char *existing_string;

			if (rule->type_merge == MERGE_STRING) {
				strncpy (buf, value, sizeof (buf));

			} else if (rule->type_merge == MERGE_COPY_PROPERTY) {
				hal_device_property_get_as_string (d, value, buf, sizeof (buf));

			}

			existing_string = hal_device_property_get_string (d, key);
			if (existing_string != NULL) {
				strncpy (buf2, buf, sizeof (buf2));
				strncat (buf2, existing_string, sizeof (buf2) - strlen(buf2));
			} else {
				strncpy (buf2, buf, sizeof (buf2));
			}
			hal_device_property_set_string (d, key, buf2);
		}

	} else if (rule->rtype == RULE_REMOVE) {

		if (rule->type_merge == MERGE_STRLIST) {
			/* covers <remove key="foobar" type="strlist">blah</remove> */
			hal_device_property_strlist_remove (d, key, value);

		} else {
			/* only allow <remove key="foobar"/>, not <remove key="foobar">blah</remove> */
			if (strlen (value) == 0)
				hal_device_property_remove (d, key);
		}

	} else if (rule->rtype == RULE_SPAWN) {
		HalDevice *spawned;
		spawned = hal_device_store_find (hald_get_gdl (), key);
		if (spawned == NULL)
			spawned = hal_device_store_find (hald_get_tdl (), key);

		if (spawned == NULL) {
			HAL_INFO (("Spawning new device object '%s' caused by <spawn> on udi '%s'",
				   key, hal_device_get_udi (d)));
			spawned = hal_device_new ();
			hal_device_property_set_string (spawned, "info.bus", "unknown");
			hal_device_property_set_string (spawned, "info.udi", key);
			hal_device_property_set_string (spawned, "info.parent", hal_device_get_udi (d));
			hal_device_set_udi (spawned, key);

			hal_device_store_add (hald_get_tdl (), spawned);

			di_search_and_merge (spawned, DEVICE_INFO_TYPE_INFORMATION);
			di_search_and_merge (spawned, DEVICE_INFO_TYPE_POLICY);

			hal_util_callout_device_add (spawned, spawned_device_callouts_add_done, NULL, NULL);
		}

	} else {
		HAL_ERROR (("Unknown rule type (%u)", rule->rtype));
		return FALSE;
	}
	return TRUE;
}

/* for each node, free memory and it's own list of nodes */
static void
rules_cleanup_list (GSList *fdi_rules)
{
	GSList *elem;

	for (elem = fdi_rules; elem != NULL; elem = g_slist_next (elem)) {
		struct rule *rule = elem->data;

		g_free (rule->key);
		g_free (rule->value);
		g_free (rule);
	}
	g_slist_free (fdi_rules);
	fdi_rules = NULL;
}

/* expat cb for start, e.g. <match foo=bar */
static void
start (void *data, const char *el, const char **attr)
{
	struct fdi_context *fdi_ctx = data;
	enum rule_type rtype = get_rule_type (el);
	int i;

	if (rtype == RULE_UNKNOWN)
		return;

	if (fdi_ctx->rule == NULL)
		return;

	/* get key and attribute for current rule */
	for (i = 0; attr[i] != NULL; i+=2) {
		if (strcmp (attr[i], "key") == 0) {
			fdi_ctx->rule->key = g_strdup (attr[1]);
			continue;
		}
		if (rtype == RULE_SPAWN) {
			if (strcmp (attr[i], "udi") == 0) {
				fdi_ctx->rule->key = g_strdup (attr[1]);
				continue;
			}
		} else if (rtype == RULE_MATCH) {
			fdi_ctx->rule->type_match = get_match_type (attr[i]);
			if (fdi_ctx->rule->type_match == MATCH_UNKNOWN)
				continue;
			fdi_ctx->rule->value = g_strdup (attr[i+1]);
		} else {
			if (strcmp (attr[i], "type") != 0)
				continue;
			fdi_ctx->rule->type_merge = get_merge_type (attr[i+1]);
			if (fdi_ctx->rule->type_merge == MERGE_UNKNOWN)
				return;
		}
	}

	if (fdi_ctx->rule->key[0] == '\0')
		return;

	fdi_ctx->rule->rtype = rtype;

	/* match rules remember the current nesting and the label to jump to if not matching */
	if (rtype == RULE_MATCH) {
		/* remember current nesting */
		fdi_ctx->depth++;

		/* remember rule at nesting level nesting */
		fdi_ctx->match_at_depth[fdi_ctx->depth] = fdi_ctx->rule;

		/* insert match rule into list and get new rule */
		fdi_ctx->rules = g_slist_append (fdi_ctx->rules, fdi_ctx->rule);
		fdi_ctx->rule = g_new0 (struct rule ,1);
	}
}

/* expat cb for character data, i.e. the text data in between tags */
static void
cdata (void *data, const char *s, int len)
{
	struct fdi_context *fdi_ctx = data;

	if (fdi_ctx->rule == NULL)
		return;

	if (fdi_ctx->rule->rtype != RULE_MERGE &&
	    fdi_ctx->rule->rtype != RULE_PREPEND &&
	    fdi_ctx->rule->rtype != RULE_APPEND &&
	    fdi_ctx->rule->rtype != RULE_REMOVE &&
	    fdi_ctx->rule->rtype != RULE_SPAWN)
		return;

	if (len < 1)
		return;

	/* copy cdata in current context */
	fdi_ctx->rule->value = g_realloc (fdi_ctx->rule->value, fdi_ctx->rule->value_len + len+1);
	memcpy (&fdi_ctx->rule->value[fdi_ctx->rule->value_len], s, len);
	fdi_ctx->rule->value_len += len;
	fdi_ctx->rule->value[fdi_ctx->rule->value_len] = '\0';
}

/* expat cb for end, e.g. </device> */
static void
end (void *data, const char *el)
{
	struct fdi_context *fdi_ctx = data;
	enum rule_type rtype = get_rule_type (el);

	if (rtype == RULE_UNKNOWN)
		return;

	if (rtype == RULE_MATCH) {
		if (fdi_ctx->depth <= 0)
			return;

		/* get corresponding match rule and set the rule to skip to */
		fdi_ctx->match_at_depth[fdi_ctx->depth]->next_rule = fdi_ctx->rule;
		fdi_ctx->depth--;
		return;
	}

	/* only valid if element is in the current context */
	if (fdi_ctx->rule->rtype != rtype)
		return;

	/* set empty value to empty string */
	if (fdi_ctx->rule->value == NULL)
		fdi_ctx->rule->value = g_strdup ("");

	if (fdi_ctx->fdi_rule_size >= 0) {
		fdi_ctx->fdi_rule_size += 
			sizeof (struct rule) + 
			strlen (fdi_ctx->rule->key) + 
			fdi_ctx->rule->value_len;
	}

	/* insert merge rule into list and get new rule */
	fdi_ctx->rules = g_slist_append (fdi_ctx->rules, fdi_ctx->rule);
	fdi_ctx->rule = g_new0 (struct rule, 1);
}

/* decompile an fdi file into a list of rules as this is quicker than opening then each time we want to search */
static int
rules_add_fdi_file (GSList **fdi_rules, const char *filename, gboolean compute_rule_size)
{
	struct fdi_context *fdi_ctx;
	char *buf;
	gsize buflen;
	int rc;
	int fdi_rule_size;

	if (!g_file_get_contents (filename, &buf, &buflen, NULL))
		return -1;

	/* get context and first rule */
	fdi_ctx = g_new0 (struct fdi_context ,1);
	fdi_ctx->rule = g_new0 (struct rule ,1);
	fdi_ctx->fdi_rule_size = compute_rule_size ? 0 : -1;

	XML_Parser parser = XML_ParserCreate (NULL);
	if (parser == NULL) {
		fprintf (stderr, "Couldn't allocate memory for parser\n");
		return -1;
	}
	XML_SetUserData (parser, fdi_ctx);
	XML_SetElementHandler (parser, start, end);
	XML_SetCharacterDataHandler (parser, cdata);
	rc = XML_Parse (parser, buf, buflen, 1);
	if (rc == 0)
		fprintf (stderr, "Parse error at line %i:\n%s\n",
			(int) XML_GetCurrentLineNumber (parser),
			XML_ErrorString (XML_GetErrorCode (parser)));
	XML_ParserFree (parser);
	g_free (buf);

	/* insert last dummy rule into list */
	fdi_ctx->rule->rtype = RULE_EOF;
	fdi_ctx->rule->key = g_strdup (filename);
	fdi_ctx->rules = g_slist_append (fdi_ctx->rules, fdi_ctx->rule);

	/* add rules to external list */
	if (rc == 0)
		rules_cleanup_list (fdi_ctx->rules);
	else
		*fdi_rules = g_slist_concat (*fdi_rules, fdi_ctx->rules);

	fdi_rule_size = (gint64) fdi_ctx->fdi_rule_size;

	g_free (fdi_ctx);

	if (rc == 0)
		return -1;

	return compute_rule_size ? fdi_rule_size : 0;
}

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

/* recurse a directory tree, searching and adding fdi files */
static int
rules_search_and_add_fdi_files (GSList **fdi_rules, const char *dir, int *rules_size)
{
	int i;
	int num_entries;
	struct dirent **name_list;

	num_entries = scandir (dir, &name_list, 0, _alphasort);
	if (num_entries == -1)
		return -1;

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
				fdi_rules_size = rules_add_fdi_file (fdi_rules, full_path, rules_size != NULL);
				if (fdi_rules_size >= 0) {
					if (rules_size != NULL) {
						*rules_size += fdi_rules_size;
						HAL_INFO (("fdi file '%s' -> %d bytes of rules", 
							   full_path, fdi_rules_size));
					}
				} else {
					HAL_WARNING (("error processing fdi file '%s'", full_path));
				}
			}
		} else if (g_file_test (full_path, (G_FILE_TEST_IS_DIR)) && filename[0] != '.') {
			int num_bytes;
			char *dirname;

			num_bytes = len + strlen (dir) + 1 + 1;
			dirname = (char *) malloc (num_bytes);
			if (dirname == NULL)
				break;

			snprintf (dirname, num_bytes, "%s/%s", dir, filename);
			rules_search_and_add_fdi_files (fdi_rules, dirname, rules_size);
			free (dirname);
		}
		g_free (full_path);
		free (name_list[i]);
	}

	for (; i >= 0; i--) {
		free (name_list[i]);
	}

	free (name_list);
	return 0;
}

/* print the rules to screen, mainly useful for debugging */
#if 0
static void
rules_dump (GSList *fdi_rules)
{
	GSList *elem;

	for (elem = fdi_rules; elem != NULL; elem = g_slist_next (elem)) {
		struct rule *rule = elem->data;

		if (rule->rtype == RULE_EOF) {
			printf ("%p: eof %s\n", rule, rule->key);
		} else if (rule->rtype == RULE_MATCH) {
			printf ("\n");
			printf ("%p: match '%s' (%s) '%s' (skip to %p)\n",
				rule, rule->key, get_match_type_str (rule->type_match),
				rule->value, rule->next_rule);
		} else {
			printf ("%p: %s '%s' (%s) '%s'\n",
				rule, get_rule_type_str (rule->rtype), rule->key,
				get_merge_type_str (rule->type_merge), rule->value);
		}
	}
}
#endif

/* setup the location of the rules */
void
di_rules_init (void)
{
	int size;
	char *hal_fdi_source_preprobe = getenv ("HAL_FDI_SOURCE_PREPROBE");
	char *hal_fdi_source_information = getenv ("HAL_FDI_SOURCE_INFORMATION");
	char *hal_fdi_source_policy = getenv ("HAL_FDI_SOURCE_POLICY");

	HAL_INFO (("Loading rules"));

	size = 0;

	if (hal_fdi_source_preprobe != NULL)
		rules_search_and_add_fdi_files (&fdi_rules_preprobe, hal_fdi_source_preprobe, &size);
	else {
		rules_search_and_add_fdi_files (&fdi_rules_preprobe, PACKAGE_DATA_DIR "/hal/fdi/preprobe", &size);
		rules_search_and_add_fdi_files (&fdi_rules_preprobe, PACKAGE_SYSCONF_DIR "/hal/fdi/preprobe", &size);
	}

	if (hal_fdi_source_information != NULL)
		rules_search_and_add_fdi_files (&fdi_rules_information, hal_fdi_source_information, &size);
	else {
		rules_search_and_add_fdi_files (&fdi_rules_information, PACKAGE_DATA_DIR "/hal/fdi/information", &size);
		rules_search_and_add_fdi_files (&fdi_rules_information, PACKAGE_SYSCONF_DIR "/hal/fdi/information", &size);
	}

	if (hal_fdi_source_policy != NULL)
		rules_search_and_add_fdi_files (&fdi_rules_policy, hal_fdi_source_policy, &size);
	else {
		rules_search_and_add_fdi_files (&fdi_rules_policy, PACKAGE_DATA_DIR "/hal/fdi/policy", &size);
		rules_search_and_add_fdi_files (&fdi_rules_policy, PACKAGE_SYSCONF_DIR "/hal/fdi/policy", &size);
	}

	/* dump the rules (commented out as this is expensive) */
	/*rules_dump (fdi_rules_preprobe);
	  rules_dump (fdi_rules_information);
	  rules_dump (fdi_rules_policy);
	*/

	HAL_INFO (("Loading rules done (occupying %d bytes)", size));
}

/* cleanup the rules */
void
di_rules_cleanup (void)
{
	rules_cleanup_list (fdi_rules_preprobe);
	rules_cleanup_list (fdi_rules_information);
	rules_cleanup_list (fdi_rules_policy);
	fdi_rules_preprobe = NULL;
	fdi_rules_information = NULL;
	fdi_rules_policy = NULL;
}

/* process a match and merge comand for a device */
static void
rules_match_and_merge_device (GSList *fdi_rules, HalDevice *d)
{
	GSList *elem;

	if (fdi_rules == NULL) {
		di_rules_cleanup ();
		di_rules_init ();
	}

	elem = fdi_rules;
	while (elem != NULL) {
		struct rule *rule = elem->data;

		switch (rule->rtype) {
		case RULE_MATCH:
			/* skip non-matching rules block */
			/*HAL_INFO(("%p match '%s' at %s", rule, rule->key, hal_device_get_udi (d)));*/
			if (!handle_match (rule, d)) {
				/*HAL_INFO(("no match, skip to rule %s (%p)", get_rule_type_str (rule->next_rule->rtype), rule->next_rule));*/
				elem = g_slist_find (elem, rule->next_rule);
				continue;
			}
			break;

		case RULE_APPEND:
		case RULE_PREPEND:
		case RULE_REMOVE:
		case RULE_CLEAR:
		case RULE_SPAWN:
		case RULE_MERGE:
			/*HAL_INFO(("%p merge '%s' at %s", rule, rule->key, hal_device_get_udi (d)));*/
			handle_merge (rule, d);
			break;

		case RULE_EOF:
			/*HAL_INFO(("%p fdi file '%s' finished", rule, rule->key));*/
			break;

		default:
			HAL_WARNING(("Unhandled rule (%i)!", rule->rtype));
			break;
		}
		elem = g_slist_next (elem);
	}
}

/* merge the device info type, either preprobe, info or policy */
gboolean
di_search_and_merge (HalDevice *d, DeviceInfoType type)
{
	switch (type) {
	case DEVICE_INFO_TYPE_PREPROBE:
		/*HAL_INFO(("apply fdi preprobe to device %p", d));*/
		rules_match_and_merge_device (fdi_rules_preprobe, d);
		break;

	case DEVICE_INFO_TYPE_INFORMATION:
		/*HAL_INFO(("apply fdi info to device %p", d));*/
		rules_match_and_merge_device (fdi_rules_information, d);
		break;

	case DEVICE_INFO_TYPE_POLICY:
		/*HAL_INFO(("apply fdi policy to device %p", d));*/
		rules_match_and_merge_device (fdi_rules_policy, d);
		break;

	default:
		break;
	}

	return TRUE;
}
