/***************************************************************************
 * CVSID: $Id$
 *
 * device_info.c : match/merge device properties.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <errno.h>

#include "hald.h"
#include "logger.h"
#include "mmap_cache.h"
#include "device_info.h"
#include "device_store.h"
#include "util.h"
#include "rule.h"
#include "osspec.h"

void *rules_ptr = NULL;

#ifdef DUMP_RULES
static char *
get_match_type_str (enum match_type type)
{
	switch (type) {
	case MATCH_STRING:
		return "string";
	case MATCH_STRING_OUTOF:
		return "string_outof";
	case MATCH_INT:
		return "int";
	case MATCH_INT_OUTOF:
		return "int_outof";
	case MATCH_UINT64:
		return "uint64";
	case MATCH_BOOL:
		return "bool";
	case MATCH_DOUBLE:
		return "double";
	case MATCH_EXISTS:
		return "exists";
	case MATCH_EMPTY:
		return "empty";
	case MATCH_ISASCII:
		return "is_ascii";
	case MATCH_IS_ABS_PATH:
		return "is_absolute_path";
	case MATCH_SIBLING_CONTAINS:
		return "sibling_contains";
	case MATCH_CONTAINS:
		return "contains";
	case MATCH_CONTAINS_NCASE:
		return "contains_ncase";
	case MATCH_CONTAINS_NOT:
		return "contains_not";
	case MATCH_CONTAINS_OUTOF:
		return "contains_outof";
	case MATCH_PREFIX:
		return "prefix";
	case MATCH_PREFIX_NCASE:
		return "prefix_ncase";
	case MATCH_PREFIX_OUTOF:
		return "prefix_outof";
	case MATCH_SUFFIX:
		return "suffix";
	case MATCH_SUFFIX_NCASE:
		return "suffix_ncase";
	case MATCH_COMPARE_LT:
		return "compare_lt";
	case MATCH_COMPARE_LE:
		return "compare_le";
	case MATCH_COMPARE_GT:
		return "compare_gt";
	case MATCH_COMPARE_GE:
		return "compare_ge";
	case MATCH_COMPARE_NE:
		return "compare_ne";
	case MATCH_UNKNOWN:
		return "unknown match type";
	}
	return "invalid match type";
}
#endif

static inline gboolean
resolve_udiprop_path_old (const char *path, const char *source_udi,
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

/** Resolve a udi-property path as used in .fdi files.
 *
 *  Examples of udi-property paths:
 *
 *   info.udi
 *   /org/freedesktop/Hal/devices/computer:kernel.name
 *   @block.storage_device:storage.bus
 *   @block.storage_device:@storage.originating_device:ide.channel
 *
 *  @param  source_udi          UDI of source device
 *  @param  path                The given path
 *  @param  udi_result          Where to store the resulting UDI
 *  @param  prop_result         Where to store the resulting property name
 *  @param  scratch		 
 *  @return                     TRUE if and only if the path resolved.
 */
static gboolean
resolve_udiprop_path (const char *path, const char *source_udi,
		      const char **udi_result, const char **prop_result,
		      const char *scratch /* HAL_PATH_MAX * 2 + 3 */)
{
	/* Detect trivial property access, e.g. path='foo.bar'   */
	if (path == NULL || !strchr (path, ':')) {
		*udi_result = source_udi;
		*prop_result = path;
		return TRUE;
	}

	/* the sub 5% 'everything else' case */
	*udi_result = scratch;
	*prop_result = scratch + HAL_PATH_MAX + 2;
	return resolve_udiprop_path_old (path, source_udi,
					 (char *) *udi_result, HAL_PATH_MAX,
					 (char *) *prop_result, HAL_PATH_MAX);
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
	char resolve_scratch[HAL_PATH_MAX*2 + 3];
	const char *udi_to_check;
	const char *prop_to_check;
	const char *key = rule->key;
	const char *value = (char *)RULES_PTR(rule->value_offset);
	const char *d_udi;
	
	d_udi = hal_device_get_udi (d);

	/* Resolve key paths like 'someudi/foo/bar/baz:prop.name' '@prop.here.is.an.udi:with.prop.name' */
	if (!resolve_udiprop_path (key,
				   d_udi,
				   &udi_to_check,
				   &prop_to_check,
				   resolve_scratch)) {
		/*HAL_ERROR (("Could not resolve keypath '%s' on udi '%s'", key, value));*/
		return FALSE;
	}

	if (strcmp(udi_to_check, d_udi)) {
		d = hal_device_store_find (hald_get_gdl (), udi_to_check);
		if (d == NULL)
			d = hal_device_store_find (hald_get_tdl (), udi_to_check);
		if (d == NULL) {
			HAL_ERROR (("Could not find device with udi '%s'", udi_to_check));
			return FALSE;
		}
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

	case MATCH_DOUBLE:
	{
		double val = atof (value);

		if (hal_device_property_get_type (d, prop_to_check) != HAL_PROPERTY_TYPE_DOUBLE)
			return FALSE;
		if (hal_device_property_get_double (d, prop_to_check) != val)
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
	case MATCH_CONTAINS_NOT:
	{
		dbus_bool_t contains = FALSE;

		if (hal_device_has_property (d, prop_to_check) && value != NULL) {

			if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
				const char *haystack;

				haystack = hal_device_property_get_string (d, prop_to_check);
				if (haystack != NULL &&  (strstr(haystack, value) != NULL))
					contains = TRUE;
			} else if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRLIST) {
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
		}
	
		if (rule->type_match == MATCH_CONTAINS) {	
			return contains;
		} else {
			return !contains; /* rule->type_match == MATCH_CONTAINS_NOT  */
		}
	}
	
	case MATCH_CONTAINS_OUTOF:
	case MATCH_PREFIX_OUTOF:
	case MATCH_STRING_OUTOF:
	{
		dbus_bool_t contains = FALSE;

		if (hal_device_has_property (d, prop_to_check) && value != NULL) {

			if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
				const char *haystack;
				gchar **values;
        			int i;

                		values = g_strsplit (value, ";", 0);
				
				haystack = hal_device_property_get_string (d, prop_to_check);
				if (haystack != NULL && values != NULL) {
					for (i = 0; values [i] ; ++i) {
						if (rule->type_match == MATCH_CONTAINS_OUTOF) {
							if (strstr(haystack, values[i]) != NULL) {
								contains = TRUE;
								break;
							}
						}
						else if (rule->type_match == MATCH_PREFIX_OUTOF) {
							if (g_str_has_prefix (haystack, values[i])) {
								contains = TRUE;
								break;
							}	
						}
						else if (rule->type_match == MATCH_STRING_OUTOF) {
							if (strcmp (haystack, values[i]) == 0) {
								contains = TRUE;
								break;
							}
						}
					}
				}
				g_strfreev (values);	
			} 
		}
	
		return contains;
	}

	case MATCH_INT_OUTOF:
	{
		dbus_bool_t contained = FALSE;

		if (hal_device_has_property (d, prop_to_check) && value != NULL) {

			if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_INT32) {
				gchar **values;
        			int i;
				int to_check;

                		values = g_strsplit (value, ";", 0);
				to_check = hal_device_property_get_int (d, prop_to_check);				

				if (values != NULL) {
					for (i = 0; values [i] ; ++i) {
                				if (to_check == strtol (values[i], NULL, 0)) {
							contained = TRUE;
							break;
						}	
					}
				}
				g_strfreev (values);	
			} 
		}
	
		return contained;
	}


	case MATCH_SIBLING_CONTAINS:
	{
		dbus_bool_t contains = FALSE;
		const char *parent_udi;

		parent_udi = hal_device_property_get_string (d, "info.parent");
		if (parent_udi != NULL) {
			GSList *i;
			GSList *siblings;

			siblings = hal_device_store_match_multiple_key_value_string (hald_get_gdl (),
										     "info.parent",
										     parent_udi);
			for (i = siblings; i != NULL; i = g_slist_next (i)) {
				HalDevice *sib = HAL_DEVICE (i->data);

				if (sib == d)
					continue;

				HAL_INFO (("Checking sibling '%s' of '%s' whether '%s' contains '%s'",
					   hal_device_get_udi (sib), hal_device_get_udi (d), prop_to_check, value));

				if (hal_device_property_get_type (sib, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
					if (hal_device_has_property (sib, prop_to_check)) {
						const char *haystack;
						
						haystack = hal_device_property_get_string (sib, prop_to_check);
						if (value != NULL && haystack != NULL && strstr (haystack, value))
							contains = TRUE;
					}
				} else if (hal_device_property_get_type (sib, prop_to_check) == HAL_PROPERTY_TYPE_STRLIST && value != NULL) {
					HalDeviceStrListIter iter;
					for (hal_device_property_strlist_iter_init (sib, prop_to_check, &iter);
					     hal_device_property_strlist_iter_is_valid (&iter);
					     hal_device_property_strlist_iter_next (&iter)) {
						const char *str = hal_device_property_strlist_iter_get_value (&iter);
						if (strcmp (str, value) == 0) {
							contains = TRUE;
							break;
						}
					}
				}

				if (contains)
					break;

			} /* for all siblings */
			g_slist_free (siblings);			
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
		} else {
			return FALSE;
		}
		return contains_ncase;
	}

	case MATCH_PREFIX:
	{
		dbus_bool_t prefix = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
			if (hal_device_has_property (d, prop_to_check)) {
				const char *haystack;
				haystack = hal_device_property_get_string (d, prop_to_check);
				if (value != NULL && haystack != NULL &&
				    g_str_has_prefix (haystack, value)) {
					prefix = TRUE;
				}
			}
		} else {
			return FALSE;
		}

		return prefix;
	}

	case MATCH_PREFIX_NCASE:
	{
		dbus_bool_t prefix_ncase = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
			if (hal_device_has_property (d, prop_to_check)) {
				char *value_lowercase;
				char *haystack_lowercase;
				value_lowercase   = g_utf8_strdown (value, -1);
				haystack_lowercase = g_utf8_strdown (hal_device_property_get_string (d, prop_to_check), -1);
				if (value_lowercase != NULL && haystack_lowercase != NULL &&
				    g_str_has_prefix (haystack_lowercase, value_lowercase)) {
					prefix_ncase = TRUE;
				}
				g_free (value_lowercase);
				g_free (haystack_lowercase);
			}
		} else {
			return FALSE;
		}
		return prefix_ncase;
	}

	case MATCH_SUFFIX:
	{
		dbus_bool_t suffix = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
			if (hal_device_has_property (d, prop_to_check)) {
				const char *haystack;
				haystack = hal_device_property_get_string (d, prop_to_check);
				if (value != NULL && haystack != NULL &&
				    g_str_has_suffix (haystack, value)) {
					suffix = TRUE;
				}
			}
		} else {
			return FALSE;
		}

		return suffix;
	}

	case MATCH_SUFFIX_NCASE:
	{
		dbus_bool_t suffix_ncase = FALSE;

		if (hal_device_property_get_type (d, prop_to_check) == HAL_PROPERTY_TYPE_STRING) {
			if (hal_device_has_property (d, prop_to_check)) {
				char *value_lowercase;
				char *haystack_lowercase;
				value_lowercase   = g_utf8_strdown (value, -1);
				haystack_lowercase = g_utf8_strdown (hal_device_property_get_string (d, prop_to_check), -1);
				if (value_lowercase != NULL && haystack_lowercase != NULL &&
				    g_str_has_suffix (haystack_lowercase, value_lowercase)) {
					suffix_ncase = TRUE;
				}
				g_free (value_lowercase);
				g_free (haystack_lowercase);
			}
		} else {
			return FALSE;
		}
		return suffix_ncase;
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

	case MATCH_COMPARE_NE:
	{
		dbus_int64_t result;

		if (!match_compare_property (d, prop_to_check, value, &result))
			return FALSE;
		else
			return result != 0;
	}


	default:
		HAL_INFO(("match ERROR"));
		return FALSE;
	}

	// return FALSE;
}

/* we have finished the callouts for a device, now add it to the gdl */
static void
spawned_device_callouts_add_done (HalDevice *d, gpointer userdata1, gpointer userdata2)
{
	/*HAL_INFO (("Add callouts completed udi=%s", hal_device_get_udi (d)));*/

	/* Move from temporary to global device store */
	hal_device_store_remove (hald_get_tdl (), d);
	hal_device_store_add (hald_get_gdl (), d);
}

/* for this device, process the rule */
static gboolean
handle_merge (struct rule *rule, HalDevice *d)
{
	const char *value = (char *)RULES_PTR(rule->value_offset);
	const char *key;
	char resolve_scratch[HAL_PATH_MAX*2 + 3];
	const char *key_to_merge;

	if (rule->rtype == RULE_MERGE || rule->rtype == RULE_APPEND || 
	    rule->rtype == RULE_PREPEND || rule->rtype == RULE_ADDSET ) {
		const char *udi_to_merge;

		/* Resolve key paths like 'someudi/foo/bar/baz:prop.name' '@prop.here.is.an.udi:with.prop.name' */
                if (!resolve_udiprop_path (rule->key, hal_device_get_udi (d),
			&udi_to_merge, &key_to_merge, resolve_scratch)) {
	                 HAL_ERROR (("Could not resolve keypath '%s' on udi '%s'", rule->key, hal_device_get_udi (d)));
			return FALSE;
		} else {
			key = key_to_merge;	

			if (strcmp(hal_device_get_udi (d), udi_to_merge) != 0) {

				d = hal_device_store_find (hald_get_gdl (), udi_to_merge);
				if (d == NULL) {
					d = hal_device_store_find (hald_get_tdl (), udi_to_merge);

					if (d == NULL) {
						HAL_ERROR (("Could not find device with udi '%s'", udi_to_merge));
						return FALSE;
					}
				}
			}
		}
	} else {
		key = rule->key;
	} 

	if (rule->rtype == RULE_MERGE) {

		if (rule->type_merge == MERGE_STRING) {
			hal_device_property_set_string (d, key, value);

		} else if (rule->type_merge == MERGE_STRLIST) {
			int type = hal_device_property_get_type (d, key);

			if (type == HAL_PROPERTY_TYPE_STRLIST || type == HAL_PROPERTY_TYPE_INVALID) {
				hal_device_property_remove (d, key);
				hal_device_property_strlist_append (d, key, value, FALSE);
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
			char more_resolve_scratch[HAL_PATH_MAX*2 + 3];
			const char *udi_to_merge_from;
			const char *prop_to_merge;

			/* Resolve key paths like 'someudi/foo/bar/baz:prop.name'
			 * '@prop.here.is.an.udi:with.prop.name'
			 */
			if (!resolve_udiprop_path (value,
						   hal_device_get_udi (d),
						   &udi_to_merge_from, &prop_to_merge,
						   more_resolve_scratch)) {
				HAL_ERROR (("Could not resolve keypath '%s' on udi '%s'", value, hal_device_get_udi (d)));
			} else {
				HalDevice *copyfrom;

				copyfrom = hal_device_store_find (hald_get_gdl (), udi_to_merge_from);
				if (copyfrom == NULL) {
					copyfrom = hal_device_store_find (hald_get_tdl (), udi_to_merge_from);
				}
				if (copyfrom == NULL) {
					HAL_ERROR (("Could not find device with udi '%s'", udi_to_merge_from));
				} else {
					hal_device_copy_property (copyfrom, prop_to_merge, d, key);
				}
			}

		} else {
			HAL_ERROR (("unknown merge type (%u)", rule->type_merge));
		}

	} else if (rule->rtype == RULE_APPEND || rule->rtype == RULE_PREPEND) {
		char buf[HAL_PATH_MAX];
		char buf2[HAL_PATH_MAX];

		if (hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRING &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRLIST &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_INVALID) {
			HAL_ERROR (("invalid key type"));
			return FALSE;
		}

		if (rule->type_merge == MERGE_STRLIST) {
			if (rule->rtype == RULE_APPEND)
				hal_device_property_strlist_append (d, key, value, FALSE);
			else	/* RULE_PREPEND */ 
				hal_device_property_strlist_prepend (d, key, value);
		} else { 
			const char *existing_string;

			switch (rule->type_merge) {
			case MERGE_STRING:
				strncpy (buf, value, sizeof (buf));
				break;
			case MERGE_COPY_PROPERTY:
			{
				char more_resolve_scratch[HAL_PATH_MAX*2 + 3];
				const char *udi_to_merge_from;
				const char *prop_to_merge;

				/* Resolve key paths like 'someudi/foo/bar/baz:prop.name'
				 * '@prop.here.is.an.udi:with.prop.name'
				 */
				if (!resolve_udiprop_path (value,
							   hal_device_get_udi (d),
							   &udi_to_merge_from, &prop_to_merge,
							   more_resolve_scratch)) {
					HAL_ERROR (("Could not resolve keypath '%s' on udi '%s'", value, hal_device_get_udi (d)));
				} else {
					HalDevice *copyfrom;

					copyfrom = hal_device_store_find (hald_get_gdl (), udi_to_merge_from);
					if (copyfrom == NULL) {
						copyfrom = hal_device_store_find (hald_get_tdl (), udi_to_merge_from);
					}
					if (copyfrom == NULL) {
						HAL_ERROR (("Could not find device with udi '%s'", udi_to_merge_from));
					} else {
						hal_device_property_get_as_string (copyfrom, prop_to_merge, buf, sizeof (buf));
					}
				}
				break;
			}
			default:
				break;
			}
		
			existing_string = hal_device_property_get_string (d, key);
			if (existing_string != NULL) {
				if (rule->rtype == RULE_APPEND) {
					strncpy (buf2, existing_string, sizeof (buf2));
					strncat (buf2, buf, sizeof (buf2) - strlen(buf2));
				} else { /* RULE_PREPEND */
					strncpy (buf2, buf, sizeof (buf2));
					strncat (buf2, existing_string, sizeof (buf2) - strlen(buf2));
				}
			} else {
				strncpy (buf2, buf, sizeof (buf2));
			}

			hal_device_property_set_string (d, key, buf2);
		
		}
	} else if (rule->rtype == RULE_ADDSET) {

		if (hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_STRLIST &&
		    hal_device_property_get_type (d, key) != HAL_PROPERTY_TYPE_INVALID) {
			HAL_ERROR (("invalid key type"));
			return FALSE;
		}

                if (!hal_device_has_property (d, key) ||
                    !hal_device_property_strlist_contains (d, key, value)) {
                        hal_device_property_strlist_append (d, key, value, FALSE);
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
			hal_device_property_set_string (spawned, "info.subsystem", "unknown");
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

static struct rule *di_next(struct rule *rule){
	struct cache_header	*header = (struct cache_header*) RULES_PTR(0);
	size_t			offset = (char *)rule - (char*)rules_ptr;
	size_t			next = offset + rule->rule_size;

	if (((offset >= header->fdi_rules_preprobe) && (next < header->fdi_rules_information)) ||
	    ((offset >= header->fdi_rules_information) && (next < header->fdi_rules_policy)) ||
	    ((offset >= header->fdi_rules_policy) && (next < header->all_rules_size))){
		return (struct rule*) RULES_PTR(next);
	}
	return NULL;
}

static struct rule *di_jump(struct rule *rule){
	struct cache_header	*header = (struct cache_header*) RULES_PTR(0);
	size_t			offset = (char *)rule - (char*)rules_ptr;
	size_t			next = rule->jump_position;

	if (next == 0) return NULL;
	if (((offset >= header->fdi_rules_preprobe) && (next < header->fdi_rules_information)) ||
	    ((offset >= header->fdi_rules_information) && (next < header->fdi_rules_policy)) ||
	    ((offset >= header->fdi_rules_policy) && (next < header->all_rules_size))){
		return (struct rule*) RULES_PTR(next);
	}
	return NULL;
}


/* process a match and merge comand for a device */
static void
rules_match_and_merge_device (void *fdi_rules_list, HalDevice *d)
{
	struct rule *rule = fdi_rules_list;
	while (rule != NULL){
		/*HAL_INFO(("== Iterating rules =="));*/

		switch (rule->rtype) {
		case RULE_MATCH:
			/* skip non-matching rules block */
			/*HAL_INFO(("%p match '%s' at %s", rule, rule->key, hal_device_get_udi (d)));*/
			if (!handle_match (rule, d)) {
				/*HAL_INFO(("no match, skip to rule (%llx)", rule->jump_position));*/
				rule = di_jump(rule);

				if(rule == NULL)
					DIE(("Rule is NULL on jump"));

				continue;
			}
			break;

		case RULE_APPEND:
		case RULE_PREPEND:
		case RULE_ADDSET:
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
			rule = di_jump(rule);
			break;
		}
		rule = di_next(rule);
	}
}

/* merge the device info type, either preprobe, info or policy */
gboolean
di_search_and_merge (HalDevice *d, DeviceInfoType type){
	struct cache_header *header;

        /* make sure our fdi rule cache is up to date */
        if (di_cache_coherency_check (FALSE)) {
                di_rules_init ();
	}

	header = (struct cache_header*) RULES_PTR(0);

	switch (type) {
	case DEVICE_INFO_TYPE_PREPROBE:
		/* Checking if we have at least one preprobe rule */
		if(header->fdi_rules_information > header->fdi_rules_preprobe)
		{
			/*HAL_INFO(("preprobe rules offset: %ld", header->fdi_rules_preprobe));
			HAL_INFO(("preprobe rules size: %ld",
			header->fdi_rules_information - header->fdi_rules_preprobe));*/
			rules_match_and_merge_device (RULES_PTR(header->fdi_rules_preprobe), d);
		}
		break;

	case DEVICE_INFO_TYPE_INFORMATION:
		/* Checking if we have at least one information rule */
		if(header->fdi_rules_policy > header->fdi_rules_information)
		{
			/*HAL_INFO(("information rules offset: %ld", header->fdi_rules_information));
			HAL_INFO(("information rules size: %ld",
			header->fdi_rules_policy - header->fdi_rules_information));*/
			rules_match_and_merge_device (RULES_PTR(header->fdi_rules_information), d);
		}
		break;

	case DEVICE_INFO_TYPE_POLICY:
		/* Checking if we have at least one policy rule */
		if(header->all_rules_size > header->fdi_rules_policy)
		{
			/*HAL_INFO(("policy rules offset: %ld", header->fdi_rules_policy));
			HAL_INFO(("policy rules size: %ld",
			header->all_rules_size - header->fdi_rules_policy));*/
			rules_match_and_merge_device (RULES_PTR(header->fdi_rules_policy), d);
		}
		break;

	default:
		break;
	}

	return TRUE;
}
