/***************************************************************************
 * CVSID: $Id$
 *
 * Copyright (C) 2005 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2005 David Zeuthen, Red Hat Inc., <davidz@redhat.com>
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

#ifndef ACPI_H
#define ACPI_H

#include "../hald.h"
#include "hotplug.h"

#ifdef HAVE_ACPI
gboolean acpi_synthesize_hotplug_events (void);
void hotplug_event_begin_add_acpi (const gchar *acpi_path, int acpi_type, HalDevice *parent, void *end_token);
void hotplug_event_begin_remove_acpi (const gchar *acpi_path, int acpi_type, void *end_token);
gboolean acpi_rescan_device (HalDevice *d);
HotplugEvent *acpi_generate_add_hotplug_event (HalDevice *d);
HotplugEvent *acpi_generate_remove_hotplug_event (HalDevice *d);
void acpi_check_is_laptop (const gchar *acpi_type);
#else /* HAVE_ACPI */
static inline gboolean acpi_synthesize_hotplug_events (void) {return FALSE;}
static inline void hotplug_event_begin_add_acpi (const gchar *acpi_path, int acpi_type, HalDevice *parent, void *end_token) {return;}
static inline void hotplug_event_begin_remove_acpi (const gchar *acpi_path, int acpi_type, void *end_token) {return;}
static inline gboolean acpi_rescan_device (HalDevice *d) {return FALSE;}
static inline HotplugEvent *acpi_generate_add_hotplug_event (HalDevice *d) {return NULL;}
static inline HotplugEvent *acpi_generate_remove_hotplug_event (HalDevice *d) {return NULL;}
static inline void acpi_check_is_laptop (const gchar *acpi_type) {return;}
#endif /* HAVE_ACPI */

#endif /* ACPI_H */
