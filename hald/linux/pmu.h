/***************************************************************************
 * CVSID: $Id$
 *
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

#ifndef PMU_H
#define PMU_H

#include "../hald.h"
#include "hotplug.h"

#ifdef HAVE_PMU
gboolean pmu_synthesize_hotplug_events (void);
void hotplug_event_begin_add_pmu (const gchar *pmu_path, int pmu_type, HalDevice *parent, void *end_token);
void hotplug_event_begin_remove_pmu (const gchar *pmu_path, int pmu_type, void *end_token);
gboolean pmu_rescan_device (HalDevice *d);
HotplugEvent *pmu_generate_add_hotplug_event (HalDevice *d);
HotplugEvent *pmu_generate_remove_hotplug_event (HalDevice *d);
#else /* HAVE_PMU */
static inline gboolean pmu_synthesize_hotplug_events (void) {return FALSE;}
static inline void hotplug_event_begin_add_pmu (const gchar *pmu_path, int pmu_type, HalDevice *parent, void *end_token) {return;}
static inline void hotplug_event_begin_remove_pmu (const gchar *pmu_path, int pmu_type, void *end_token) {return;}
static inline gboolean pmu_rescan_device (HalDevice *d) {return FALSE;}
static inline HotplugEvent *pmu_generate_add_hotplug_event (HalDevice *d) {return NULL;}
static inline HotplugEvent *pmu_generate_remove_hotplug_event (HalDevice *d) {return NULL;}
#endif /* HAVE_PMU */

#endif /* PMU_H */
