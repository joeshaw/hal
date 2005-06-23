/***************************************************************************
 * CVSID: $Id$
 *
 * util.h - Various utilities
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

#ifndef UTIL_H
#define UTIL_H

#include "device.h"
#include "device_store.h"

int util_compute_time_remaining (const char *id, int chargeRate, int chargeLevel, int chargeLastFull, gboolean isDischarging, gboolean isCharging);

gboolean hal_util_remove_trailing_slash (gchar *path);

const gchar *hal_util_get_last_element (const gchar *s);

gchar *hal_util_get_parent_path (const gchar *path);

gchar *hal_util_get_normalized_path (const gchar *path1, const gchar *path2);

gboolean hal_util_get_int_from_file (const gchar *directory, const gchar *file, gint *result, gint base);

gboolean hal_util_set_int_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file, gint base);

gchar *hal_util_get_string_from_file (const gchar *directory, const gchar *file);

gboolean hal_util_set_string_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file);

gboolean hal_util_get_bcd2_from_file (const gchar *directory, const gchar *file, gint *result);

gboolean hal_util_set_bcd2_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file);

void hal_util_compute_udi (HalDeviceStore *store, gchar *dst, gsize dstsize, const gchar *format, ...);

gboolean hal_util_path_ascend (gchar *path);

void hal_util_grep_discard_existing_data (void);

gchar *hal_util_grep_file (const gchar *directory, const gchar *file, const gchar *linestart, gboolean reuse_file);

gint hal_util_grep_int_elem_from_file (const gchar *directory, const gchar *file, 
				       const gchar *linestart, guint elem, guint base, gboolean reuse_file);

gchar *hal_util_grep_string_elem_from_file (const gchar *directory, const gchar *file, 
					    const gchar *linestart, guint elem, gboolean reuse_file);

gboolean hal_util_set_string_elem_from_file (HalDevice *d, const gchar *key, 
					     const gchar *directory, const gchar *file, 
					     const gchar *linestart, guint elem, gboolean reuse_file);

gboolean hal_util_set_int_elem_from_file (HalDevice *d, const gchar *key, 
					  const gchar *directory, const gchar *file, 
					  const gchar *linestart, guint elem, guint base, gboolean reuse_file);

gboolean hal_util_set_bool_elem_from_file (HalDevice *d, const gchar *key, 
					   const gchar *directory, const gchar *file, 
					   const gchar *linestart, guint elem, const gchar *expected, 
					   gboolean reuse_file);

struct HalHelperData_s;
typedef struct HalHelperData_s HalHelperData;

typedef void (*HalHelperTerminatedCB) (HalDevice *d, gboolean timed_out, gint return_code, 
				       gpointer data1, gpointer data2, HalHelperData *helper_data);

struct HalHelperData_s
{
	GPid pid;
	guint timeout_watch_id;
	guint child_watch_id;
	HalHelperTerminatedCB cb;
	gpointer data1;
	gpointer data2;
	HalDevice *d;

	gboolean already_issued_callback;
	gboolean already_issued_kill;
};

unsigned int hal_util_kill_all_helpers (void);

HalHelperData  *hal_util_helper_invoke (const gchar *command_line, gchar **extra_env, HalDevice *d, 
					gpointer data1, gpointer data2, HalHelperTerminatedCB cb, guint timeout);

void hal_util_terminate_helper (HalHelperData *helper_data);

gchar **hal_util_dup_strv_from_g_slist (GSList *strlist);

typedef void (*HalCalloutsDone) (HalDevice *d, gpointer userdata1, gpointer userdata2);

void hal_util_callout_device_add (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2);
void hal_util_callout_device_remove (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2);
void hal_util_callout_device_preprobe (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2);

void hal_util_hexdump (const void *buf, unsigned int size);

#define HAL_HELPER_TIMEOUT 10000

#define HAL_PATH_MAX 256

#endif /* UTIL_H */
