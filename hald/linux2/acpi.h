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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifndef ACPI_H
#define ACPI_H

#include "../hald.h"
#include "hotplug.h"

void acpi_synthesize_hotplug_events (void);

void hotplug_event_begin_add_acpi (const gchar *acpi_path, int acpi_type, HalDevice *parent, void *end_token);

void hotplug_event_begin_remove_acpi (const gchar *acpi_path, int acpi_type, void *end_token);

gboolean acpi_rescan_device (HalDevice *d);

HotplugEvent *acpi_generate_add_hotplug_event (HalDevice *d);

HotplugEvent *acpi_generate_remove_hotplug_event (HalDevice *d);

#endif /* ACPI_H */
