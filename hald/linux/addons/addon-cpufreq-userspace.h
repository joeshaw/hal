/***************************************************************************
 *                                                                         *
 *                      addon-cpufreq-userspace.h                          *
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

#ifndef ADDON_CPUFREQ_USERSPACE_H
#define ADDON_CPUFREQ_USERSPACE_H

#define USERSPACE_STRING	"userspace"
#define USERSPACE_POLL_INTERVAL	333

struct userspace_interface {
	int	base_cpu;
	int	last_step;
	int	current_speed;
	int	g_source_id;
	int	prev_cpu_load;
	GSList	*cpus;
	GArray	*speeds_kHz;
	GArray	*demotion;
};

gboolean	userspace_adjust_speeds		(GSList *cpufreq_objs);

gboolean	userspace_init			(struct userspace_interface *iface,
						 GSList *cpus);

gboolean	userspace_set_performance	(void *data,
						 int up_threshold);

int		userspace_get_performance	(void);

gboolean	userspace_set_consider_nice	(void *data,
						 gboolean consider);

gboolean	userspace_get_consider_nice	(void);

void		userspace_free			(void *data);

void		free_cpu_load_data		(void);

#endif /* ADDON_CPUFREQ_USERSPACE_H */
