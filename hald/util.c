/***************************************************************************
 * CVSID: $Id$
 *
 * util.c - Various utilities
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
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
#include <stdarg.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "osspec.h"
#include "logger.h"
#include "hald.h"
#include "device_info.h"

#include "util.h"

gboolean 
hal_util_remove_trailing_slash (gchar *path)
{
	gchar *c = NULL;

	if (path == NULL) {
		return FALSE;
	}

	c = strrchr (path, '/');
	if (c == NULL) {
		HAL_WARNING (("Invalid path %s", path));
		return 1;
	}
	if (*(c+1) == '\0') 
		*c = '\0';

	return TRUE;
}

gboolean 
hal_util_get_fs_mnt_path (const gchar *fs_type, gchar *mnt_path, gsize len)
{
	FILE *mnt;
	struct mntent *mntent;
	gboolean rc;
	gsize dirlen;

	rc = FALSE;
	dirlen = 0;

	if (fs_type == NULL || mnt_path == NULL || len == 0) {
		HAL_ERROR (("Arguments not sane"));
		return -1;
	}

	if ((mnt = setmntent ("/proc/mounts", "r")) == NULL) {
		HAL_ERROR (("Error getting mount information"));
		return -1;
	}

	while (rc == FALSE && dirlen == 0 && (mntent = getmntent(mnt)) != NULL) {
		if (strcmp (mntent->mnt_type, fs_type) == 0) {
			dirlen = strlen (mntent->mnt_dir);
			if (dirlen <= (len - 1)) {
				g_strlcpy (mnt_path, mntent->mnt_dir, len);
				rc = TRUE;
			} else {
				HAL_ERROR (("Error - mount path too long"));
				rc = FALSE;
			}
		}
	}
	endmntent (mnt);
	
	if (dirlen == 0 && rc == TRUE) {
		HAL_ERROR (("Filesystem %s not found", fs_type));
		rc = FALSE;
	}

	if ((!hal_util_remove_trailing_slash (mnt_path)))
		rc = FALSE;
	
	return rc;
}

/** Given a path, /foo/bar/bat/foobar, return the last element, e.g.
 *  foobar.
 *
 *  @param  path                Path
 *  @return                     Pointer into given string
 */
const gchar *
hal_util_get_last_element (const gchar *s)
{
	int len;
	const gchar *p;

	len = strlen (s);
	for (p = s + len - 1; p > s; --p) {
		if ((*p) == '/')
			return p + 1;
	}

	return s;
}

/** Given a path, this functions finds the path representing the
 *  parent directory by truncation.
 *
 *  @param  path                Path
 *  @return                     Path for parent or NULL. Must be freed by caller
 */
gchar *
hal_util_get_parent_path (const gchar *path)
{
	guint i;
	guint len;
	gchar *parent_path;

	/* Find parent device by truncating our own path */
	parent_path = g_strndup (path, HAL_PATH_MAX);
	len = strlen (parent_path);
	for (i = len - 1; parent_path[i] != '/'; --i) {
		parent_path[i] = '\0';
	}
	parent_path[i] = '\0';

	return parent_path;
}

gchar *
hal_util_get_normalized_path (const gchar *path1, const gchar *path2)
{
	int i;
	int len1;
	int len2;
	const gchar *p1;
	const gchar *p2;
	gchar buf[HAL_PATH_MAX];

	len1 = strlen (path1);
	len2 = strlen (path1);

	p1 = path1 + len1;

	i = 0;
	p2 = path2;
	while (p2 < path2 + len2 && strncmp (p2, "../", 3) == 0) {
		p2 += 3;

		while (p1 >= path1 && *(--p1)!='/')
			;

	}

	strncpy (buf, path1, (p1-path1));
	buf[p1-path1] = '\0';

	return g_strdup_printf ("%s/%s", buf, p2);
}

gboolean
hal_util_get_int_from_file (const gchar *directory, const gchar *file, gint *result, gint base)
{
	FILE *f;
	char buf[64];
	gchar path[HAL_PATH_MAX];
	gboolean ret;

	f = NULL;
	ret = FALSE;

	g_snprintf (path, sizeof (path), "%s/%s", directory, file);

	f = fopen (path, "rb");
	if (f == NULL) {
		HAL_ERROR (("Cannot open '%s'", path));
		goto out;
	}

	if (fgets (buf, sizeof (buf), f) == NULL) {
		HAL_ERROR (("Cannot read from '%s'", path));
		goto out;
	}

	/* TODO: handle error condition */
	*result = strtol (buf, NULL, base);
	ret = TRUE;

out:
	if (f != NULL)
		fclose (f);

	return ret;
}

gboolean
hal_util_set_int_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file, gint base)
{
	gint value;
	gboolean ret;

	ret = FALSE;

	if (hal_util_get_int_from_file (directory, file, &value, base))
		ret = hal_device_property_set_int (d, key, value);

	return ret;
}

gboolean
hal_util_get_bcd2_from_file (const gchar *directory, const gchar *file, gint *result)
{
	FILE *f;
	char buf[64];
	gchar path[HAL_PATH_MAX];
	gboolean ret;
	gint digit;
	gint left, right;
	gboolean passed_white_space;
	gint num_prec;
	gsize len;
	gchar c;
	guint i;

	f = NULL;
	ret = FALSE;

	g_snprintf (path, sizeof (path), "%s/%s", directory, file);

	f = fopen (path, "rb");
	if (f == NULL) {
		HAL_ERROR (("Cannot open '%s'", path));
		goto out;
	}

	if (fgets (buf, sizeof (buf), f) == NULL) {
		HAL_ERROR (("Cannot read from '%s'", path));
		goto out;
	}

	left = 0;
	len = strlen (buf);
	passed_white_space = FALSE;
	for (i = 0; i < len && buf[i] != '.'; i++) {
		if (g_ascii_isspace (buf[i])) {
			if (passed_white_space)
				break;
			else
				continue;
		}
		passed_white_space = TRUE;
		left *= 16;
		c = buf[i];
		digit = (int) (c - '0');
		left += digit;
	}
	i++;
	right = 0;
	num_prec = 0;
	for (; i < len; i++) {
		if (g_ascii_isspace (buf[i]))
			break;
		if (num_prec == 2)	        /* Only care about two digits 
						 * of precision */
			break;
		right *= 16;
		c = buf[i];
		digit = (int) (c - '0');
		right += digit;
		num_prec++;
	}

	for (; num_prec < 2; num_prec++)
		right *= 16;

	*result = left * 256 + (right & 255);
	ret = TRUE;

out:
	if (f != NULL)
		fclose (f);

	return ret;
}

gboolean
hal_util_set_bcd2_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file)
{
	gint value;
	gboolean ret;

	ret = FALSE;

	if (hal_util_get_bcd2_from_file (directory, file, &value))
		ret = hal_device_property_set_int (d, key, value);

	return ret;
}

gchar *
hal_util_get_string_from_file (const gchar *directory, const gchar *file)
{
	FILE *f;
	static gchar buf[256];
	gchar path[HAL_PATH_MAX];
	gchar *result;
	gsize len;
	guint i;
	
	f = NULL;
	result = NULL;

	g_snprintf (path, sizeof (path), "%s/%s", directory, file);

	f = fopen (path, "rb");
	if (f == NULL) {
		HAL_ERROR (("Cannot open '%s'", path));
		goto out;
	}

	if (fgets (buf, sizeof (buf), f) == NULL) {
		HAL_ERROR (("Cannot read from '%s'", path));
		goto out;
	}
       
	len = strlen (buf);
	if (len>0)
		buf[len-1] = '\0';

	/* Clear remaining whitespace */
	for (i = len-2; i >= 0; --i) {
		if (!g_ascii_isspace (buf[i]))
			break;
		buf[i] = '\0';
	}

	result = buf;

out:
	if (f != NULL)
		fclose (f);

	return result;
}

gboolean
hal_util_set_string_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file)
{
	gchar *buf;
	gboolean ret;

	ret = FALSE;

	if ((buf = hal_util_get_string_from_file (directory, file)) != NULL)
		ret = hal_device_property_set_string (d, key, buf);

	return ret;
}

void
hal_util_compute_udi (HalDeviceStore *store, gchar *dst, gsize dstsize, const gchar *format, ...)
{
	guint i;
	va_list args;
	gchar buf[256];

	va_start (args, format);
	g_vsnprintf (buf, sizeof (buf), format, args);
	va_end (args);

	g_strcanon (buf, 
		    "/_"
		    "abcdefghijklmnopqrstuvwxyz"
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    "1234567890", '_');

	g_strlcpy (dst, buf, dstsize);
	if (hal_device_store_find (store, dst) == NULL)
		goto out;

	for (i = 0; ; i++) {
		g_snprintf (dst, dstsize, "%s_%d", buf, i);
		if (hal_device_store_find (store, dst) == NULL)
			goto out;
	}

out:
	;
}

void
hal_util_terminate_helper (HalHelperData *ed)
{
	HAL_INFO (("killing %d for udi %s", ed->pid, ed->d->udi));

	/* kill kenny! kill it!! */
	kill (ed->pid, SIGTERM);
	/* TODO: yikes; what about removing the zombie? */

	if (ed->timeout_watch_id != (guint) -1) {
		g_source_remove (ed->timeout_watch_id);
		ed->timeout_watch_id = -1;
	}

	ed->already_issued_callback = TRUE;

	ed->cb (ed->d, TRUE, -1, ed->data1, ed->data2, ed);

	/* ed will be cleaned up when helper_child_exited reaps the child */
	return;
}

static gboolean
helper_child_timeout (gpointer data)
{
	HalHelperData *ed = (HalHelperData *) data;

	HAL_INFO (("child timeout for pid %d", ed->pid));

	/* kill kenny! kill it!! */
	kill (ed->pid, SIGTERM);

	ed->timeout_watch_id = -1;
	ed->already_issued_callback = TRUE;

	ed->cb (ed->d, TRUE, -1, ed->data1, ed->data2, ed);

	/* ed will be cleaned up when helper_child_exited reaps the child */
	return FALSE;
}

static GSList *running_helpers = NULL;

static void 
helper_child_exited (GPid pid, gint status, gpointer data)
{
	HalHelperData *ed = (HalHelperData *) data;

	HAL_INFO (("child exited for pid %d", pid));

	if (ed->timeout_watch_id != (guint) -1)
		g_source_remove (ed->timeout_watch_id);
	g_spawn_close_pid (ed->pid);

	if (!ed->already_issued_callback)
		ed->cb (ed->d, FALSE, WEXITSTATUS (status), ed->data1, ed->data2, ed);

	running_helpers = g_slist_remove (running_helpers, ed);		
	g_free (ed);
}

static gboolean
helper_add_property_to_env (HalDevice *device, HalProperty *property, gpointer user_data)
{
	char *prop_upper, *value;
	char *c;
	gchar ***ienvp = (gchar ***) user_data;
	gchar **envp;

	envp = *ienvp;
	*ienvp = *ienvp + 1;

	prop_upper = g_ascii_strup (hal_property_get_key (property), -1);
	
	/* periods aren't valid in the environment, so replace them with
	 * underscores. */
	for (c = prop_upper; *c; c++) {
		if (*c == '.')
			*c = '_';
	}
	
	value = hal_property_to_string (property);
	
	*envp = g_strdup_printf ("HAL_PROP_%s=%s", prop_upper, value);

	g_free (value);
	g_free (prop_upper);

	return TRUE;
}


HalHelperData *
hal_util_helper_invoke (const gchar *command_line, gchar **extra_env, HalDevice *d, 
			gpointer data1, gpointer data2, HalHelperTerminatedCB cb, guint timeout)
{
	HalHelperData *ed;
	gint argc;
	gchar **argv;
	gchar **envp;
	gchar **ienvp;
	GError *err = NULL;
	guint num_env_vars;
	guint i, j;
	guint num_properties;
	guint num_extras;

	ed = g_new0 (HalHelperData, 1);
	ed->data1 = data1;
	ed->data2 = data2;
	ed->d = d;
	ed->cb = cb;

	num_properties = hal_device_num_properties (d);
	if (extra_env == NULL)
		num_extras = 0;
	else
		num_extras = g_strv_length ((gchar **) extra_env);
	num_env_vars = num_properties + 2 + num_extras;
	if (hald_is_verbose)
		num_env_vars++;
	if (hald_is_initialising)
		num_env_vars++;

	envp = g_new (char *, num_env_vars);
	ienvp = envp;
	hal_device_property_foreach (d, helper_add_property_to_env, &ienvp);
	i = num_properties;
	envp[i++] = g_strdup_printf ("UDI=%s", hal_device_get_udi (d));
	if (hald_is_verbose)
		envp[i++] = g_strdup ("HALD_VERBOSE=1");
	if (hald_is_initialising)
		envp[i++] = g_strdup ("HALD_STARTUP=1");
	for (j = 0; j < num_extras; j++) {
		envp[i++] = g_strdup (extra_env[j]);
	}
	envp[i++] = NULL;

	err = NULL;
	if (!g_shell_parse_argv (command_line, &argc, &argv, &err)) {
		HAL_ERROR (("Error parsing commandline '%s': %s", command_line, err->message));
		g_error_free (err);
		g_free (ed);
		ed = NULL;
	} else {
		err = NULL;
		if (!g_spawn_async (NULL, 
				    argv, 
				    envp, 
				    G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
				    NULL,
				    NULL,
				    &ed->pid,
				    &err)) {
			HAL_ERROR (("Couldn't spawn '%s' err=%s!", command_line, err->message));
			g_error_free (err);
			g_free (ed);
			ed = NULL;
		} else {
			ed->child_watch_id = g_child_watch_add (ed->pid, helper_child_exited, (gpointer) ed);
			if (timeout > 0)
				ed->timeout_watch_id = g_timeout_add (timeout, helper_child_timeout, (gpointer) ed);
			else
				ed->timeout_watch_id = (guint) -1;

			running_helpers = g_slist_prepend (running_helpers, ed);
		}
	}

	g_strfreev (envp);
	g_free (argv);

	return ed;
}

gboolean
hal_util_path_ascend (gchar *path)
{
	gchar *p;

	if (path == NULL)
		return FALSE;

	p = strrchr (path, '/');
	if (p == NULL)
		return FALSE;

	*p = '\0';
	return TRUE;
}

/** Given a directory and filename, open the file and search for the
 *  first line that starts with the given linestart string. Returns
 *  the rest of the line as a string if found.
 *
 *  @param  directory           Directory, e.g. "/proc/acpi/battery/BAT0"
 *  @param  file                File, e.g. "info"
 *  @param  linestart           Start of line, e.g. "serial number"
 *  @return                     NULL if not found, otherwise the remainder
 *                              of the line, e.g. ":           21805" if
 *                              the file /proc/acpi/battery/BAT0 contains
 *                              this line "serial number:           21805"
 *                              The string is only valid until the next
 *                              invocation of this function.
 */
gchar *
hal_util_grep_file (const gchar *directory, const gchar *file, const gchar *linestart)
{
	FILE *f;
	static gchar buf[512];
	static gchar filename[HAL_PATH_MAX];
	gchar *result;
	gsize linestart_len;

	result = NULL;

	if (file != NULL && strlen (file) > 0)
		snprintf (filename, sizeof (filename), "%s/%s", directory, file);
	else
		strncpy (filename, directory, sizeof (filename));
	f = fopen (filename, "r");
	if (f == NULL)
		goto out;

	linestart_len = strlen (linestart);

	do {
		if (fgets (buf, sizeof (buf), f) == NULL)
			goto out;

		if (strncmp (buf, linestart, linestart_len) == 0) {
			guint i;
			gsize len;

			len = strlen (buf);
			for (i = len - 1; i > 0; --i) {
				if (buf[i] == '\n' || buf[i] == '\r')
					buf[i] = '\0';
				else
					break;
			}
			break;
		}
	} while (TRUE);

	result = buf + linestart_len;

out:
	if (f != NULL)
		fclose (f);
	return result;
}

gchar *
hal_util_grep_string_elem_from_file (const gchar *directory, const gchar *file, 
				     const gchar *linestart, guint elem)
{
	gchar *line;
	gchar *res;
	static gchar buf[256];
	gchar **tokens;
	guint i, j;

	res = NULL;
	tokens = NULL;

	if (((line = hal_util_grep_file (directory, file, linestart)) == NULL) || (strlen (line) == 0))
		goto out;

	tokens = g_strsplit_set (line, " \t:", 0);
	for (i = 0, j = 0; tokens[i] != NULL; i++) {
		if (strlen (tokens[i]) == 0)
			continue;
		if (j == elem) {
			strncpy (buf, tokens[i], sizeof (buf));
			res = buf;
			goto out;
		}
		j++;
	}
	
out:
	if (tokens != NULL)
		g_strfreev (tokens);

	return res;
}

gint
hal_util_grep_int_elem_from_file (const gchar *directory, const gchar *file, 
				  const gchar *linestart, guint elem, guint base)
{
	gchar *endptr;
	gchar *strvalue;
	int value;

	value = G_MAXINT;

	if ((strvalue = hal_util_grep_string_elem_from_file (directory, file, linestart, elem)) == NULL)
		goto out;

	value = strtol (strvalue, &endptr, base);
	if (endptr == strvalue) {
		value = G_MAXINT;
		goto out;
	}

out:
	return value;
}

/** Get a string value from a formatted text file and assign it to
 *  a property on a device object.
 *
 *  Example: Given that the file /proc/acpi/battery/BAT0/info contains
 *  the line
 *
 *    "design voltage:          10800 mV"
 *
 *  then hal_util_set_string_elem_from_file (d, "system.battery.foo",
 *  "/proc/acpi/battery/BAT0", "info", "design voltage", 1) will assign
 *  the string "mV" to the property "system.battery.foo" on d.
 *
 *  @param  d                   Device object
 *  @param  key                 Property name
 *  @param  directory           Directory, e.g. "/proc/acpi/battery/BAT0"
 *  @param  file                File, e.g. "info"
 *  @param  linestart           Start of line, e.g. "design voltage"
 *  @param  elem                Element number after linestart to extract
 *                              excluding whitespace and ':' characters.
 *  @return                     TRUE, if, and only if, the value could be
 *                              extracted and the property was set
 */
gboolean
hal_util_set_string_elem_from_file (HalDevice *d, const gchar *key, 
				    const gchar *directory, const gchar *file, 
				    const gchar *linestart, guint elem)
{
	gboolean res;
	gchar *value;

	res = FALSE;

	if ((value = hal_util_grep_string_elem_from_file (directory, file, linestart, elem)) == NULL)
		goto out;

	res = hal_device_property_set_string (d, key, value);
out:
	return res;
}

/** Get an integer value from a formatted text file and assign it to
 *  a property on a device object.
 *
 *  Example: Given that the file /proc/acpi/battery/BAT0/info contains
 *  the line
 *
 *    "design voltage:          10800 mV"
 *
 *  then hal_util_set_int_elem_from_file (d, "system.battery.bar",
 *  "/proc/acpi/battery/BAT0", "info", "design voltage", 0) will assign
 *  the integer 10800 to the property "system.battery.foo" on d.
 *
 *  @param  d                   Device object
 *  @param  key                 Property name
 *  @param  directory           Directory, e.g. "/proc/acpi/battery/BAT0"
 *  @param  file                File, e.g. "info"
 *  @param  linestart           Start of line, e.g. "design voltage"
 *  @param  elem                Element number after linestart to extract
 *                              excluding whitespace and ':' characters.
 *  @return                     TRUE, if, and only if, the value could be
 *                              extracted and the property was set
 */
gboolean
hal_util_set_int_elem_from_file (HalDevice *d, const gchar *key, 
				 const gchar *directory, const gchar *file, 
				 const gchar *linestart, guint elem, guint base)
{
	gchar *endptr;
	gboolean res;
	gchar *strvalue;
	int value;

	res = FALSE;

	if ((strvalue = hal_util_grep_string_elem_from_file (directory, file, linestart, elem)) == NULL)
		goto out;

	value = strtol (strvalue, &endptr, base);
	if (endptr == strvalue)
		goto out;
	
	res = hal_device_property_set_int (d, key, value);
	
out:
	return res;

}

/** Get a value from a formatted text file, test it against a given
 *  value, and set a boolean property on a device object with the
 *  test result.
 *
 *  Example: Given that the file /proc/acpi/battery/BAT0/info contains
 *  the line
 *
 *    "present:                 yes"
 *
 *  then hal_util_set_bool_elem_from_file (d, "system.battery.baz",
 *  "/proc/acpi/battery/BAT0", "info", "present", 0, "yes") will assign
 *  the boolean TRUE to the property "system.battery.baz" on d.
 *
 *  If, instead, the line was
 *
 *    "present:                 no"
 *
 *  the value assigned will be FALSE.
 *
 *  @param  d                   Device object
 *  @param  key                 Property name
 *  @param  directory           Directory, e.g. "/proc/acpi/battery/BAT0"
 *  @param  file                File, e.g. "info"
 *  @param  linestart           Start of line, e.g. "design voltage"
 *  @param  elem                Element number after linestart to extract
 *                              excluding whitespace and ':' characters.
 *  @param  expected            Value to test against
 *  @return                     TRUE, if, and only if, the value could be
 *                              extracted and the property was set
 */
gboolean
hal_util_set_bool_elem_from_file (HalDevice *d, const gchar *key, 
				  const gchar *directory, const gchar *file, 
				  const gchar *linestart, guint elem, const gchar *expected)
{
	gchar *line;
	gboolean res;
	gchar **tokens;
	guint i, j;

	res = FALSE;
	tokens = NULL;

	if (((line = hal_util_grep_file (directory, file, linestart)) == NULL) || (strlen (line) == 0))
		goto out;

	tokens = g_strsplit_set (line, " \t:", 0);

	for (i = 0, j = 0; tokens[i] != NULL; i++) {
		if (strlen (tokens[i]) == 0)
			continue;
		if (j == elem) {
			hal_device_property_set_bool (d, key, strcmp (tokens[i], expected) == 0);
			res = TRUE;
			goto out;
		}
		j++;
	}


out:
	if (tokens != NULL)
		g_strfreev (tokens);

	return res;
}

gchar **
hal_util_dup_strv_from_g_slist (GSList *strlist)
{
	guint j;
	guint len;
	gchar **strv;
	GSList *i;

	len = g_slist_length (strlist);
	strv = g_new (char *, len + 1);

	for (i = strlist, j = 0; i != NULL; i = g_slist_next (i), j++) {
		strv[j] = g_strdup ((const gchar *) i->data);
	}
	strv[j] = NULL;

	return strv;	
}

/* -------------------------------------------------------------------------------------------------------------- */

typedef struct {
	HalDevice *d;
	gchar **programs;
	gchar **extra_env;
	guint next_program;

	HalCalloutsDone callback;
	gpointer userdata1;
	gpointer userdata2;

} Callout;

static void callout_do_next (Callout *c);

static void 
callout_terminated (HalDevice *d, gboolean timed_out, gint return_code, 
		    gpointer data1, gpointer data2, HalHelperData *helper_data)
{
	Callout *c;

	c = (Callout *) data1;
	callout_do_next (c);
}

static void
callout_do_next (Callout *c)
{

	/* Check if we're done */
	if (c->programs[c->next_program] == NULL) {
		HalDevice *d;
		gpointer userdata1;
		gpointer userdata2;
		HalCalloutsDone callback;

		d = c->d;
		userdata1 = c->userdata1;
		userdata2 = c->userdata2;
		callback = c->callback;

		g_strfreev (c->programs);
		g_strfreev (c->extra_env);
		g_free (c);

		callback (d, userdata1, userdata2);

	} else {
		hal_util_helper_invoke (c->programs[c->next_program], c->extra_env, c->d, 
					(gpointer) c, NULL, callout_terminated, HAL_HELPER_TIMEOUT);
		c->next_program++;
	}
}

static void
hal_callout_device (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2, 
		    GSList *programs, gchar **extra_env)
{
	Callout *c;

	c = g_new0 (Callout, 1);
	c->d = d;
	c->callback = callback;
	c->userdata1 = userdata1;
	c->userdata2 = userdata2;
	c->programs = hal_util_dup_strv_from_g_slist (programs);
	c->extra_env = g_strdupv (extra_env);
	c->next_program = 0;

	callout_do_next (c);
}

void
hal_util_callout_device_add (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2)
{
	GSList *programs;
	gchar *extra_env[2] = {"HALD_ACTION=add", NULL};

	if ((programs = hal_device_property_get_strlist (d, "info.callouts.add")) == NULL) {
		callback (d, userdata1, userdata2);
		goto out;
	}	

	HAL_INFO (("Add callouts for udi=%s", d->udi));

	hal_callout_device (d, callback, userdata1, userdata2, programs, extra_env);
out:
	;
}

void
hal_util_callout_device_remove (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2)
{
	GSList *programs;
	gchar *extra_env[2] = {"HALD_ACTION=remove", NULL};

	if ((programs = hal_device_property_get_strlist (d, "info.callouts.remove")) == NULL) {
		callback (d, userdata1, userdata2);
		goto out;
	}	

	HAL_INFO (("Remove callouts for udi=%s", d->udi));

	hal_callout_device (d, callback, userdata1, userdata2, programs, extra_env);
out:
	;
}

void
hal_util_callout_device_preprobe (HalDevice *d, HalCalloutsDone callback, gpointer userdata1, gpointer userdata2)
{
	GSList *programs;
	gchar *extra_env[2] = {"HALD_ACTION=preprobe", NULL};

	if ((programs = hal_device_property_get_strlist (d, "info.callouts.preprobe")) == NULL) {
		callback (d, userdata1, userdata2);
		goto out;
	}	

	HAL_INFO (("Preprobe callouts for udi=%s", d->udi));

	hal_callout_device (d, callback, userdata1, userdata2, programs, extra_env);
out:
	;
}

/** Kill all helpers we have running; useful when exiting hald.
 *
 *  @return                     Number of childs killed
 */
unsigned int 
hal_util_kill_all_helpers (void)
{
	unsigned int n;
	GSList *i;

	n = 0;
	for (i = running_helpers; i != NULL; i = i->next) {
		HalHelperData *ed;

		ed = i->data;
		HAL_INFO (("Killing helper with pid %d", ed->pid));
		kill (ed->pid, SIGTERM);
		n++;
	}

	return n;
}

void
hal_util_hexdump (const void *mem, unsigned int size)
{
	unsigned int i;
	unsigned int j;
	unsigned int n;
	const char *buf = (const char *) mem;

	n = 0;
	printf ("Dumping %d=0x%x bytes\n", size, size);
	while (n < size) {

		printf ("0x%04x: ", n);

		j = n;
		for (i = 0; i < 16; i++) {
			if (j >= size)
				break;
			printf ("%02x ", buf[j]);
			j++;
		}
		
		for ( ; i < 16; i++) {
			printf ("   ");
		}
		
		printf ("   ");
		
		j = n;
		for (i = 0; i < 16; i++) {
			if (j >= size)
				break;
			printf ("%c", isprint(buf[j]) ? buf[j] : '.');
			j++;
		}

		printf ("\n");
		
		n += 16;
	}
}

