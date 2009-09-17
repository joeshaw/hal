/***************************************************************************
 * CVSID: $Id$
 *
 * hf-util.h : utilities
 *
 * Copyright (C) 2006, 2007 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#ifndef _HF_UTIL_H
#define _HF_UTIL_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdarg.h>
#include <glib.h>

#include "../hald.h"

#define HF_COMPUTER		"/org/freedesktop/Hal/devices/computer"

#define HF_BOOL_TO_STRING(val)	((val) ? "true" : "false")

#define HF_LIST_FOREACH(var, head)	\
  for ((var) = (head);			\
       (var);				\
       (var) = (var)->next)

/*
 * The hf_is_waiting variable is set to true when hald is waiting in
 * one of the following functions:
 *
 *	hf_device_preprobe()
 *	hf_device_add()
 *	hf_device_preprobe_and_add()
 *	hf_device_remove()
 *	hf_device_remove_children()
 *	hf_device_remove_tree()
 *	hf_runner_run_sync()
 *
 * Since these functions wait by recursing into the GLib main loop,
 * main loop callbacks can be executed during the wait and cause
 * undesirable side effects. For instance, an USB attach event for a
 * child device could be received while the parent device is still
 * being processed in hf_usb_probe_device().
 *
 * Main loop callbacks must therefore do nothing and return if
 * hf_is_waiting is true.
 */
extern int hf_is_waiting;

HalDevice *hf_device_new (HalDevice *parent);

gboolean hf_device_preprobe (HalDevice *device);
void hf_device_add (HalDevice *device);
gboolean hf_device_preprobe_and_add (HalDevice *device);
void hf_device_remove (HalDevice *device);
void hf_device_remove_children (HalDevice *device);
void hf_device_remove_tree (HalDevice *device);

void hf_device_set_udi (HalDevice *device,
			const char *format,
			...) G_GNUC_PRINTF(2, 3);
void hf_device_set_full_udi (HalDevice *device,
			     const char *format,
			     ...) G_GNUC_PRINTF(2, 3);

void hf_device_property_set_string_printf (HalDevice *device,
					   const char *key,
					   const char *format,
					   ...) G_GNUC_PRINTF(3, 4);

void hf_device_set_input (HalDevice *device,
			  const char *capability1,
			  const char *capability2,
			  const char *devname);

HalDevice *hf_device_store_get_parent (HalDeviceStore *store,
				       HalDevice *device);
GSList *hf_device_store_get_children (HalDeviceStore *store,
				      HalDevice *device);

gboolean hf_has_sysctl (const char *format, ...) G_GNUC_PRINTF(1, 2);
gboolean hf_get_int_sysctl (int *value,
			    GError **err,
			    const char *format,
			    ...) G_GNUC_PRINTF(3, 4);
char *hf_get_string_sysctl (GError **err,
			    const char *format,
			    ...) G_GNUC_PRINTF(2, 3);

char *hf_run (GError **err, const char *format, ...) G_GNUC_PRINTF(2, 3);

int hf_runner_run_sync (HalDevice *device, int timeout, const char *command_line, ...);

int hf_strv_find (char **strv, const char *elem);

char *hf_str_escape (const char *str);

HalDevice *hf_device_store_match (HalDeviceStore *store, ...);

#endif /* _HF_UTIL_H */
