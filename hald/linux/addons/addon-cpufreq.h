/***************************************************************************
 *                                                                         *
 *                            addon-cpufreq.h                              *
 *                                                                         *
 *              Copyright (C) 2006 SUSE Linux Products GmbH                *
 *                                                                         *
 *               Author(s): Holger Macht <hmacht@suse.de>                  *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at you   *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU       *
 * General Public License for more details.                                *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA                  *
 *                                                                         *
 ***************************************************************************/

#ifndef ADDON_CPUFREQ_H
#define ADDON_CPUFREQ_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

/** UP_THRESHOLD defines at which CPU load (in percent) we switch up */
#define UP_THRESHOLD_MAX		99
#define UP_THRESHOLD_MIN		11
/** this is the kernel default up_threshold */
#define UP_THRESHOLD_BASE		80
#define DEFAULT_PERFORMANCE		50

struct cpufreq_obj {
	void	 *iface;
	gboolean (*set_performance)   (void *data, int);
	gboolean (*set_consider_nice) (void *data, gboolean);
	int      (*get_performance)   (void);
	gboolean (*get_consider_nice) (void);
	void     (*free)              (void *data);
};

gboolean	write_line		(const char *filename,
					 const char *fmt, ...);

gboolean	read_line		(const char *filename,
					 char *line,
					 unsigned len);

gboolean	read_line_int_split	(char *filename,
					 gchar *delim,
					 GSList **list);

gboolean	cpu_online		(int cpu_id);

gboolean	write_governor		(char *new_governor,
					 int cpu_id);

gboolean	dbus_init		(void);

gboolean	dbus_init_local		(void);

#endif /* ADDON_CPUFREQ_H */
