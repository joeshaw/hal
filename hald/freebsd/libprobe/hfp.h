/***************************************************************************
 * CVSID: $Id$
 *
 * hfp.h : utility library for HAL probers
 *
 * Copyright (C) 2006 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#ifndef _HFP_H
#define _HFP_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <sys/param.h>
#if __FreeBSD_version < 600000
#include <sys/time.h>
#endif

#include "libhal/libhal.h"

/* from GLib */
#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
#define HFP_GNUC_PRINTF(format_idx, arg_idx) \
  __attribute__((__format__(__printf__, format_idx, arg_idx)))
#else
#define HFP_GNUC_PRINTF(format_idx, arg_idx)
#endif

/* from GLib */
#define HFP_N_ELEMENTS(arr)		(sizeof(arr) / sizeof((arr)[0]))

typedef int boolean;

typedef enum
{
  HFP_LOG_LEVEL_DEBUG,
  HFP_LOG_LEVEL_INFO,
  HFP_LOG_LEVEL_WARNING,
  HFP_LOG_LEVEL_CRITICAL
} HFPLogLevel;

extern LibHalContext *hfp_ctx;
extern char *hfp_udi;
extern DBusError hfp_error;

boolean hfp_init (int argc, char **argv);

void *hfp_malloc (size_t size);
void *hfp_malloc0 (size_t size);

#define hfp_new(type, number)	((type *) hfp_malloc(sizeof(type) * number))
#define hfp_new0(type, number)	((type *) hfp_malloc0(sizeof(type) * number))

char *hfp_strdup (const char *str);
char *hfp_strndup (const char *str, size_t size);
char *hfp_strdup_printf (const char *format, ...) HFP_GNUC_PRINTF(1, 2);

void hfp_free (void *mem);

void hfp_logv (HFPLogLevel log_level, const char *format, va_list args);

void hfp_info (const char *format, ...) HFP_GNUC_PRINTF(1, 2);
void hfp_warning (const char *format, ...) HFP_GNUC_PRINTF(1, 2);
void hfp_critical (const char *format, ...) HFP_GNUC_PRINTF(1, 2);

/* this is used by volume_id */
void volume_id_log (const char *format, ...) HFP_GNUC_PRINTF(1, 2);

boolean hfp_getenv_bool (const char *variable);

void hfp_clock_gettime (struct timespec *t);
void hfp_timespecadd (struct timespec *t1, const struct timespec *t2);
void hfp_timespecsub (struct timespec *t1, const struct timespec *t2);

/* from sys/time.h (_KERNEL) */
#define hfp_timespeccmp(t1, t2, cmp) \
  (((t1)->tv_sec == (t2)->tv_sec	\
    ? ((t1)->tv_nsec cmp (t2)->tv_nsec)	\
    : ((t1)->tv_sec cmp (t2)->tv_sec)))

#endif /* _HFP_H */
