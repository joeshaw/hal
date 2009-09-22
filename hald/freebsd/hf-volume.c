/***************************************************************************
 * CVSID: $Id$
 *
 * hf-volume.c : volume device support
 *
 * Copyright (C) 2006 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>

#include "../hald.h"
#include "../hald_dbus.h"
#include "../hald_runner.h"
#include "../logger.h"
#include "../util.h"
#include "../device_info.h"
#include "../osspec.h"

#include "hf-volume.h"
#include "hf-block.h"
#include "hf-storage.h"
#include "hf-util.h"

#define PROBE_VOLUME_TIMEOUT		(HAL_HELPER_TIMEOUT * 6)
#define HF_VOLUME_FUSE_DB		"/tmp/.fuse-mnts"

static void
hf_volume_get_mounts (struct statfs **mounts, int *n_mounts)
{
  g_return_if_fail(mounts != NULL);
  g_return_if_fail(n_mounts != NULL);

  *n_mounts = getmntinfo(mounts, MNT_NOWAIT);
  if (*n_mounts == 0)
    {
      HAL_WARNING(("unable to get list of mounted filesystems: %s", g_strerror(errno)));
      *mounts = NULL;
    }
}

static char *
hf_volume_resolve_fuse (const char *special)
{
  gchar *contents;
  gchar **lines;
  gsize len;
  int i;

  g_return_val_if_fail(special != NULL, NULL);

  if (! g_file_get_contents(HF_VOLUME_FUSE_DB, &contents, &len, NULL))
    return g_strdup(special);

  lines = g_strsplit(contents, "\n", 0);
  g_free(contents);

  for (i = 0; lines && lines[i]; i++)
    {
      gchar **fields;

      fields = g_strsplit(lines[i], "=", 2);
      if (fields && g_strv_length(fields) == 2)
        {
          if (strcmp(fields[0], special) == 0)
	    {
	      g_strfreev(fields);
	      g_strfreev(lines);
	      return g_strdup(fields[1]);
	    }
	}
      g_strfreev(fields);
    }

    g_strfreev(lines);

    return g_strdup(special);
}

static char *
hf_volume_resolve_special (const char *special)
{
  g_return_val_if_fail(special != NULL, NULL);

  if (strstr(special, "fuse"))
    return hf_volume_resolve_fuse(special);

  return g_strdup(special);
}

static const struct statfs *
hf_volume_mounts_find (const struct statfs *mounts,
		       int n_mounts,
		       const char *special)
{
  int i;

  g_return_val_if_fail(mounts != NULL, NULL);
  g_return_val_if_fail(special != NULL, NULL);

  for (i = 0; i < n_mounts; i++)
    {
      char *resolved;

      resolved = hf_volume_resolve_special(mounts[i].f_mntfromname);
      if (! strcmp(resolved, special))
        {
	  g_free(resolved);
          return &mounts[i];
	}

      g_free(resolved);
    }

  return NULL;
}

static void
hf_volume_device_update_mount_properties (HalDevice *device,
					  const struct statfs *mounts,
					  int n_mounts)
{
  const struct statfs *mount = NULL;

  g_return_if_fail(HAL_IS_DEVICE(device));

  if (mounts)
    {
      const char *special;

      special = hal_device_property_get_string(device, "block.device");
      if (special)
	{
	  mount = hf_volume_mounts_find(mounts, n_mounts, special);
          if (mount && strcmp(special, mount->f_mntfromname))
            hal_device_property_set_string(device, "volume.freebsd.real_mounted_device", mount->f_mntfromname);
	  else
	    hal_device_property_remove(device, "volume.freebsd.real_mounted_device");
	}
    }

  hal_device_property_set_bool(device, "volume.is_mounted", mount != NULL);
  hal_device_property_set_bool(device, "volume.is_mounted_read_only", mount && (mount->f_flags & MNT_RDONLY) != 0);
  hal_device_property_set_string(device, "volume.mount_point", mount ? mount->f_mntonname : NULL);
}

HalDevice *
hf_volume_device_add (HalDevice *parent,
		      HalDevice *storage_device,
		      gboolean has_children,
		      gboolean is_swap,
		      const char *devname,
		      const char *gclass,
		      const char *gstr_type,
		      int gtype,
		      int gindex,
		      dbus_int64_t partition_offset,
		      dbus_int64_t partition_size)
{
  HalDevice *device;
  char *partition_offset_str;
  char *partition_size_str;
  char *type_str;
  char *geom_class;
  char *index_str;
  gboolean is_volume;

  g_return_val_if_fail(HAL_IS_DEVICE(parent), NULL);
  g_return_val_if_fail(HAL_IS_DEVICE(storage_device), NULL);
  g_return_val_if_fail(devname != NULL, NULL);

  device = hf_device_new(parent);

  hf_block_device_enable(device, devname);

  if (! hf_device_preprobe(device))
    goto end;

  if (! gclass)
    geom_class = g_strdup("");
  else
    geom_class = g_strdup(gclass);

  if (gstr_type)
    type_str = g_strdup(gstr_type);
  else
    type_str = g_strdup_printf("%d", gtype);

  index_str = g_strdup_printf("%d", gindex);

  partition_offset_str = g_strdup_printf("%ju", partition_offset);
  partition_size_str = g_strdup_printf("%ju", partition_size);

  is_volume = hf_runner_run_sync(device, PROBE_VOLUME_TIMEOUT,
                                 "hald-probe-volume",
				 "HF_HAS_CHILDREN", HF_BOOL_TO_STRING(has_children),
				 "HF_IS_SWAP", HF_BOOL_TO_STRING(is_swap),
				 "HF_VOLUME_GEOM_CLASS", geom_class,
				 "HF_VOLUME_PART_TYPE", type_str,
				 "HF_VOLUME_PART_INDEX", index_str,
				 "HF_VOLUME_OFFSET", partition_offset_str,
				 "HF_VOLUME_SIZE", partition_size_str,
				 NULL) == 0;
  if (is_volume)
    {
      struct statfs *mounts;
      int n_mounts;

      hf_volume_get_mounts(&mounts, &n_mounts);
      hf_volume_device_update_mount_properties(device, mounts, n_mounts);
    }

  g_free(partition_offset_str);
  g_free(partition_size_str);
  g_free(index_str);
  g_free(type_str);
  g_free(geom_class);

  hf_block_device_complete(device, storage_device, is_volume);

  hf_device_add(device);

 end:
  return device;
}

static void
hf_volume_update_mounts (void)
{
  GSList *l;
  struct statfs *mounts;
  int n_mounts;

  hf_volume_get_mounts(&mounts, &n_mounts);

  HF_LIST_FOREACH(l, hald_get_gdl()->devices)
    {
      HalDevice *device = l->data;

      if (hal_device_property_get_bool(device, "block.is_volume"))
	{
	  device_property_atomic_update_begin();
	  hf_volume_device_update_mount_properties(device, mounts, n_mounts);
	  device_property_atomic_update_end();
	}
    }
}

void
hf_volume_update_mount (HalDevice *device)
{
  struct statfs *mounts;
  int n_mounts;

  g_return_if_fail(device != NULL);

  hf_volume_get_mounts(&mounts, &n_mounts);

  device_property_atomic_update_begin();
  hf_volume_device_update_mount_properties(device, mounts, n_mounts);
  device_property_atomic_update_end();
}

static gboolean
hf_volume_update_mounts_timeout_cb (gpointer data)
{
  if (hf_is_waiting)
    return TRUE;

  hf_volume_update_mounts();

  return TRUE;
}

static void
hf_volume_init (void)
{
  g_timeout_add(3000, hf_volume_update_mounts_timeout_cb, NULL);
}

static gboolean
hf_volume_device_reprobe (HalDevice *device)
{
  const char *storage_device_udi;
  HalDevice *storage_device;

  storage_device_udi = hal_device_property_get_string(device, "block.storage_device");
  if (! storage_device_udi || ! strcmp(storage_device_udi, hal_device_get_udi(device)))
    return FALSE;		/* not a child of a storage device */

  storage_device = hal_device_store_find(hald_get_gdl(), storage_device_udi);
  g_assert(storage_device != NULL);

  hf_device_remove_tree(device);
  osspec_probe();

  hf_storage_device_probe(storage_device, TRUE);

  return TRUE;
}

HFHandler hf_volume_handler = {
  .init =		hf_volume_init,
  .device_reprobe =	hf_volume_device_reprobe
};
