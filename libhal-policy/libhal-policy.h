/***************************************************************************
 *
 * libhal-policy.h : Simple library for hald to query policy and UI shells
 *                   to query and modify policy
 *
 * Copyright (C) 2006 David Zeuthen, <david@fubar.dk>
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

#ifndef LIBHAL_POLICY_H
#define LIBHAL_POLICY_H

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <glib.h>

typedef enum {
	LIBHAL_POLICY_RESULT_OK,
	LIBHAL_POLICY_RESULT_ERROR,
	LIBHAL_POLICY_RESULT_INVALID_CONTEXT,
	LIBHAL_POLICY_RESULT_PERMISSON_DENIED,
	LIBHAL_POLICY_RESULT_NO_SUCH_POLICY
} LibHalPolicyResult;

struct LibHalPolicyContext_s;
typedef struct LibHalPolicyContext_s LibHalPolicyContext;


typedef enum {
	LIBHAL_POLICY_ELEMENT_TYPE_UID,
	LIBHAL_POLICY_ELEMENT_TYPE_GID
} LibHalPolicyElementType;

struct LibHalPolicyElement_s;
typedef struct LibHalPolicyElement_s LibHalPolicyElement;


LibHalPolicyContext *libhal_policy_new_context                  (void);

gboolean             libhal_policy_context_set_txt_source       (LibHalPolicyContext   *ctx,
								 const char *directory);

gboolean             libhal_policy_free_context                 (LibHalPolicyContext   *ctx);

LibHalPolicyResult   libhal_policy_get_policies                 (LibHalPolicyContext   *ctx,
								 GList                **result);

LibHalPolicyResult libhal_policy_is_uid_allowed_for_policy      (LibHalPolicyContext    *ctx,
							         uid_t                   uid, 
							         const char             *policy, 
							         const char             *resource,
							         gboolean               *result);


LibHalPolicyResult libhal_policy_is_uid_gid_allowed_for_policy  (LibHalPolicyContext    *ctx,
							         uid_t                   uid, 
								 guint                   num_gids,
								 gid_t                  *gid_list,
							         const char             *policy, 
							         const char             *resource,
							         gboolean               *result);



LibHalPolicyResult libhal_policy_get_whitelist                  (LibHalPolicyContext    *ctx,
							         const char             *policy,
							         GList                 **result);

LibHalPolicyResult libhal_policy_get_blacklist                  (LibHalPolicyContext    *ctx,
							         const char             *policy,
							         GList                 **result);

LibHalPolicyResult libhal_policy_set_whitelist                  (LibHalPolicyContext    *ctx,
							         const char             *policy,
							         GList                  *whitelist);

LibHalPolicyResult libhal_policy_set_blacklist                  (LibHalPolicyContext    *ctx,
							         const char             *policy,
							         GList                  *blacklist);


LibHalPolicyElementType libhal_policy_element_get_type          (LibHalPolicyElement     *elem);

gboolean                libhal_policy_element_get_include_all   (LibHalPolicyElement     *elem);

gboolean                libhal_policy_element_get_exclude_all   (LibHalPolicyElement     *elem);

uid_t                   libhal_policy_element_get_uid           (LibHalPolicyElement     *elem);

gid_t                   libhal_policy_element_get_gid           (LibHalPolicyElement     *elem);

const char             *libhal_policy_element_get_resource      (LibHalPolicyElement     *elem);



LibHalPolicyElement    *libhal_policy_element_new               (LibHalPolicyContext   *ctx);

void                    libhal_policy_element_set_type          (LibHalPolicyElement     *elem, 
								 LibHalPolicyElementType  type);

void                    libhal_policy_element_set_include_all   (LibHalPolicyElement     *elem, 
								 gboolean                 value);

void                    libhal_policy_element_set_exclude_all   (LibHalPolicyElement     *elem, 
								 gboolean                 value);

void                    libhal_policy_element_set_uid           (LibHalPolicyElement     *elem, 
								 uid_t                    uid);

void                    libhal_policy_element_set_gid           (LibHalPolicyElement     *elem, 
								 gid_t                    gid);

void                    libhal_policy_element_set_resource      (LibHalPolicyElement     *elem, 
								 const char              *resource);



void                    libhal_policy_free_element              (LibHalPolicyElement     *elem);

void                    libhal_policy_free_element_list         (GList *policy_element_list);



char *libhal_policy_util_uid_to_name (LibHalPolicyContext *ctx, uid_t uid, gid_t *default_gid);
char *libhal_policy_util_gid_to_name (LibHalPolicyContext *ctx, gid_t gid);

uid_t libhal_policy_util_name_to_uid (LibHalPolicyContext *ctx, const char *username, gid_t *default_gid);
gid_t libhal_policy_util_name_to_gid (LibHalPolicyContext *ctx, const char *groupname);

void  libhal_policy_element_dump     (LibHalPolicyElement *elem, FILE* fp);

#endif /* LIBHAL_POLICY_H */


