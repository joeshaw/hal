/***************************************************************************
 * CVSID: $Id$
 *
 * pstore.c : persistent property store on disk
 *
 * Copyright (C) 2004 Kay Sievers, <kay.sievers@vrfy.org>
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
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <glib.h>

#include "logger.h"
#include "pstore.h"

#define PSTR		"String:"
#define PINT32		"Int32:"
#define PBOOL		"Bool:"
#define PDOUBLE		"Double:"
#define UDI_STRIP	"/org/freedesktop/Hal"

static char *pstore_uuid = NULL;

struct _HalPStore {
	char *path;
	const char *uuid;
};

/** Create relative or absolute path.
 *
 */
static int
create_path(const char *path)
{
	char *p;
	char *pos;
	struct stat stats;

	if (stat (path, &stats) == 0)
		return 0;

	p = g_strdup (path);
	pos = strrchr (p, '/');
	if (pos != NULL && pos != p) {
		pos[0] = '\0';
		create_path (p);
	}
	g_free (p);

	return mkdir(path, 0755);
}

/** Return absolute filename with simplified udi string.
 *
 */
static char
*build_path (HalPStore *pstore,
	     const char *udi, const char *file)
{
	char *path;
	const char *udi_s;

	udi_s = udi;
	/* strip namespace part of udi string */
	if (udi != NULL &&
	    g_ascii_strncasecmp(udi, UDI_STRIP, sizeof(UDI_STRIP)-1) == 0)
			udi_s = &udi[sizeof(UDI_STRIP)-1];

	path = g_build_filename (G_DIR_SEPARATOR_S,
				 pstore->path, pstore->uuid,
				 udi_s, file, NULL);

	return path;
}

/** Init pstore system by reading or generating our uuid.
 *
 */
void
hal_pstore_init (const char *uuid_file)
{
	char *uuid;
	char hostname[HOST_NAME_MAX];
	char *p;
	GIOChannel *io;
	GError *error = NULL;
	int written;

	g_file_get_contents (uuid_file, &uuid, NULL, NULL);

	if (uuid == NULL) {
		/* create new uuid and save it to disk */
		gethostname (hostname, HOST_NAME_MAX);
		uuid = g_strdup_printf ("%s-%lx", hostname, time(NULL));

		p = g_path_get_dirname (uuid_file);
		create_path(p);
		g_free(p);

		io = g_io_channel_new_file (uuid_file, "w", &error);
		if (error != NULL) {
			HAL_WARNING (("error creating file %s", error->message));
			g_error_free (error);
			return;
		}
		g_io_channel_write_chars (io, uuid, -1, &written, &error);
		g_io_channel_shutdown (io, TRUE, NULL);
		if (error != NULL) {
			HAL_WARNING (("error writing to file %s", error->message));
			g_error_free (error);
			return;
		}
	}

	HAL_DEBUG (("uuid is %s", uuid));
	pstore_uuid = uuid;
}


/** Open pstore on the given location.
 *
 */
HalPStore
*hal_pstore_open (const char *path)
{
	HalPStore *pstore;
	char *p;

	g_return_val_if_fail (pstore_uuid != NULL, NULL);
	g_return_val_if_fail (path != NULL, NULL);

	pstore = g_new0 (HalPStore, 1);

	pstore->path = g_strdup (path);

	pstore->uuid = pstore_uuid;

	p = g_build_path (G_DIR_SEPARATOR_S,
			  pstore->path, pstore->uuid, NULL);
	create_path(p);
	g_free(p);

	HAL_DEBUG (("opened pstore at %s/%s/", pstore->path, pstore->uuid));

	return pstore;
}

/** Turn around the OPEN sign, in the store's entrance :)
 *
 */
void
hal_pstore_close (HalPStore *pstore)
{
	g_return_if_fail (pstore != NULL);

	g_free (pstore->path);
	g_free (pstore);
}

/** Save a property value to the disk.
 *
 */
void
hal_pstore_save_property (HalPStore *pstore,
			  HalDevice *device, HalProperty *prop)
{
	char *path;
	char *file;
	GIOChannel *io;
	GError *error = NULL;
	char *id;
	char *value;
	char *buf;
	int written;

	g_return_if_fail (pstore != NULL);
	g_return_if_fail (device != NULL);
	g_return_if_fail (prop != NULL);

	switch (hal_property_get_type (prop)) {
	case DBUS_TYPE_STRING:
		id = PSTR;
		break;
	case DBUS_TYPE_INT32:
		id = PINT32;
		break;
	case DBUS_TYPE_BOOLEAN:
		id = PBOOL;
		break;
	case DBUS_TYPE_DOUBLE:
		id = PDOUBLE;
		break;
	default:
		HAL_WARNING (("unknown property type %s",
			   hal_property_get_key (prop)));
		return;
	}

	path = build_path (pstore, hal_device_get_udi (device), NULL);
	create_path(path);
	g_free(path);

	file = build_path (pstore,
			   hal_device_get_udi (device),
			   hal_property_get_key (prop));

	io = g_io_channel_new_file (file, "w", &error);
	if (error != NULL) {
		HAL_WARNING (("error creating file %s (%s)", file, error->message));
		g_error_free (error);
		g_free (file);
		return;
	}

	value = hal_property_to_string (prop);

	buf = g_strconcat (id, value, NULL);
	g_free (value);

	HAL_DEBUG (("write %s to disk", hal_property_get_key (prop)));

	g_io_channel_write_chars (io, buf, -1, &written, &error);
	if (error != NULL) {
		HAL_WARNING (("error writing to file %s (%s)", file, error->message));
		g_error_free (error);
	}

	g_io_channel_shutdown (io, TRUE, NULL);
	g_free (buf);
	g_free (file);
}

/** Load a stored property value from the disk.
 *
 */
void
hal_pstore_load_property (HalPStore *pstore,
			   HalDevice *device, const char *key)
{
	char *file;
	char *buf;
	char *str;
	int i;
	double d;

	g_return_if_fail (pstore != NULL);
	g_return_if_fail (device != NULL);
	g_return_if_fail (key != NULL);

	file = build_path (pstore, hal_device_get_udi (device), key);

	g_file_get_contents (file, &buf, NULL, NULL);
	g_free (file);
	if (buf == NULL) {
		HAL_DEBUG (("error reading file %s", file));
		return;
	}

	if (g_ascii_strncasecmp (buf, PSTR, sizeof (PSTR)-1) == 0) {
		str =  &buf[sizeof (PSTR)-1];
		hal_device_property_set_string (device, key, str);
		HAL_INFO (("STRING %s read for %s", str, key));
		goto exit;
	}

	if (g_ascii_strncasecmp (buf, PINT32, sizeof (PINT32)-1) == 0) {
		str =  &buf[sizeof (PINT32)-1];
		i = strtol (str, NULL, 10);
		hal_device_property_set_int (device, key, i);
		goto exit;
	}

	if (g_ascii_strncasecmp (buf, PBOOL, sizeof (PBOOL)-1) == 0) {
		str =  &buf[sizeof (PBOOL)-1];
		if (g_ascii_strcasecmp (str, "true") == 0)
			i = TRUE;
		else
			i = FALSE;
		hal_device_property_set_bool (device, key, i);
		goto exit;
	}

	if (g_ascii_strncasecmp (buf, PDOUBLE, sizeof (PDOUBLE)-1) == 0) {
		str =  &buf[sizeof (PDOUBLE)-1];
		d = atof (str);
		hal_device_property_set_double (device, key, d);
		goto exit;
	}

	HAL_WARNING (("error determining pstore property type %s", key));

exit:
	g_free (buf);
}

/** Delete a stored property from the disk.
 *
 */
void
hal_pstore_delete_property (HalPStore *pstore,
			    HalDevice *device, HalProperty *prop)
{
	char *file;

	g_return_if_fail (pstore != NULL);
	g_return_if_fail (device != NULL);
	g_return_if_fail (prop != NULL);

	file = build_path (pstore,
			   hal_device_get_udi (device),
			   hal_property_get_key (prop));

	HAL_DEBUG (("unlinking %s", file));

	unlink(file);
	g_free(file);
}

/** Load all stored properties of a device from the disk.
 *
 */
void
hal_pstore_load_device (HalPStore *pstore,
			HalDevice *device)
{
	GDir *dir;
	GError *error  = NULL;
	const char *dirname;
	char *path;

	g_return_if_fail (pstore != NULL);
	g_return_if_fail (device != NULL);

	path = build_path (pstore,
			   hal_device_get_udi (device),
			   NULL);

	HAL_DEBUG (("reading directory %s", path));

	dir = g_dir_open (path, 0, &error);
	if (error)
		goto exit;

	while (1) {
		dirname = g_dir_read_name (dir);
		if (dirname == NULL)
			break;

		hal_pstore_load_property (pstore, device, dirname);
	}

	g_dir_close (dir);

exit:
	g_free (path);
}
