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

#include "../osspec.h"
#include "../logger.h"
#include "../hald.h"
#include "../callout.h"
#include "../device_info.h"
#include "../hald_conf.h"

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

/** Given a sysfs-path for a device, this functions finds the sysfs
 *  path representing the parent of the given device by truncation.
 *
 *  @param  path                Sysfs-path of device to find parent for
 *  @return                     Path for parent or NULL if there is no parent; 
 *                              must be freed by caller
 */
gchar *
hal_util_get_parent_sysfs_path (const gchar *path)
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



/* Returns the path of the udevinfo program 
 *
 * @return                      Path or NULL if udevinfo program is not found
 */
static const gchar *
hal_util_get_udevinfo_path (void)
{
	guint i;
	struct stat s;
	static gchar *path = NULL;
	gchar *possible_paths[] = { 
		"/sbin/udevinfo",
		"/usr/bin/udevinfo",
		"/usr/sbin/udevinfo",
		"/usr/local/sbin/udevinfo"
	};

	if (path != NULL)
		return path;

	for (i = 0; i < sizeof (possible_paths) / sizeof (char *); i++) {
		if (stat (possible_paths[i], &s) == 0 && S_ISREG (s.st_mode)) {
			path = possible_paths[i];
			break;
		}
	}
	return path;
}

/** Get the name of the special device file given the sysfs path.
 *
 *  @param  sysfs_path          Path to class device in sysfs
 *  @param  dev_file            Where the special device file name should be stored
 *  @param  dev_file_length     Size of dev_file character array
 *  @return                     TRUE only if the device file could be found
 */
gboolean
hal_util_get_device_file (const gchar *sysfs_path, gchar *dev_file, gsize dev_file_length)
{
	int i;
	gsize sysfs_path_len;
	gsize sysfs_mount_path_len;
	gchar sysfs_path_trunc[HAL_PATH_MAX];
	gchar sysfs_path_dev_trunc[HAL_PATH_MAX + 4];
	char *udev_argv[7] = { NULL, 
			       "-r", "-q", "name", "-p",
			       sysfs_path_trunc, NULL };
	char *udev_stdout;
	char *udev_stderr;
	int udev_exitcode;
	struct stat statbuf;

	/* check for dev file in sysfs path */
	sysfs_path_len = strlen (sysfs_path);
	strncpy (sysfs_path_dev_trunc, sysfs_path, HAL_PATH_MAX);
	strncat (sysfs_path_dev_trunc + sysfs_path_len, "/dev", 4);
	if (g_stat (sysfs_path_dev_trunc, &statbuf) != 0)
		return FALSE;

	/* get path to udevinfo */
	udev_argv[0] = (char *) hal_util_get_udevinfo_path ();
	if (udev_argv[0] == NULL)
		return FALSE;

	/* compute truncated sysfs path as udevinfo doesn't want the sysfs_mount_path (e.g. /sys) prefix */
	sysfs_mount_path_len = strlen (hal_sysfs_path);
	if (strlen (sysfs_path) > sysfs_mount_path_len) {
		strncpy (sysfs_path_trunc, sysfs_path + sysfs_mount_path_len, HAL_PATH_MAX - sysfs_mount_path_len);
	}

	/* Now invoke udevinfo */
	if (udev_argv[0] == NULL || g_spawn_sync ("/",
						  udev_argv,
						  NULL,
						  0,
						  NULL,
						  NULL,
						  &udev_stdout,
						  &udev_stderr,
						  &udev_exitcode,
						  NULL) != TRUE) {
		HAL_ERROR (("Couldn't invoke %s", udev_argv[0]));
		return FALSE;
	}

	if (udev_exitcode != 0) {
		HAL_ERROR (("%s returned %d for %s", udev_argv[0], udev_exitcode, sysfs_path_trunc));
		return FALSE;
	}

	/* sanitize string returned by udev */
	for (i = 0; udev_stdout[i] != 0; i++) {
		if (udev_stdout[i] == '\r' || udev_stdout[i] == '\n') {
			udev_stdout[i] = 0;
			break;
		}
	}

	/*HAL_INFO (("got device file %s for %s", udev_stdout, sysfs_path));*/

	strncpy (dev_file, udev_stdout, dev_file_length);
	return TRUE;
}


/** Find the closest ancestor by looking at sysfs paths
 *
 *  @param  sysfs_path           Path into sysfs, e.g. /sys/devices/pci0000:00/0000:00:1d.7/usb1/1-0:1.0
 *  @return                      Parent Hal Device Object or #NULL if there is none
 */
HalDevice *
hal_util_find_closest_ancestor (const gchar *sysfs_path)
{	
	gchar buf[512];
	HalDevice *parent;

	parent = NULL;

	strncpy (buf, sysfs_path, sizeof (buf));
	do {
		char *p;

		p = strrchr (buf, '/');
		if (p == NULL)
			break;
		*p = '\0';

		parent = hal_device_store_match_key_value_string (hald_get_gdl (), 
								  "linux.sysfs_path_device", 
								  buf);
		if (parent != NULL)
			break;

	} while (TRUE);

	return parent;
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

gboolean
hal_util_set_string_from_file (HalDevice *d, const gchar *key, const gchar *directory, const gchar *file)
{
	FILE *f;
	gchar buf[256];
	gchar path[HAL_PATH_MAX];
	gboolean ret;
	gsize len;

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

	len = strlen (buf);
	if (len>0)
		buf[len-1] = '\0';

	ret = hal_device_property_set_string (d, key, buf);

out:
	if (f != NULL)
		fclose (f);

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

	g_strlcpy (dst, buf, dstsize);
	if (hal_device_store_find (store, dst) == NULL)
		goto out;

	for (i = 0; ; i++) {
		g_snprintf (dst, dstsize, "%s-%d", buf, i);
		if (hal_device_store_find (store, dst) == NULL)
			goto out;
	}

out:
	;
}


typedef struct
{
	GPid pid;
	guint timeout_watch_id;
	guint child_watch_id;

	HelperTerminatedCB cb;
	gpointer data1;
	gpointer data2;

	HalDevice *d;
} HelperData;

static gboolean
helper_child_timeout (gpointer data)
{
	HelperData *ed = (HelperData *) data;

	HAL_INFO (("child timeout for pid %d", ed->pid));

	/* kill kenny! kill it!! */
	kill (ed->pid, SIGTERM);
	/* TODO: yikes; what about removing the zombie? */

	g_source_remove (ed->child_watch_id);
	g_spawn_close_pid (ed->pid);

	ed->cb (ed->d, TRUE, -1, ed->data1, ed->data2);

	g_free (ed);
	return FALSE;
}

static void 
helper_child_exited (GPid pid, gint status, gpointer data)
{
	HelperData *ed = (HelperData *) data;

	HAL_INFO (("child exited for pid %d", ed->pid));

	g_source_remove (ed->timeout_watch_id);
	g_spawn_close_pid (ed->pid);

	ed->cb (ed->d, FALSE, WEXITSTATUS (status), ed->data1, ed->data2);

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


gboolean
helper_invoke (const gchar *path, HalDevice *d, gpointer data1, gpointer data2, HelperTerminatedCB cb, guint timeout)
{
	gboolean ret;
	HelperData *ed;
	gchar *argv[] = {(gchar *) path, NULL};
	gchar **envp;
	gchar **ienvp;
	GError *err;
	guint num_env_vars;
	guint i;
	guint num_properties;

	ed = g_new0 (HelperData, 1);
	ed->data1 = data1;
	ed->data2 = data2;
	ed->d = d;
	ed->cb = cb;

	num_properties = hal_device_num_properties (d);
	num_env_vars = num_properties + 2;
	if (hald_is_verbose)
		num_env_vars++;
	if (hald_is_initialising)
		num_env_vars++;
	if (hald_is_shutting_down)
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
	if (hald_is_shutting_down)
		envp[i++] = g_strdup ("HALD_SHUTDOWN=1");
	envp[i++] = NULL;

	
	err = NULL;
	if (!g_spawn_async (NULL, 
			    argv, 
			    envp, 
			    G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
			    NULL,
			    NULL,
			    &ed->pid,
			    &err)) {
		HAL_ERROR (("Couldn't spawn '%s' err=%s!", path, err->message));
		g_error_free (err);
		ret = FALSE;
		g_free (ed);
	} else {
		ed->child_watch_id = g_child_watch_add (ed->pid, helper_child_exited, (gpointer) ed);
		ed->timeout_watch_id = g_timeout_add (timeout, helper_child_timeout, (gpointer) ed);
		ret = TRUE;
	}

	g_strfreev (envp);

	return ret;
}

