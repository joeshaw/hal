/***************************************************************************
 *                                                                         *
 *                            addon-cpufreq.c                              *
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <glib/gprintf.h>

#include "addon-cpufreq.h"
#include "addon-cpufreq-userspace.h"
#include "libhal/libhal.h"
#include "../../logger.h"
#include "../../util_helper_priv.h"

#define MAX_LINE_SIZE				255
#define CPUFREQ_POLKIT_PRIVILEGE		"org.freedesktop.hal.power-management.cpufreq"
#define DBUS_INTERFACE				"org.freedesktop.Hal.Device.CPUFreq"

#define CPUFREQ_ERROR_GENERAL			"GeneralError"
#define CPUFREQ_ERROR_UNKNOWN_GOVERNOR		"UnknownGovernor"
#define CPUFREQ_ERROR_PERMISSION_DENIED		"PermissionDenied"
#define CPUFREQ_ERROR_NO_SUITABLE_GOVERNOR	"NoSuitableGovernor"
#define CPUFREQ_ERROR_GOVERNOR_INIT_FAILED	"GovernorInitFailed"

static const char SYSFS_GOVERNOR_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor";

static const char SYSFS_AVAILABLE_GOVERNORS_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_available_governors";

static const char ONDEMAND_UP_THRESHOLD_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/ondemand/up_threshold";

static const char SYSFS_AFFECTED_CPUS_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/affected_cpus";

static const char SYSFS_CPU_ONLINE_FILE[] =
     "/sys/devices/system/cpu/cpu%u/online";

static const char ONDEMAND_IGNORE_NICE_LOAD_FILE[] =
     "/sys/devices/system/cpu/cpu%u/cpufreq/ondemand/ignore_nice_load";

static gboolean dbus_raise_error(DBusConnection *connection, DBusMessage *message,
				 const char *error_name, char *format, ...);

static gboolean dbus_raise_no_suitable_governor(DBusConnection *connection,
						DBusMessage *message,
						char *method);

static gboolean dbus_raise_governor_init_failed(DBusConnection *connection,
						DBusMessage *message,
						char *governor);

/** list holding all cpufreq objects (userspace, ondemand, etc.) */
static GSList *cpufreq_objs = NULL;

static LibHalContext *halctx = NULL;

static char *udi = NULL;

/******************** helper functions **********************/

/** reads one integer from filename and stores it in val */
static gboolean read_line_int(const char *filename, int *val)
{
	char line[MAX_LINE_SIZE + 1];

	if (!read_line(filename, line, MAX_LINE_SIZE)) {
		HAL_WARNING(("Could not read from %s", filename));
		return FALSE;
	}

	/* strip trailing '\n' */
	line[strlen(line) - 1] = '\0';
	*val = atoi(line);

	return TRUE;
}

/** reads one line from filename with the given length */
gboolean read_line(const char *filename, char *line, unsigned len)
{
	FILE *fp = fopen(filename, "r");
	if (!fp) {
		HAL_WARNING(("Could not open '%s': %s", filename, strerror(errno)));
		return FALSE;
	}
	if ((!fgets(line, len, fp))) {
		HAL_WARNING(("Could not read from '%s': %s", filename, strerror(errno)));
		fclose(fp);
		return FALSE;
	}
	fclose(fp);
	return TRUE;
}

/** writes one line with the given format to filename */
gboolean write_line(const char *filename, const char *fmt, ...)
{
	va_list	ap;
	FILE	*fp;
	int	l;

	fp = fopen(filename, "w+");
	if (!fp) {
		HAL_WARNING(("Could not open file for writing: %s; %s", filename,
			     strerror(errno)));
		return FALSE;
	}

	va_start(ap, fmt);
	l = vfprintf(fp, fmt, ap);
	va_end(ap);

	fclose(fp);

	if (l < 0) {
		HAL_WARNING(("Could not write to file: %s", filename));
		return FALSE;
	}

	return TRUE;
}

/** 
 * read_line_str_split:
 * @filename:	Filename
 * @delim:      The delimiter to split
 *
 * Returns:     The splitted line as array
 * 
 * Reads one line from filename, splits it by delim and returns a two
 * dimension array of strings or NULL on error 
 */
static gchar **read_line_str_split(char *filename, gchar *delim)
{
	gchar	line[MAX_LINE_SIZE];
	int	i;
	gchar	**l;

        if(!read_line(filename, line, MAX_LINE_SIZE)) { 
		printf("returning NULL from str split\n");
		return NULL;
	}

	/* strip trailing '\n' */
	line[strlen(line)-1] = '\0';

	l = g_strsplit(line, delim, MAX_LINE_SIZE);

	if (l[0] == NULL)
		return NULL;

	for (i = 0; l[i] != NULL; i++) {
		if (g_ascii_strcasecmp(l[i], "") == 0) {
			free(l[i]);
			l[i] = NULL;
		}
	} 
	return l;
}

/** 
 * read_line_int_split:
 * @filename:	Filename
 * @delim:      The delimiter to split
 * @list:       List with the splitted integers 
 *
 * Returns:     TRUE/FALSE
 *
 * Reads one line from filename, splits its integers by delim and stores
 * all items in the given list 
 */
gboolean read_line_int_split(char *filename, gchar *delim, GSList **list)
{
	gchar	**l;
	int	i;

	l = read_line_str_split(filename, delim);

        if (l == NULL)
                return FALSE;
	
	for (i = 0; l[i] != NULL; i++) {
		int value = atoi(l[i]);
		*list = g_slist_append(*list, GINT_TO_POINTER(value));
	} 
	g_strfreev(l);
	return TRUE;
}

/** gets a two dimensional list of integers and sorts out duplicates */
static void cpu_list_unique(gpointer data, gpointer whole_list)
{
	GSList	**list		= (GSList**)whole_list;
	GSList	*current	= (GSList*)data;
	GSList	*it		= NULL;

	for (it = *list; it != NULL; it = g_slist_next(it)) {
		gboolean equal = TRUE;
		GSList *list_it = NULL;
		GSList *current_it = NULL;
		
		if (current == it->data)
			continue;

		for (list_it = it->data, current_it = current;
		     list_it != NULL && current_it != NULL;
		     list_it = g_slist_next(list_it), current_it = g_slist_next(current_it)) {

			HAL_DEBUG(("comparing %d with %d", GPOINTER_TO_INT(current_it->data),
				   GPOINTER_TO_INT(list_it->data)));
			if (GPOINTER_TO_INT(current_it->data) != GPOINTER_TO_INT(list_it->data))
				equal = FALSE;
		}

		HAL_DEBUG(("equal? %s, %d", equal ? "yes" : "no", equal));
		if (equal) {
			HAL_DEBUG(("remove: %d", g_slist_length(*list)));
			*list = g_slist_remove(*list, current);
			HAL_DEBUG(("remove_2: %d", g_slist_length(*list)));
			return;
		}
	}
}

/** @brief gets the CPUs and their dependencies */
static gboolean get_cpu_dependencies(GSList **cpu_list, int num_cpus)
{
	int i;

	for (i = 0; i < num_cpus; i++) {
		GSList	*int_cpus		= NULL;
		GSList	*affected_cpus		= NULL;
		GSList	*it			= NULL;
		char	*affected_cpus_file	= NULL;

		affected_cpus_file = g_strdup_printf(SYSFS_AFFECTED_CPUS_FILE, i); 

		if (!read_line_int_split(affected_cpus_file, " ", &affected_cpus)) {
			g_free(affected_cpus_file);
			return FALSE;
		}
		g_free(affected_cpus_file);

		if (affected_cpus == NULL)
			return FALSE;

		for (it = affected_cpus; it != NULL; it = g_slist_next(it)) {
			int_cpus = g_slist_append(int_cpus,
						  GINT_TO_POINTER(affected_cpus->data));
		}
		g_slist_free(affected_cpus);

		if (!g_slist_length(int_cpus)) {
			HAL_WARNING(("failed to get affected_cpus for cpu %d", i));
			continue;
		}

		*cpu_list = g_slist_append(*cpu_list, int_cpus);
	}

	HAL_DEBUG(("Number of CPUs before uniquing cpu_list: %d", g_slist_length(*cpu_list)));
	g_slist_foreach(*cpu_list, (GFunc)cpu_list_unique, cpu_list);
	HAL_DEBUG(("Number of CPUs after uniquing cpu_list: %d", g_slist_length(*cpu_list)));

	if (g_slist_length(*cpu_list) == 0)
		return FALSE;
	return TRUE;
}

/** check if given CPU starting from 0 is online */
gboolean cpu_online(int cpu_id)
{
	gboolean	online;
	char		online_str[2];
	char		*online_file;

	online_file = g_strdup_printf(SYSFS_CPU_ONLINE_FILE, cpu_id); 

	if (access(online_file, F_OK) < 0) {
		online = TRUE;
		goto Out;
	}

	if (!read_line(online_file, online_str, 2)) {
		HAL_WARNING(("Unable to open file: %s", online_file));
		online = FALSE;
		goto Out;
	}
	
	online = atoi(online_str);

	if (!online)
		online = FALSE;
Out:
	g_free(online_file);
	return online;
}

/** writes the new_governor string into the sysfs interface */ 
gboolean write_governor(char *new_governor, int cpu_id)
{
	gboolean	ret		= TRUE;
	char		*governor_file  = NULL;
	char		governor[MAX_LINE_SIZE + 1];

	if (!cpu_online(cpu_id))
		goto Out;

	governor_file = g_strdup_printf(SYSFS_GOVERNOR_FILE, cpu_id); 
	HAL_DEBUG(("Trying to write governor %s", new_governor));

	if (!write_line(governor_file, "%s", new_governor)) {
		ret = FALSE;
		goto Out;
	}
	
	/* check if governor has been set */
	read_line(governor_file, governor, MAX_LINE_SIZE);
	if (strstr(governor, new_governor))
		ret = TRUE;
	else
		ret = FALSE;
Out:
	g_free(governor_file);
	return ret;
}
/******************** helper functions end ********************/

/********************* ondemand interface *********************/
#define ONDEMAND_STRING "ondemand"

struct ondemand_interface {
	int base_cpu;
};

static gboolean ondemand_set_performance(void *data, int performance)
{
	struct ondemand_interface	*iface		   = data;
	char				*up_threshold_file = NULL;

	up_threshold_file = g_strdup_printf(ONDEMAND_UP_THRESHOLD_FILE,
					    iface->base_cpu);

        if(!write_line(up_threshold_file, "%u", performance)){
                HAL_WARNING(("Could not set up_threshold to %u kHz; %s", performance,
			     strerror(errno)));
		g_free(up_threshold_file);
		return FALSE;
        }
	g_free(up_threshold_file);
	HAL_DEBUG(("Up threshold set to %d for ondemand", performance));

	return TRUE;
}

static int ondemand_get_performance(void)
{
	char	*governor_file;
	int	performance	= -1;

	governor_file = g_strdup_printf(ONDEMAND_UP_THRESHOLD_FILE, 0); 

	if (!read_line_int(governor_file, &performance)) {
		HAL_WARNING(("Could not read up_threshold"));
		g_free(governor_file);
		return -1;
	}
	g_free(governor_file);

	return performance;
}

static gboolean ondemand_set_consider_nice(void *data, gboolean consider)
{
	struct ondemand_interface	*iface		= data;
	char				*consider_file;

	consider_file = g_strdup_printf(ONDEMAND_IGNORE_NICE_LOAD_FILE, iface->base_cpu); 

        if(!write_line(consider_file, "%u", !consider)){
                HAL_WARNING(("Could not set ignore_nice_load to: %u; %s", consider,
			     strerror(errno)));
		g_free(consider_file);
                return FALSE;
        }
	g_free(consider_file);
	HAL_DEBUG(("Set consider nice to %d for ondemand", consider));

	return TRUE;
}

static gboolean ondemand_get_consider_nice(void)
{
	char		*governor_file;
	gboolean	consider	= -1;

	/* only read the setting of cpu0 */
	governor_file = g_strdup_printf(ONDEMAND_IGNORE_NICE_LOAD_FILE, 0); 

	if (!read_line_int(governor_file, &consider)) {
		HAL_WARNING(("Could not read ignore_nice_load file"));
		g_free(governor_file);
		return -1;
	}
	g_free(governor_file);

	return !consider;
}

static gboolean ondemand_init(struct ondemand_interface *iface, GSList *cores)
{
	if (iface == NULL)
		return FALSE;

	if (!write_governor(ONDEMAND_STRING, GPOINTER_TO_INT(cores->data))) {
		HAL_WARNING(("Could not set ondemand governor."));
		return FALSE;
	}

	iface->base_cpu = GPOINTER_TO_INT(cores->data);

	return TRUE;
}

static void ondemand_free(void *data)
{
	return;
}

/********************* ondemand end *********************/

/********************* main interface *********************/

/**
 * set_performance: 
 * @connection:		connection to D-Bus
 * @message:		Message
 * @performance:	performance value to set
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises NoSuitableGoveror
 *
 * sets the performance for all cpufreq objects
 */
static gboolean set_performance(DBusConnection *connection, DBusMessage *message,
				int performance)
{
	float	steps;
	float	up_threshold;
	GSList	*it		= NULL;

	if (cpufreq_objs == NULL) {
		dbus_raise_no_suitable_governor(connection, message,
						"SetCPUFreqPerformance");
		return FALSE;
	}

	if (performance < 1)
		performance = 1;
	if (performance > 100)
		performance = 100;

	if (performance >= 50) {
		steps = UP_THRESHOLD_BASE - UP_THRESHOLD_MIN + 1;
		up_threshold = (UP_THRESHOLD_BASE) - (((float)performance - 50.0) *
						      (steps / 51.0)); 
		performance = (int)up_threshold;
	} else if (performance < 50) {
		steps = UP_THRESHOLD_MAX - UP_THRESHOLD_BASE;
		up_threshold = (UP_THRESHOLD_MAX + 1) - ((float)performance *
							 (steps / 49.0)); 
		performance = (int)up_threshold;
	}

	for (it = cpufreq_objs; it != NULL; it = g_slist_next(it)) {
		struct cpufreq_obj *obj = it->data; 
		obj->set_performance(obj->iface, performance);
	}
	return TRUE;
}

/** 
 * get_performance:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @performance:	pointer to return the current performance value
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises (NoSuitableGoveror|GeneralError)
 * 
 * sets the performance for all cpufreq objects
 */
static gboolean get_performance(DBusConnection *connection, DBusMessage *message,
				int *performance)
{
	struct cpufreq_obj *obj;
	float		   steps;
	float		   perf;
	int		   up_threshold;

	if (cpufreq_objs == NULL) {
		dbus_raise_no_suitable_governor(connection, message,
						"GetCPUFreqPerformance");
		return FALSE;
	}

	obj = cpufreq_objs->data;

	up_threshold = obj->get_performance();
	if (up_threshold < 0) {
		dbus_raise_error(connection, message, CPUFREQ_ERROR_GENERAL,
				 "Could not read up_threshold");
		return FALSE;
	}

	if (up_threshold < UP_THRESHOLD_BASE) {
		steps = UP_THRESHOLD_BASE - UP_THRESHOLD_MIN + 1;
		perf = (((UP_THRESHOLD_BASE) - up_threshold) /
			       (steps / 51.0)) + 50.0;
	} else if (up_threshold >= UP_THRESHOLD_BASE) {
		steps = UP_THRESHOLD_MAX - UP_THRESHOLD_BASE;
		perf = ((UP_THRESHOLD_MAX + 1) - up_threshold) /
			(steps / 49.0);
	}

	*performance = (int)perf;

	return TRUE;
}

/** 
 * set_consider_nice:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @consider:		TRUE if should set consider nice
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises NoSuitableGoveror
 * 
 * sets consider nice to all cpufreq opjects. 
 */
static gboolean set_consider_nice(DBusConnection *connection, DBusMessage *message,
				  gboolean consider)
{
	GSList *it = NULL;

	if (cpufreq_objs == NULL) {
		dbus_raise_no_suitable_governor(connection, message,
						"SetCPUFreqConsiderNice");
		return FALSE;
	}

	for (it = cpufreq_objs; it != NULL; it = g_slist_next(it)) {
		struct cpufreq_obj *obj= it->data; 
		obj->set_consider_nice(obj->iface, consider);
	}
	return TRUE;
}

/** 
 * get_consider_nice:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @consider:		pointer to return the current state of consider nice
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises NoSuitableGoveror
 * 
 * Gets consider nice state. 
 */
static gboolean get_consider_nice(DBusConnection *connection, DBusMessage *message,
				  int *consider)
{
	struct cpufreq_obj *obj;

	if (cpufreq_objs == NULL) {
		dbus_raise_no_suitable_governor(connection, message,
						"GetCPUFreqConsiderNice");
		return FALSE;
	}
	obj = cpufreq_objs->data;

	*consider = obj->get_consider_nice();

	return TRUE;
}

/** 
 * get_available_governors:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @governors:		to return the current available governors.
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises GeneralError
 * 
 * Gets the names of all availabe governors. 
 */
static gboolean get_available_governors(DBusConnection *connection, DBusMessage *message,
					gchar ***governors)
{
	char *agovs_file;

	agovs_file = g_strdup_printf(SYSFS_AVAILABLE_GOVERNORS_FILE, 0); 
	*governors = read_line_str_split(agovs_file, " ");
	g_free(agovs_file);

	if (*governors == NULL) {
		dbus_raise_error(connection, message, CPUFREQ_ERROR_GENERAL,
				 "No CPUFreq governors");
		return FALSE;
	}		

	return TRUE;
}

/** 
 * set_governors:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @governor:		name of the governor to set
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises (GeneralError|UnknownGovernor|GovernorInitFailed) 
 * 
 * sets a governor for all cpufreq objects. 
 */
static gboolean set_governors(DBusConnection *connection, DBusMessage *message,
				       const char *governor)
{
	GSList		*cpus			= NULL;
	GSList		*it			= NULL;
	static int	g_source_id		= -1;
	gboolean	have_governor		= FALSE;
	int		i;
	int		num_cpus;
	gchar		**available_governors;

	if (!get_available_governors(connection, message, &available_governors))
		return FALSE;

	for (i = 0; available_governors[i] != NULL; i++) {
		if (strcmp(available_governors[i], governor) == 0) {
			have_governor = TRUE;
			break;
		}		
	}
	g_strfreev(available_governors);

	if (!have_governor) {
		dbus_raise_error(connection, message,
				 CPUFREQ_ERROR_UNKNOWN_GOVERNOR,
				 "No governor '%s' available", governor);
		return FALSE;
	}

	/** clear all previous cpufreq_objs */
	if (g_slist_length(cpufreq_objs) > 0) {
		GSList *iter = NULL;
 		for (iter = cpufreq_objs; iter != NULL; iter = g_slist_next(iter)) {
			struct cpufreq_obj *obj = iter->data; 
			obj->free(obj->iface);
			free(obj->iface);
			free(obj);
		}
		g_slist_free(cpufreq_objs);
		cpufreq_objs = NULL;
		g_source_remove(g_source_id);
		g_source_id = -1;
	}

	num_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (num_cpus < 0) {
		dbus_raise_error(connection, message, CPUFREQ_ERROR_GENERAL,
				 "No CPUs found in system");
		HAL_WARNING(("No CPUs found in system"));
		return FALSE;
	}

	if (!get_cpu_dependencies(&cpus, num_cpus)) {
		dbus_raise_error(connection, message, CPUFREQ_ERROR_GENERAL,
				 "Could not figure out cpu core dependencies");
		HAL_WARNING(("Could not figure out cpu core dependencies"));
		return FALSE;
	}
	
	if (!strcmp(governor, USERSPACE_STRING)) {
		struct cpufreq_obj *cpufreq_obj;
		struct userspace_interface *iface;

		for (it = cpus; it != NULL; it = g_slist_next(it)) {
			cpufreq_obj = malloc(sizeof(struct cpufreq_obj));
			iface = malloc(sizeof(struct userspace_interface));

			if (userspace_init(iface, it->data)) {
				cpufreq_obj->iface = iface;
				cpufreq_obj->set_performance   = userspace_set_performance;
				cpufreq_obj->get_performance   = userspace_get_performance;
				cpufreq_obj->set_consider_nice = userspace_set_consider_nice;
				cpufreq_obj->get_consider_nice = userspace_get_consider_nice;
				cpufreq_obj->free = userspace_free;
				cpufreq_objs = g_slist_append(cpufreq_objs, cpufreq_obj);
				HAL_DEBUG(("added userspace interface"));
			} else {
				dbus_raise_governor_init_failed(connection, message,
								(char*)governor);
				return FALSE;
			}
		}
		g_source_id = g_timeout_add(USERSPACE_POLL_INTERVAL,
					    (GSourceFunc)userspace_adjust_speeds,
					    cpufreq_objs);

	} else if (!strcmp(governor, ONDEMAND_STRING)) {
		struct cpufreq_obj *cpufreq_obj;
		struct ondemand_interface *iface;

		for (it = cpus; it != NULL; it = g_slist_next(it)) {
			cpufreq_obj = malloc(sizeof(struct cpufreq_obj));
			iface = malloc(sizeof(struct ondemand_interface));

			if (ondemand_init(iface, it->data)) {
				cpufreq_obj->iface = iface;
				cpufreq_obj->set_performance   = ondemand_set_performance;
				cpufreq_obj->get_performance   = ondemand_get_performance;
				cpufreq_obj->set_consider_nice = ondemand_set_consider_nice;
				cpufreq_obj->get_consider_nice = ondemand_get_consider_nice;
				cpufreq_obj->free = ondemand_free;
				cpufreq_objs = g_slist_append(cpufreq_objs, cpufreq_obj);
				HAL_DEBUG(("added ondemand interface"));
			} else {
				dbus_raise_governor_init_failed(connection, message,
								(char*)governor);
				return FALSE;
			}
		}
	} else {
		for (it = cpus; it != NULL; it = g_slist_next(it)) {
			if (!write_governor((char*)governor,
					    GPOINTER_TO_INT(((GSList*)it->data)->data))) {
				dbus_raise_governor_init_failed(connection, message,
								(char*)governor);
				HAL_WARNING(("Could not set %s governor.", governor));
				return FALSE;
			}
		}
	}
	
	set_performance(NULL, NULL, DEFAULT_PERFORMANCE);

	return TRUE;
}

/** 
 * get_governors:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @governor:		pointer to the name of the current set governor
 *
 * Returns: 		TRUE/FALSE
 *
 * @raises GeneralError 
 * 
 * gets the current governor which is set for all cpufreq objects
 */
static gboolean get_governors(DBusConnection *connection, DBusMessage *message,
			   char *governor)
{
	char	*governor_file;
	int	cpu_id		= 0;

	governor_file = g_strdup_printf(SYSFS_GOVERNOR_FILE, cpu_id); 

	if (!read_line(governor_file, governor, MAX_LINE_SIZE)) {
		dbus_raise_error(connection, message, CPUFREQ_ERROR_GENERAL,
				 "Could not read current governor");
		g_free(governor_file);
		return FALSE;
	}
	g_free(governor_file);

	/* strip trailing '\n' */
	governor[strlen(governor)-1] = '\0';

	return TRUE;
}
/********************* main interface end *********************/

/********************* DBus stuff *********************/

/** 
 * dbus_raise_no_suitable_governor:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @method:		name of the method
 *
 * Returns: 		TRUE/FALSE
 *
 * Raises the NoSuitableGovernor error with the given method in the
 * detail field
 */
static gboolean dbus_raise_no_suitable_governor(DBusConnection *connection,
						DBusMessage *message,
						char *method)
{
	return dbus_raise_error(connection, message,
				CPUFREQ_ERROR_NO_SUITABLE_GOVERNOR,
				"No '%s' setting for current governor",
				method);
}

/** 
 * dbus_raise_governor_init_failed:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @governor:		name of the governor
 *
 * Returns: 		TRUE/FALSE
 *
 * Raises the GovernorInitFailed error with the given governor in the
 * detail field
 */
static gboolean dbus_raise_governor_init_failed(DBusConnection *connection,
						DBusMessage *message,
						char *governor)
{
	return dbus_raise_error(connection, message,
				CPUFREQ_ERROR_GOVERNOR_INIT_FAILED,
				"Initialization of %s interface failed",
				governor);
}

/** 
 * dbus_raise_error:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @error_name:		name of the error
 * @format:             to format the error message
 * @...:		more args
 *
 * Returns: 		TRUE/FALSE
 *
 * Raises the given error_name with the format in the detail field
 */
static gboolean dbus_raise_error(DBusConnection *connection, DBusMessage *message,
				 const char *error_name, char *format, ...)
{
	char		buf[2 * MAX_LINE_SIZE];
	DBusMessage	*reply;
	va_list		args;
	char		*error = NULL;

	if (connection == NULL || message == NULL)
		return FALSE;

	va_start(args, format);
	vsnprintf(buf, sizeof buf, format, args);
	va_end(args);

	error = g_strdup_printf("%s.%s", DBUS_INTERFACE, error_name);
	reply = dbus_message_new_error(message, error, buf);
	g_free(error);
	if (reply == NULL) {
		HAL_WARNING(("No memory"));
		return FALSE;
	}

	if (!dbus_connection_send(connection, reply, NULL)) {
		HAL_WARNING(("No memory"));
		dbus_message_unref(reply);
		return FALSE;
	}
	dbus_message_unref(reply);

	return TRUE;
}


/** 
 * dbus_send_reply:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @type:               the type of data param
 * @data:		data to send
 *
 * Returns: 		TRUE/FALSE
 *
 * sends a reply to message with the given data and its dbus_type 
 */
static gboolean dbus_send_reply(DBusConnection *connection, DBusMessage *message,
				int dbus_type, void *data)
{
	DBusMessage *reply;

	if ((reply = dbus_message_new_method_return(message)) == NULL) {
		HAL_WARNING(("Could not allocate memory for the DBus reply"));
		return FALSE;
	}

	if (data != NULL)
		dbus_message_append_args(reply, dbus_type, data, DBUS_TYPE_INVALID);

	if (!dbus_connection_send(connection, reply, NULL)) {
		HAL_WARNING(("Could not sent reply"));
		return FALSE;
	}
	dbus_connection_flush(connection);
	dbus_message_unref(reply);
	
	return TRUE;
}

/** 
 * dbus_send_reply:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @list:               list to send
 *
 * Returns: 		TRUE/FALSE
 *
 * sends a reply to message appending a list of strings 
 */
static gboolean dbus_send_reply_strlist(DBusConnection *connection, DBusMessage *message,
					gchar **list)
{
	DBusMessage	*reply;
	DBusMessageIter	iter;
	DBusMessageIter	iter_array;
	int		i;

	if ((reply = dbus_message_new_method_return(message)) == NULL) {
		HAL_WARNING(("Could not allocate memory for the DBus reply"));
		return FALSE;
	}

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, 
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_STRING_AS_STRING,
					 &iter_array);
	
	for (i = 0; list[i] != NULL; i++)
		dbus_message_iter_append_basic (&iter_array, DBUS_TYPE_STRING, &list[i]);

	dbus_message_iter_close_container (&iter, &iter_array);

	if (!dbus_connection_send(connection, reply, NULL)) {
		HAL_WARNING(("Could not sent reply"));
		return FALSE;
	}

	dbus_connection_flush(connection);
	dbus_message_unref(reply);

	return TRUE;
}

/** 
 * dbus_get_argument:
 * @connection:		connection to D-Bus
 * @message:		Message
 * @dbus_error:         the D-Bus error
 * @type:               the type of arg param
 * @arg:		the value to get from the message
 *
 * Returns: 		TRUE/FALSE
 *
 * gets one argument from message with the given dbus_type and stores it in arg
 */
static gboolean dbus_get_argument(DBusConnection *connection, DBusMessage *message,
				  DBusError *dbus_error, int dbus_type, void *arg)
{
	dbus_message_get_args(message, dbus_error, dbus_type, arg,
			      DBUS_TYPE_INVALID);
	if (dbus_error_is_set(dbus_error)) {
		HAL_WARNING(("Could not get argument of DBus message: %s",
			     dbus_error->message));
		dbus_error_free(dbus_error);
		return FALSE;
	}
	return TRUE;
}

static DBusHandlerResult dbus_filter_function_local(DBusConnection *connection,
						    DBusMessage *message,
						    void *user_data)
{
	if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL,
				   "Disconnected")) {
		HAL_DEBUG(("DBus daemon disconnected. Trying to reconnect..."));
		dbus_connection_unref(connection);
		g_timeout_add(5000, (GSourceFunc)dbus_init_local, NULL);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

/** 
 * dbus_filter_function:
 * @connection:		connection to D-Bus
 * @message:		message
 * @user_data:          pointer to the data
 *
 * Returns: 		the result
 * 
 * @raises UnknownMethod
 *
 * D-Bus filter function
 */
static DBusHandlerResult dbus_filter_function(DBusConnection *connection,
					      DBusMessage *message,
					      void *user_data)
{
	DBusError	dbus_error;
	const char	*member		= dbus_message_get_member(message);
	const char	*path		= dbus_message_get_path(message);

	HAL_DEBUG(("Received DBus message with member %s", member));
	HAL_DEBUG(("Received DBus message with path %s", path));

	dbus_error_init(&dbus_error);

	/* since we wait only for method calls with interface == DBUS_INTERFACE check 
	   it and return if there are calls with other interfaces */
	if (!dbus_message_has_interface(message, DBUS_INTERFACE)) 
		return DBUS_HANDLER_RESULT_HANDLED;

#ifdef HAVE_POLKIT
	if (!check_priv (halctx, connection, message, dbus_message_get_path (message), CPUFREQ_POLKIT_PRIVILEGE))
		return DBUS_HANDLER_RESULT_HANDLED;
#endif

	if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					"SetCPUFreqGovernor")) {
		char *arg;

		if (!dbus_get_argument(connection, message, &dbus_error,
				       DBUS_TYPE_STRING, &arg)) {
			LIBHAL_FREE_DBUS_ERROR (&dbus_error);
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
 		HAL_DEBUG(("Received argument: %s", arg));
			
		if (set_governors(connection, message, arg))
			dbus_send_reply(connection, message, DBUS_TYPE_INVALID, NULL);

	} else if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					       "SetCPUFreqPerformance")) {
		int arg;

		if (!dbus_get_argument(connection, message, &dbus_error,
				       DBUS_TYPE_INT32, &arg)) {
			LIBHAL_FREE_DBUS_ERROR (&dbus_error);
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
 		HAL_DEBUG(("Received argument: %d", arg));

		if (set_performance(connection, message, arg))
			dbus_send_reply(connection, message, DBUS_TYPE_INVALID, NULL);

	} else if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					       "SetCPUFreqConsiderNice")) {
		gboolean arg;

		if (!dbus_get_argument(connection, message, &dbus_error,
				       DBUS_TYPE_BOOLEAN, &arg)) {
			LIBHAL_FREE_DBUS_ERROR (&dbus_error);
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
 		HAL_DEBUG(("Received argument: %d", arg));

		if (set_consider_nice(connection, message, arg))
			dbus_send_reply(connection, message, DBUS_TYPE_INVALID, NULL);

	} else if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					       "GetCPUFreqGovernor")) {
		char governor[MAX_LINE_SIZE + 1];
		char *gov				= governor;

		if (get_governors(connection, message, governor))
			dbus_send_reply(connection, message, DBUS_TYPE_STRING, &gov);

	} else if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					       "GetCPUFreqPerformance")) {
		int performance	= -1;

		if (get_performance(connection, message, &performance))
			dbus_send_reply(connection, message, DBUS_TYPE_INT32, &performance);

	} else if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					       "GetCPUFreqConsiderNice")) {
		int consider = -1;

		if (get_consider_nice(connection, message, &consider))
			dbus_send_reply(connection, message, DBUS_TYPE_BOOLEAN, &consider);

	} else if (dbus_message_is_method_call(message, DBUS_INTERFACE,
					       "GetCPUFreqAvailableGovernors")) {
		gchar **governors = NULL;

		if (get_available_governors(connection, message, &governors))
			dbus_send_reply_strlist(connection, message, governors);
		g_strfreev(governors);

	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static gboolean is_supported(void)
{
	char *governor_file = NULL;

	governor_file = g_strdup_printf(SYSFS_GOVERNOR_FILE, 0); 
	if (access(governor_file, F_OK) != 0) {
		g_free(governor_file);
		return FALSE;
	}
	g_free(governor_file);
	return TRUE;
}

/* returns FALSE on success because it's used as a callback */
gboolean dbus_init_local(void)
{
	DBusConnection	*dbus_connection;
	DBusError	dbus_error;

	dbus_error_init(&dbus_error);

	dbus_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_error);
	if (dbus_error_is_set(&dbus_error)) {
		dbus_error_free (&dbus_error);
		HAL_WARNING(("Cannot get D-Bus connection"));
		return TRUE;
	}

	dbus_connection_setup_with_g_main(dbus_connection, NULL);
	dbus_connection_add_filter(dbus_connection, dbus_filter_function_local,
				   NULL, NULL);
	dbus_connection_set_exit_on_disconnect(dbus_connection, 0);
	return FALSE;
}

gboolean dbus_init(void)
{
	DBusError	dbus_error;
	DBusConnection	*dbus_connection;

        udi = getenv("UDI");

	dbus_error_init(&dbus_error);

	if ((halctx = libhal_ctx_init_direct(&dbus_error)) == NULL) {
		HAL_WARNING(("Cannot connect to hald"));
		goto Error;
	}

	if ((dbus_connection = libhal_ctx_get_dbus_connection(halctx)) == NULL) {
		HAL_WARNING(("Cannot get DBus connection"));
		goto Error;
	}

	if (!libhal_device_addon_is_ready (halctx, udi, &dbus_error)) {
		goto Error;
	}

	if (!libhal_device_claim_interface(halctx, udi,
		"org.freedesktop.Hal.Device.CPUFreq", 
		"    <method name=\"SetCPUFreqGovernor\">\n"
		"      <arg name=\"governor_string\" direction=\"in\" type=\"s\"/>\n"
		"    </method>\n"
		"    <method name=\"SetCPUFreqPerformance\">\n"
		"      <arg name=\"value\" direction=\"in\" type=\"i\"/>\n"
		"    </method>\n"
		"    <method name=\"SetCPUFreqConsiderNice\">\n"
		"      <arg name=\"value\" direction=\"in\" type=\"b\"/>\n"
		"    </method>\n"
		"    <method name=\"GetCPUFreqGovernor\">\n"
		"      <arg name=\"return_code\" direction=\"out\" type=\"s\"/>\n"
		"    </method>\n"
		"    <method name=\"GetCPUFreqPerformance\">\n"
		"      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
		"    </method>\n"
		"    <method name=\"GetCPUFreqConsiderNice\">\n"
		"      <arg name=\"return_code\" direction=\"out\" type=\"b\"/>\n"
		"    </method>\n"
		"    <method name=\"GetCPUFreqAvailableGovernors\">\n"
		"      <arg name=\"return_code\" direction=\"out\" type=\"as\"/>\n"
		"    </method>\n",
		&dbus_error)) {

		HAL_WARNING(("Cannot claim interface: %s", dbus_error.message));
		fprintf(stderr, "direct Cannot claim interface: %s", dbus_error.message);
		goto Error;
	}

	libhal_device_add_capability(halctx,
				     "/org/freedesktop/Hal/devices/computer",
				     "cpufreq_control",
				     &dbus_error);

	if (dbus_error_is_set(&dbus_error)) {
		HAL_WARNING(("Cannot add capability cpufreq_control: %s", dbus_error.message));
		goto Error;
	}
	
	dbus_connection_setup_with_g_main(dbus_connection, NULL);
	dbus_connection_add_filter(dbus_connection, dbus_filter_function, NULL, NULL);
	dbus_connection_set_exit_on_disconnect(dbus_connection, 0);
	return TRUE;

Error:
        LIBHAL_FREE_DBUS_ERROR (&dbus_error);

	if (halctx != NULL) {
                libhal_ctx_shutdown (halctx, &dbus_error);
                LIBHAL_FREE_DBUS_ERROR (&dbus_error);
                libhal_ctx_free (halctx);
        }

	return FALSE;
}
/********************* DBus end *********************/

static void exit_handler(int i)
{
	GSList *it = NULL;

	for (it = cpufreq_objs; it != NULL; it = g_slist_next(it)) {
		struct cpufreq_obj *obj = it->data; 
		obj->free(obj->iface);
		free(obj->iface);
		free(obj);
	}
	g_slist_free(cpufreq_objs);

	HAL_DEBUG(("exit"));
	exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	struct sigaction	signal_action;
	GMainLoop		*gmain;

	memset(&signal_action, 0, sizeof(signal_action));
	sigaddset(&signal_action.sa_mask, SIGTERM);
	signal_action.sa_flags = SA_RESTART || SA_NOCLDSTOP;
	signal_action.sa_handler = exit_handler;
	sigaction(SIGINT, &signal_action, NULL);
	sigaction(SIGQUIT, &signal_action, NULL);
	sigaction(SIGTERM, &signal_action, NULL);

	setup_logger ();

	if (!is_supported()) {
		HAL_WARNING(("CPUFreq not supported. Exiting..."));
		exit(EXIT_FAILURE);
	}

	if (!dbus_init() || dbus_init_local())
		exit(EXIT_FAILURE);

	gmain = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(gmain);

	return 0;
}
