/***************************************************************************
 * CVSID: $Id$
 *
 * rule.h : struct rule and surrounding helpful declarations/macros.
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

#ifndef __RULE_H__
#define __RULE__H__

/* rule type to process */
typedef enum {
	RULE_UNKNOWN,
	RULE_MATCH,
	RULE_MERGE,
	RULE_APPEND,
	RULE_PREPEND,
	RULE_REMOVE,
	RULE_CLEAR,
	RULE_SPAWN,
	RULE_EOF,
        RULE_ADDSET
} rule_type;

/* type of merge command */
typedef enum {
	MERGE_UNKNOWN,
	MERGE_STRING,
	MERGE_BOOLEAN,
	MERGE_INT32,
	MERGE_UINT64,
	MERGE_DOUBLE,
	MERGE_COPY_PROPERTY,
	MERGE_STRLIST,
	MERGE_REMOVE,
} merge_type;

/* type of match command */
typedef enum {
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
	MATCH_PREFIX,
	MATCH_PREFIX_NCASE,
	MATCH_SUFFIX,
	MATCH_SUFFIX_NCASE,
	MATCH_COMPARE_LT,
	MATCH_COMPARE_LE,
	MATCH_COMPARE_GT,
	MATCH_COMPARE_GE,
	MATCH_SIBLING_CONTAINS,
	MATCH_COMPARE_NE,
	MATCH_CONTAINS_NOT,
	MATCH_DOUBLE,
	MATCH_CONTAINS_OUTOF,
	MATCH_INT_OUTOF,
	MATCH_PREFIX_OUTOF,
	MATCH_STRING_OUTOF,
} match_type;

/* a "rule" structure that is a generic node of the fdi file */
struct rule {
	size_t		rule_size;	/* offset to next rule in the list (aligned to 4 bytes) */
	u_int32_t	jump_position;	/* the rule to jumo position (aligned to 4 bytes) */

	rule_type	rtype;		/* type of rule */
	match_type      type_match;
	merge_type      type_merge;

	u_int32_t	value_offset;	/* offset to keys value (aligned to 4 bytes) */
	size_t		value_len;	/* length of keys value */

	size_t		key_len;
	char		key[0];
};

struct cache_header {
	u_int32_t	fdi_rules_preprobe;
	u_int32_t	fdi_rules_information;
	u_int32_t	fdi_rules_policy;
	u_int32_t	all_rules_size;
	char		empty_string[4];
};

#define HAL_MAX_INDENT_DEPTH		64

#define HALD_CACHE_FILE PACKAGE_LOCALSTATEDIR "/cache/hald/fdi-cache"

#endif
