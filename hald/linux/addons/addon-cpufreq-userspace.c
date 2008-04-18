/***************************************************************************
 *                                                                         *
 *                      addon-cpufreq-userspace.c                          *
 *                                                                         *
 *              Copyright (C) 2006 SUSE Linux Products GmbH                *
 *                                                                         *
 *              Author(s): Holger Macht <hmacht@suse.de>                   *
 *                         Speed adjustments based on code by              *
 *                           Thomas Renninger <trenn@suse.de>              *
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "addon-cpufreq.h"
#include "addon-cpufreq-userspace.h"
#include "../../logger.h"

/* at which load difference (in percent) we should immediately switch to
 * the maximum possible frequency */
#define JUMP_CPUFREQ_LIMIT_MIN		20
/* the load difference at which we jump up to the maximum freq
 * immediately is calculated by the UP_THRESHOLD multiplied with this
 * relation value */
#define THRESHOLD_JUMP_LIMIT_RELATION	0.625
/* how many frequency steps we should consider */
#define HYSTERESIS			5
#define DEFAULT_CONSIDER_NICE		FALSE
#define PROC_STAT_FILE			"/proc/stat"

static const char SYSFS_SCALING_SETSPEED_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_setspeed";

static const char SYSFS_SCALING_AVAILABLE_FREQS_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_available_frequencies";

/* shortcut for g_array_index */
#define g_a_i(a,i)	g_array_index(a, unsigned, i)

struct userspace_config {
	int up_threshold;
	int cpu_high_limit;
	int consider_nice;
	int performance;
};

static struct userspace_config config = { UP_THRESHOLD_MAX,
					  JUMP_CPUFREQ_LIMIT_MIN,
					  DEFAULT_CONSIDER_NICE,
					  DEFAULT_PERFORMANCE };

/********************* CPU load calculation *********************/
struct cpuload_data {
	int		num_cpus;
	int		*load;
	unsigned long	*last_total_time;
	unsigned long	*last_working_time;
};
static struct cpuload_data cpuload = { -1,
				       NULL,
				       NULL,
				       NULL };

/** 
 * free_cpu_load_data:
 * 
 * frees data needed for CPU load calculation 
 */
void free_cpu_load_data(void)
{
	if (cpuload.num_cpus != -1) {
		free(cpuload.last_working_time);
		free(cpuload.last_total_time);
		free(cpuload.load);
		cpuload.num_cpus = -1;
		cpuload.load = NULL;
		cpuload.last_total_time = NULL;
		cpuload.last_working_time = NULL;
	}
}

/** 
 * calc_cpu_load:
 * @consider_nice:
 * 
 * Returns:
 * 
 * calculates current cpu load and stores it in cpuload_data object 
 */
static int calc_cpu_load(const int consider_nice)
{
	unsigned long	total_elapsed, working_elapsed;
	char		what[32];
	unsigned long	user_time, nice_time, system_time, idle_time;
	unsigned long	total_time, iowait_time;
	unsigned	scan_ret;
	char		line[256];
	char		cpu_string[7];
	FILE		*fp;
	int		new_num_cpus, i;

	new_num_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (new_num_cpus == -1 || new_num_cpus != cpuload.num_cpus) {
		free_cpu_load_data();
		cpuload.num_cpus = new_num_cpus;
		if (cpuload.num_cpus <= 0) {
			errno = ENODEV;
			return -20;
		}

		cpuload.last_total_time = (unsigned long *)calloc(cpuload.num_cpus + 1,
								  sizeof(unsigned long));
		cpuload.last_working_time = (unsigned long *)calloc(cpuload.num_cpus + 1,
								    sizeof(unsigned long));
		cpuload.load = (int *)calloc(cpuload.num_cpus + 1, sizeof(int));
	}

	if ((fp = fopen(PROC_STAT_FILE, "r")) == NULL) {
		HAL_DEBUG(("Could not open %s: %s", PROC_STAT_FILE, strerror(errno)));
		return -1;
	}

	/* start with the first line, "overall" cpu load */
	/* if cpuload.num_cpus == 1, we do not need to evaluate "overall" and "per-cpu" load */
	sprintf(cpu_string, "cpu ");

	for (i = 0; i <= cpuload.num_cpus - (cpuload.num_cpus == 1); i++) {
		unsigned long working_time;
		
		if (fgets(line,255,fp) == NULL) {
			HAL_WARNING(("%s too short (%s)", PROC_STAT_FILE, cpu_string));
			fclose(fp);
			return -1;
		}
		if (memcmp(line, cpu_string, strlen(cpu_string))) {
			HAL_WARNING(("no '%s' string in %s line %d",
				     cpu_string, PROC_STAT_FILE, i));
			fclose(fp);
			return -1;
		}
		/* initialized, since it is simply not there in 2.4 */
		iowait_time = 0;
		scan_ret = sscanf(line, "%s %lu %lu %lu %lu %lu", what, &user_time, &nice_time,
			  &system_time, &idle_time, &iowait_time);
		if (scan_ret < 5) {
			HAL_WARNING(("only %d values in %s. Please report.",
				     scan_ret, PROC_STAT_FILE));
			fclose(fp);
			return -1;
		}

		if (consider_nice) {
			working_time = user_time + system_time + nice_time;
			idle_time += iowait_time;
		} else {
			working_time = user_time + system_time;
			idle_time += (nice_time + iowait_time);
		}
		total_time = working_time + idle_time;
		total_elapsed = total_time - cpuload.last_total_time[i];
		working_elapsed = working_time - cpuload.last_working_time[i];
		cpuload.last_working_time[i] = working_time;
		cpuload.last_total_time[i] = total_time;
		
		if (!total_elapsed) {
			/* not once per CPU, only once per check. */
			if (!i)
				HAL_DEBUG(("%s not updated yet, poll slower.", PROC_STAT_FILE));
		} else
			cpuload.load[i] = working_elapsed * 100 / total_elapsed;

		sprintf(cpu_string, "cpu%d ", i);
	}
	/* shortcut for UP systems */
	if (cpuload.num_cpus == 1)
		cpuload.load[1] = cpuload.load[0];

	fclose(fp);
	
	return 0;
}

/** 
 * get_cpu_load:
 * @cpu_id:	The ID of the CPU
 * 
 * Returns:     returns current cpuload which has been caluclated before 
 *
 * To get the current CPU load for a given CPU.
 */
static int get_cpu_load(const int cpu_id)
{
	if (cpu_id < -1) {
		errno = EINVAL;
		return -10;
	}

	if (cpuload.load == NULL) {
		HAL_WARNING(("cpuload.load uninitialized"));
		errno = EFAULT;
		return -40;
	}

	if (cpu_id >= cpuload.num_cpus) {
		errno = ENODEV;
		return -30;
	}

	return cpuload.load[cpu_id + 1];
}
/********************* CPU load end *********************/

/********************* userspace interface *********************/
static gboolean write_speed(unsigned kHz, int cpu_id)
{
	char		*speed_file	= NULL;

	if (!cpu_online(cpu_id))
		return FALSE;

	speed_file = g_strdup_printf(SYSFS_SCALING_SETSPEED_FILE, cpu_id); 
        if(!write_line(speed_file, "%u", kHz)){
                HAL_WARNING(("Could not set speed to: %u kHz; %s", kHz, strerror(errno)));
		g_free(speed_file);
                return FALSE;
        }
	g_free(speed_file);
	HAL_DEBUG(("Speed set to: %uKHz  for CPU %d", kHz, cpu_id));

	return TRUE;
}

static void reinit_speed(struct userspace_interface *iface, int current_speed)
{
	if (!cpu_online(iface->base_cpu))
		return;

	write_speed(g_a_i(iface->speeds_kHz, current_speed), iface->base_cpu);
	HAL_DEBUG(("forced speed to %d kHz", g_a_i(iface->speeds_kHz, current_speed)));
}

/**
 * set_speed:
 * @iface: 		struct with the userspace interface
 * @target_speed	the speed to set
 *
 * Returns:		the current speed as integer value
 *
 * Set a speed with traversing all intermediary speeds 
 */
static int set_speed(struct userspace_interface *iface, int target_speed)
{
	int delta;
	int current_speed = iface->current_speed;

	if (current_speed == target_speed)
		return -1;

	if (current_speed > target_speed)
		delta = -1;
	else
		delta = 1;

	do {
		current_speed += delta;
		write_speed(g_a_i(iface->speeds_kHz, current_speed), iface->base_cpu);
	} while (current_speed != target_speed);

	return current_speed;
}

/** 
 * increase_speed:
 * @iface:	struct with the userspace interface
 *
 * Returns:	integer with result of increase speed
 * 		0 if maximum is already reached
 * 		1 if new speed could be set
 * 		-1 if mode is not userspace
 *
 * set speed to the next higher supported value
 */
static int increase_speed(struct userspace_interface *iface)
{
	int new_speed = iface->current_speed;
	int current_speed = iface->current_speed;

	if (current_speed != 0)
		new_speed--;
	else
		return current_speed;
	if (current_speed != new_speed) {
		HAL_DEBUG(("current: %u new: %u", g_a_i(iface->speeds_kHz, current_speed),
			   g_a_i(iface->speeds_kHz, new_speed)));
		set_speed(iface, new_speed);
	}
	return new_speed;
}

/** 
 * decrease_speed:
 * @iface:	struct with the userspace interface
 *
 * Returns:	integer with result of decrease speed
 *
 * set speed to the next lower supported value
 */
static int decrease_speed(struct userspace_interface *iface)
{
	int new_speed = iface->current_speed;
	int current_speed = iface->current_speed;

	
	if (g_a_i(iface->speeds_kHz, new_speed + 1) != 0)
		new_speed++;
	else
		return current_speed;
	if (current_speed != new_speed) {
		HAL_DEBUG(("current: %u new: %u", g_a_i(iface->speeds_kHz, current_speed),
			   g_a_i(iface->speeds_kHz, new_speed)));
		set_speed(iface, new_speed);
	}
	return new_speed;
}

/** 
 * adjust_speed:
 * @iface:	struct with the userspace interface
 *
 * Returns:	TRUE/FALSE
 *
 * increases and decreases speeds
 */
static gboolean adjust_speed(struct userspace_interface *iface)
{
	GSList		*cpus	 = (GSList*)iface->cpus;
	GSList		*it	 = NULL;
	int		cpu_load = 0;

	for (it = cpus; it != NULL; it = g_slist_next(it)) {
		HAL_DEBUG(("checking cpu %d: cpu_core: %d",
			   GPOINTER_TO_INT(it->data), GPOINTER_TO_INT(it->data)));
		if (get_cpu_load(GPOINTER_TO_INT(it->data)) > cpu_load)
			 cpu_load = get_cpu_load(GPOINTER_TO_INT(it->data));
	}

	HAL_DEBUG(("cpu_max: %d cpu_high_limit: %d consider_nice: %d",
		   config.up_threshold, config.cpu_high_limit,
		   config.consider_nice));
	HAL_DEBUG(("Current: %u; current speed: %u MHz", 
		   iface->current_speed, g_a_i(iface->speeds_kHz, iface->current_speed)));
	HAL_DEBUG(("CPU load: %d, Previous CPU load %d, cpu_load diff: %d, last_step: %d, demotion: %u",
		   cpu_load, iface->prev_cpu_load, cpu_load - iface->prev_cpu_load, iface->last_step,
		   g_a_i(iface->demotion, iface->current_speed)));

	/* directly increase speed to maximum if cpu load jumped */
	if (config.cpu_high_limit &&
	    (cpu_load - iface->prev_cpu_load) > config.cpu_high_limit) {
		if (iface->current_speed != 0) {
			set_speed(iface, 0);
			iface->current_speed = 0;
			HAL_DEBUG(("jumped to max (%d kHz)", 
				   g_a_i(iface->speeds_kHz, iface->current_speed)));
		}
	} else if (cpu_load > config.up_threshold && iface->current_speed > 0) {
		iface->current_speed = increase_speed(iface);
		HAL_DEBUG(("increased to %d kHz", g_a_i(iface->speeds_kHz, iface->current_speed)));
	} else if (cpu_load < (int)g_a_i(iface->demotion, iface->current_speed) &&
		   iface->current_speed < iface->last_step) {
		iface->current_speed = decrease_speed(iface);
		HAL_DEBUG(("decreased to %d kHz", g_a_i(iface->speeds_kHz, iface->current_speed)));
	} else {
		HAL_DEBUG(("Speed not changed"));
	}

	iface->prev_cpu_load = cpu_load;
	return TRUE;
}

/* create the hysteresis array */
static void create_hysteresis_array(struct userspace_interface *iface)
{
	int i;
	
	g_array_free(iface->demotion, TRUE);
	iface->demotion = g_array_new(TRUE, TRUE, sizeof(unsigned));

	if (iface->last_step > 0) {
		for (i = 0; i < iface->last_step; i++) {
			int demotion = (config.up_threshold - HYSTERESIS) *
				g_a_i(iface->speeds_kHz, i + 1) / 
				g_a_i(iface->speeds_kHz, i);
			g_array_append_val(iface->demotion, demotion);
			HAL_DEBUG(("Speed: %2u, kHz: %9u, demotion: %3u %%", i,
				   g_a_i(iface->speeds_kHz, i), g_a_i(iface->demotion, i)));
		}
	}
}

static gboolean read_frequencies(struct userspace_interface *iface)
{
	int	num_speeds			= 0;
	GSList	*it				= NULL;
	GSList	*available_freqs		= NULL;
	char	*available_frequencies_file	= NULL;
	
	if (!cpu_online(iface->base_cpu))
		return FALSE;

	available_frequencies_file = g_strdup_printf(SYSFS_SCALING_AVAILABLE_FREQS_FILE,
						     iface->base_cpu); 
	if (!read_line_int_split(available_frequencies_file, " ", &available_freqs)) {
		g_free(available_frequencies_file);
		return FALSE;
	}
	g_free(available_frequencies_file);
	
	if (available_freqs == NULL) {
		iface->last_step = 0;
		return FALSE;
	}
	
	for (num_speeds = 0, it = available_freqs; it != NULL;
	     num_speeds++, it = g_slist_next(it)) {

		unsigned index = GPOINTER_TO_UINT(it->data);
		g_array_append_val(iface->speeds_kHz, index);
	}
	g_slist_free(available_freqs);
	
	iface->last_step = num_speeds - 1;
	HAL_DEBUG(("Number of speeds: %d, last_step: %d", num_speeds, iface->last_step));
	
	reinit_speed(iface, 0);
	
	HAL_DEBUG(("Available speeds:"));
	for (num_speeds = 0; g_a_i(iface->speeds_kHz, num_speeds); num_speeds++) {
		HAL_DEBUG((" %2u: %9uKHz", num_speeds, g_a_i(iface->speeds_kHz, num_speeds)));
	}

	return TRUE;
}

/** 
 * userspace_adjust_speeds:
 * @cpufreq_objs:	List with with CPU Freq objects
 *
 * Returns:		Result of the call (TRUE/FALSE)
 *
 * calculates current cpu load and traverses all existing interfaces 
 */
gboolean userspace_adjust_speeds(GSList *cpufreq_objs)
{
	GSList *it = NULL;

	HAL_DEBUG(("Adjusting speeds..."));

	if ((calc_cpu_load(DEFAULT_CONSIDER_NICE) < 0)) {
		HAL_DEBUG(("calc_cpu_load failed. Cannot adjust speeds"));
		return TRUE;
	}

	for (it = cpufreq_objs; it != NULL; it = g_slist_next(it)) {
		struct cpufreq_obj *obj = it->data;
		adjust_speed(obj->iface);
	}

	return TRUE;
}

/** 
 * userspace_init:
 * @iface:	allocated struct with the userspace interface
 * @cpus:	list of CPU cores
 *
 * Returns:     TRUE/FALSE
 *
 * Inits one userspace interface with the given cores list. iface has to
 * be allocated before passing it to that fucntion 
 */
gboolean userspace_init(struct userspace_interface *iface, GSList *cpus)
{
	if (iface == NULL)
		return FALSE;

	iface->demotion		= g_array_new(TRUE, TRUE, sizeof(unsigned));
	iface->speeds_kHz	= g_array_new(TRUE, TRUE, sizeof(unsigned));
	iface->last_step	= -1;
	iface->current_speed	= 0;
	iface->cpus		= cpus;
	iface->prev_cpu_load	= 50;
	iface->base_cpu		= GPOINTER_TO_INT(cpus->data);
	
	if (!write_governor(USERSPACE_STRING, GPOINTER_TO_INT(cpus->data))) {
		HAL_WARNING(("Could not set userspace governor."));
		return FALSE;
	}

	if (!read_frequencies(iface)) {
		HAL_WARNING(("Could not read available frequencies"));
		return FALSE;
	}

	return TRUE;
}

/** 
 * userspace_free:
 * @data:	userspace interface object to free
 * 
 * frees the userspace data 
 */
void userspace_free(void *data)
{
	struct userspace_interface *iface = data;
	free_cpu_load_data();
	g_array_free(iface->speeds_kHz, TRUE);
	g_array_free(iface->demotion, TRUE);
}

/** 
 * userspace_set_performance:
 * @data:		userspace interface struct
 * @up_threshold:	performance value for the userspace governor
 * 
 * Returns:		TRUE/FALSE
 * 
 * Sets the performance of the userspace governor. num has to be between 1 and 100 
 */
gboolean userspace_set_performance(void *data, int up_threshold)
{
	struct userspace_interface *iface = data;

	config.up_threshold = up_threshold;

	config.cpu_high_limit = (int)(up_threshold * THRESHOLD_JUMP_LIMIT_RELATION);
	if (config.cpu_high_limit < JUMP_CPUFREQ_LIMIT_MIN)
		config.cpu_high_limit = JUMP_CPUFREQ_LIMIT_MIN;

	HAL_DEBUG(("cpu_max set to %d, cpu_high_limit set to %d",
		   config.up_threshold, config.cpu_high_limit));

	create_hysteresis_array(iface);

	return TRUE;
}

/** 
 * userspace_get_performance:
 * 
 * Returns:	current performance setting
 *
 * Return the current performance setting 
 */
int userspace_get_performance(void)
{
	return config.up_threshold;
}

/** 
 * userspace_set_consider_nice_
 * @data:	void pointer
 * @consider:   if process should be considered nice
 * 
 * Returns:	TRUE/FALSE
 * 
 * sets whether niced processes should be considered when calculating CPU load 
*/
gboolean userspace_set_consider_nice(void *data, gboolean consider)
{
	HAL_DEBUG(("consider nice set to %d for userspace", consider));
	config.consider_nice = consider;
	return TRUE;
}

/** 
 * userspace_get_consider_nice:
 * 
 * Returns:	TRUE/FALSE
 * 
 * Return the current consider nice setting.
 */
gboolean userspace_get_consider_nice(void)
{
	return config.consider_nice;
}
/********************* userspace end *********************/
