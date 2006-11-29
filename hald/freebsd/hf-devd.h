/***************************************************************************
 * CVSID: $Id$
 *
 * hf-devd.h : process devd events
 *
 * Copyright (C) 2006 Joe Marcus Clarke <marcus@FreeBSD.org>
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

#ifndef _HF_DEVD_H
#define _HF_DEVD_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>

#include "hf-osspec.h"

extern HFHandler hf_devd_handler;

typedef struct
{
  /* return TRUE to consume the event and stop processing */

  gboolean (*add)	(const char *name,
			 GHashTable *params,	/* values may be NULL */
			 GHashTable *at,	/* values may be NULL */
			 const char *parent);	/* may be NULL */
  gboolean (*remove)	(const char *name,
			 GHashTable *params,	/* values may be NULL */
			 GHashTable *at,	/* values may be NULL */
			 const char *parent);	/* may be NULL */
  gboolean (*notify)	(const char *system,
			 const char *subsystem,
			 const char *type,
			 const char *data);	/* may be NULL */
  gboolean (*nomatch)	(GHashTable *at,	/* values may be NULL */
			 const char *parent);	/* may be NULL */
} HFDevdHandler;

#endif /* _HF_DEVD_H */
