/***************************************************************************
 * CVSID: $Id$
 *
 * logger.c : Logging 
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
#include <stdarg.h>

#include "logger.h"

/**
 * @defgroup HalDaemonLogging Logging system
 * @ingroup HalDaemon
 * @brief Logging system for the HAL daemon
 * @{
 */


static int priority;
static const char *file;
static int line;
static const char *function;

/** Initialize logging system
 *
 */
void
logger_init ()
{
}

/** Setup logging entry
 *
 *  @param  priority            Logging priority, one of HAL_LOGPRI_*
 *  @param  file                Name of file where the log entry originated
 *  @param  line                Line number of file
 *  @param  function            Name of function
 */
void
logger_setup (int _priority, const char *_file, int _line,
	      const char *_function)
{
	priority = _priority;
	file = _file;
	line = _line;
	function = _function;
}

/** Emit logging entry
 *
 *  @param  format              Message format string, printf style
 *  @param  ...                 Parameters for message, printf style
 */
void
logger_emit (const char *format, ...)
{
	va_list args;
	char buf[512];
	char *pri;

	va_start (args, format);
	vsnprintf (buf, 512, format, args);

	switch (priority) {
	case HAL_LOGPRI_TRACE:
		pri = "[T]";
		break;
	case HAL_LOGPRI_DEBUG:
		pri = "[D]";
		break;
	case HAL_LOGPRI_INFO:
		pri = "[I]";
		break;
	case HAL_LOGPRI_WARNING:
		pri = "[W]";
		break;
	default:		/* explicit fallthrough */
	case HAL_LOGPRI_ERROR:
		pri = "[E]";
		break;
	}

	/** @todo Make programmatic interface to logging */
	if (priority != HAL_LOGPRI_TRACE)
		fprintf (stderr, "%s %s:%d %s() : %s\n",
			 pri, file, line, function, buf);

	va_end (args);
}


/** @} */
