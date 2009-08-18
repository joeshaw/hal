/***************************************************************************
 * CVSID: $Id$
 *
 * hfp.c : utility library for HAL probers
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <syslog.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <libgen.h>

#include "hfp.h"

LibHalContext *hfp_ctx = NULL;
char *hfp_udi = NULL;
DBusError hfp_error;

static char *log_domain;
static boolean verbose = FALSE;
static boolean use_syslog = FALSE;

boolean
hfp_init (int argc, char **argv)
{
  char *psname = NULL;

  assert(hfp_ctx == NULL);

  if (argc > 0)
    psname = basename(argv[0]);

  if (psname)
    log_domain = hfp_strdup(psname);
  else
    log_domain = "hfp";

  if (getenv("HALD_VERBOSE"))
    verbose = TRUE;
  if (getenv("HALD_USE_SYSLOG"))
    use_syslog = TRUE;

  hfp_udi = getenv("UDI");
  if (! hfp_udi)
    return FALSE;

  dbus_error_init(&hfp_error);
  hfp_ctx = libhal_ctx_init_direct(&hfp_error);
  LIBHAL_FREE_DBUS_ERROR(&hfp_error);

  return TRUE;
}

static void
hfp_no_memory (void)
{
  hfp_critical("memory allocation failure");
  abort();
}

void *
hfp_malloc (size_t size)
{
  void *mem;

  mem = malloc(size);
  if (! mem)
    hfp_no_memory();

  return mem;
}

void *
hfp_malloc0 (size_t size)
{
  void *mem;

  mem = calloc(1, size);
  if (! mem)
    hfp_no_memory();

  return mem;
}

char *
hfp_strdup (const char *str)
{
  char *copy;

  if (str)
    {
      copy = strdup(str);
      if (! copy)
	hfp_no_memory();
    }
  else
    copy = NULL;

  return copy;
}

char *
hfp_strndup (const char *str, size_t size)
{
  char *copy;

  if (str)
    {
      copy = hfp_new(char, size + 1);
      strncpy(copy, str, size);
      copy[size] = 0;
    }
  else
    copy = NULL;

  return copy;
}

char *
hfp_strdup_printf (const char *format, ...)
{
  va_list args;
  char *str;

  assert(format != NULL);

  va_start(args, format);
  if (vasprintf(&str, format, args) == -1)
    hfp_no_memory();
  va_end(args);

  return str;
}

void
hfp_free (void *mem)
{
  if (mem)
    free(mem);
}

void
hfp_logv (HFPLogLevel log_level, const char *format, va_list args)
{
  assert(log_level == HFP_LOG_LEVEL_DEBUG
	 || log_level == HFP_LOG_LEVEL_INFO
	 || log_level == HFP_LOG_LEVEL_WARNING
	 || log_level == HFP_LOG_LEVEL_CRITICAL);
  assert(format != NULL);

  if (verbose)
    {
      static const char *log_levels[] = { "debug", "info", "WARNING", "CRITICAL" };
      static const int syslog_levels[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_CRIT };
      if (use_syslog)
        vsyslog(syslog_levels[log_level], format, args);
      else
        {
          fprintf(stderr, "%s %s: ", log_domain, log_levels[log_level]);
	  vfprintf(stderr, format, args);
	  fprintf(stderr, "\n");
	}
    }
}

#define LOG_FUNCTION(name, level) \
  void name (const char *format, ...)			\
  {							\
    va_list args;					\
							\
    assert(format != NULL);				\
							\
    va_start(args, format);				\
    hfp_logv(HFP_LOG_LEVEL_ ## level, format, args);	\
    va_end(args);					\
  }

LOG_FUNCTION(hfp_info, INFO)
LOG_FUNCTION(hfp_warning, WARNING)
LOG_FUNCTION(hfp_critical, CRITICAL)
LOG_FUNCTION(volume_id_log, DEBUG) /* used by volume_id */

boolean
hfp_getenv_bool (const char *variable)
{
  char *value;

  assert(variable != NULL);

  value = getenv(variable);

  return value && ! strcmp(value, "true");
}

void
hfp_clock_gettime (struct timespec *t)
{
  int status;

  assert(t != NULL);

#ifdef CLOCK_MONOTONIC_FAST
  status = clock_gettime(CLOCK_MONOTONIC_FAST, t);
#else
  status = clock_gettime(CLOCK_MONOTONIC, t);
#endif
  assert(status == 0);
}

/* timespec functions from sys/kern/kern_time.c */

static void
hfp_timespecfix (struct timespec *t)
{
  assert(t != NULL);

  if (t->tv_nsec < 0)
    {
      t->tv_sec--;
      t->tv_nsec += 1000000000;
    }
  if (t->tv_nsec >= 1000000000)
    {
      t->tv_sec++;
      t->tv_nsec -= 1000000000;
    }
}

void
hfp_timespecadd (struct timespec *t1, const struct timespec *t2)
{
  assert(t1 != NULL);
  assert(t2 != NULL);

  t1->tv_sec += t2->tv_sec;
  t1->tv_nsec += t2->tv_nsec;

  hfp_timespecfix(t1);
}

void
hfp_timespecsub (struct timespec *t1, const struct timespec *t2)
{
  assert(t1 != NULL);
  assert(t2 != NULL);

  t1->tv_sec -= t2->tv_sec;
  t1->tv_nsec -= t2->tv_nsec;

  hfp_timespecfix(t1);
}
