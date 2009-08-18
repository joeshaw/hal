/***************************************************************************
 * CVSID: $Id$
 *
 * hf-storage.c : storage device support
 *
 * Copyright (C) 2006, 2007 Jean-Yves Lefort <jylefort@FreeBSD.org>
 *                    Joe Marcus Clarke <marcus@FreeBSD.org>
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

#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/disklabel.h>

#include "../logger.h"
#include "../osspec.h"

#include "hf-storage.h"
#include "hf-block.h"
#include "hf-devd.h"
#include "hf-devtree.h"
#include "hf-volume.h"
#include "hf-util.h"

typedef struct
{
  char *name;
  char *conf;
} Disk;

typedef struct
{
  char *class;
  char *dev;
  char *str_type;
  guint hash;
  guint64 mediasize;
  guint64 offset;
  guint sectorsize;
  gint type;
  gint index;
} Geom_Object;

static GNode *hf_storage_geom_tree = NULL;
static GHashTable *hf_storage_geom_hash = NULL;

static void hf_storage_init_geom (gboolean force);
static gboolean hf_storage_device_has_addon (HalDevice *device);

static void
hf_storage_geom_free (gpointer data)
{
  Geom_Object *geom_obj;

  g_return_if_fail(data != NULL);

  geom_obj = (Geom_Object *) data;
  g_free(geom_obj->class);
  g_free(geom_obj->dev);
  g_free(geom_obj->str_type);

  g_free(geom_obj);
}

/* Disk_Names() has a memory leak, hence this */
static char **
hf_storage_get_disk_names (GError **err)
{
  char *list;
  char **names;

  list = hf_get_string_sysctl(err, "kern.disks");
  if (! list)
    return NULL;

  names = g_strsplit(list, " ", 0);
  g_free(list);

  return names;
}

static gboolean
hf_storage_class_is_partitionable (const char *geom_class)
{
  return (! strcmp(geom_class, "MBR") ||
          ! strcmp(geom_class, "MBREXT") ||
	  ! strcmp(geom_class, "PART") ||
	  ! strcmp(geom_class, "JOURNAL") ||
	  ! strcmp(geom_class, "GPT") ||
          ! strcmp(geom_class, "APPLE") || ! strcmp(geom_class, "SUN"));
}

static gboolean
hf_storage_geom_has_partitions (const Geom_Object *geom_obj, GNode *node)
{
  if (! node || ! geom_obj)
    return FALSE;

  if (g_node_n_children(node) > 0)
    return TRUE;

  /*
  if (hf_storage_class_is_partitionable(geom_obj->class) &&
      g_node_next_sibling(node) != NULL)
    {
      GNode *sibling;

      for (sibling = g_node_next_sibling(node); sibling;
           sibling = g_node_next_sibling(sibling))
        {
          Geom_Object *sibling_geom;

          sibling_geom = g_hash_table_lookup(hf_storage_geom_hash,
                                             sibling->data);

	  if (sibling_geom &&
              hf_storage_class_is_partitionable(sibling_geom->class))
            return TRUE;
        }
    }
    */

  return FALSE;
}

static gboolean
hf_storage_geom_is_swap (const Geom_Object *geom_obj)
{
  g_return_val_if_fail(geom_obj != NULL, FALSE);

  return (! strcmp(geom_obj->class, "BSD") && geom_obj->type == FS_SWAP)
	|| ((! strcmp(geom_obj->class, "MBR") ||
             ! strcmp(geom_obj->class, "MBREXT"))
	&& (geom_obj->type == 0x18		/* AST Windows swapfile */
	    || geom_obj->type == 0x42		/* SFS or Linux swap */
	    || geom_obj->type == 0x82		/* Linux swap or Solaris x86 */
	    || geom_obj->type == 0xB8));	/* BSDI BSD/386 swap */
}

static void
hf_storage_device_probe_geom (HalDevice *parent,
			      HalDevice *storage_device,
			      const Geom_Object *geom_obj)
{
  HalDevice *device = NULL;
  GNode *node;
  Geom_Object *next;

  g_return_if_fail(HAL_IS_DEVICE(parent));
  g_return_if_fail(HAL_IS_DEVICE(storage_device));

  if (! geom_obj)
    return;

  node = g_node_find(hf_storage_geom_tree, G_PRE_ORDER, G_TRAVERSE_ALL,
                     GUINT_TO_POINTER(geom_obj->hash));

  if (! node)
    return;

  if (geom_obj->type != FS_UNUSED)
    {
      char *special;

      special = g_strdup_printf("/dev/%s", geom_obj->dev);
      device = hal_device_store_match_key_value_string(hald_get_gdl(), "block.device", special);
      g_free(special);

      if (! device)
	device = hf_volume_device_add(parent,
				      storage_device,
				      hf_storage_geom_has_partitions(geom_obj, node),
				      hf_storage_geom_is_swap(geom_obj),
				      geom_obj->dev,
				      geom_obj->class,
				      geom_obj->str_type,
				      geom_obj->type,
				      geom_obj->index,
				      geom_obj->offset,
				      geom_obj->mediasize);
    }

  if (! device || ! hal_device_property_get_bool(device, "info.ignore"))
    {
      next = (g_node_first_child(node)) ?
              g_hash_table_lookup(hf_storage_geom_hash,
              (g_node_first_child(node))->data) : NULL;
      hf_storage_device_probe_geom(device ? device : parent, storage_device,
                                   next);
    }
  next = (g_node_next_sibling(node)) ? g_hash_table_lookup(hf_storage_geom_hash,
          (g_node_next_sibling(node))->data) : NULL;
  hf_storage_device_probe_geom(parent, storage_device, next);
}

static void
hf_storage_device_probe_partitions (HalDevice *device)
{
  char *diskname;
  guint hash;
  Geom_Object *geom_obj;
  GNode *node;

  g_return_if_fail(HAL_IS_DEVICE(device));

  diskname = hf_devtree_device_get_name(device);
  if (! diskname)
    return;

  hash = g_str_hash(diskname);

  node = g_node_find(hf_storage_geom_tree, G_PRE_ORDER, G_TRAVERSE_ALL,
                     GUINT_TO_POINTER(hash));
  if (! node || ! g_node_first_child(node))
    return;

  geom_obj = g_hash_table_lookup(hf_storage_geom_hash,
                                 (g_node_first_child(node))->data);
  if (! geom_obj)
    return;

  hf_storage_device_probe_geom(device, device, geom_obj);

  g_free(diskname);
}

static gboolean
hf_storage_device_has_partitions (HalDevice *device)
{
  gboolean has = FALSE;
  char *diskname;
  Geom_Object *geom_obj;
  guint hash;

  g_return_val_if_fail(HAL_IS_DEVICE(device), FALSE);

  diskname = hf_devtree_device_get_name(device);
  if (! diskname)
    return FALSE;

  hash = g_str_hash(diskname);
  geom_obj = g_hash_table_lookup(hf_storage_geom_hash,
                                 GUINT_TO_POINTER(hash));
  if (geom_obj)
    {
      GNode *node;

      node = g_node_find(hf_storage_geom_tree, G_PRE_ORDER,
                         G_TRAVERSE_ALL, GUINT_TO_POINTER(hash));
      if (hf_storage_geom_has_partitions(geom_obj, node))
        has = TRUE;
    }
  g_free(diskname);

  return has;
}

static HalDevice *
hf_storage_device_new (HalDevice *parent, const char *diskname)
{
  HalDevice *device;

  g_return_val_if_fail(diskname != NULL, NULL);

  device = hf_device_new(parent);

  hf_devtree_device_set_name(device, diskname);

  hf_block_device_enable(device, diskname);
  hf_storage_device_enable(device);
  hf_block_device_complete(device, device, FALSE);

  return device;
}

void
hf_storage_device_probe (HalDevice *device, gboolean only_media)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hf_storage_init_geom(TRUE);

  if (hf_runner_run_sync(device, 0, "hald-probe-storage",
			 "HF_HAS_CHILDREN", HF_BOOL_TO_STRING(hf_storage_device_has_partitions(device)),
			 "HF_ONLY_CHECK_FOR_MEDIA", HF_BOOL_TO_STRING(only_media),
			 NULL) == 2)
    {				/* add a child volume */
      char *devname;

      devname = hf_devtree_device_get_name(device);
      g_assert(devname != NULL);

      /*
       * We do not need to check if the device already exists:
       *
       *   - if we're called from osspec_device_rescan() or
       *     osspec_device_reprobe(), the child device has been removed.
       *   - otherwise, the storage device is new and has no child.
       */

      hf_volume_device_add(device, device, FALSE, FALSE, devname, NULL, NULL, -1, 0, 0, 0);
      g_free(devname);
    }
  else				/* probe partitions */
    hf_storage_device_probe_partitions(device);
}

static void
hf_storage_probe (void)
{
  GError *err = NULL;
  char **disks;

  /* add disks which have not been handled by hf-ata or hf-scsi */

  disks = hf_storage_get_disk_names(&err);
  if (disks)
    {
      int i;

      for (i = 0; disks[i]; i++)
	{
	  HalDevice *device;

	  device = hf_devtree_find_from_name(hald_get_gdl(), disks[i]);
	  if (! device)
	    {
	      HalDevice *parent;

	      /* device not found, add a generic storage device */

	      parent = hf_devtree_find_parent_from_name(hald_get_gdl(), disks[i]);
	      if (! parent || ! hal_device_property_get_bool(parent, "info.ignore"))
		{
		  device = hf_storage_device_new(parent, disks[i]);
		  hf_storage_device_add(device);
		}
	    }
	}

      g_strfreev(disks);
    }
  else
    {
      HAL_WARNING(("unable to get disk list: %s", err->message));
      g_error_free(err);
    }
}

static GSList *
hf_storage_parse_conftxt (const char *conftxt)
{
  GSList *disks = NULL;
  char **lines;
  Disk *disk = NULL;
  GString *disk_conf = NULL;
  GNode *root, *parent;
  GNode *child = NULL;
  GHashTable *table;
  int curr_depth = 0;
  int i;

  if (! conftxt)
    return NULL;

  root = g_node_new (NULL);
  parent = root;

  table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                (GDestroyNotify) hf_storage_geom_free);

  lines = g_strsplit(conftxt, "\n", 0);
  for (i = 0; lines[i]; i++)
    {
      Geom_Object *geom_obj;
      char **fields;
      int depth;
      guint hash;

      if (! *lines[i])
        continue;

      fields = g_strsplit(lines[i], " ", 0);
      if (g_strv_length(fields) < 3)
        {
          g_strfreev(fields);
          continue;
	}

      depth = atoi(fields[0]);
      hash = g_str_hash(fields[2]);
      if (g_hash_table_lookup(table, GUINT_TO_POINTER(hash)) != NULL)
        {
          g_strfreev(fields);
	  curr_depth = depth;
	  continue;
	}

      geom_obj = g_new0(Geom_Object, 1);

      geom_obj->class = g_strdup(fields[1]);
      geom_obj->dev = g_strdup(fields[2]);
      geom_obj->type = -1;	/* We use -1 here to denote a missing type. */
      geom_obj->hash = hash;

      if (g_strv_length(fields) >= 5)
        {
          geom_obj->mediasize = strtoumax(fields[3], NULL, 10);
	  geom_obj->sectorsize = (guint) strtoul(fields[4], NULL, 10);
	}

      if (g_strv_length(fields) >= 7)
        if (! strcmp(fields[5], "i"))
          geom_obj->index = atoi(fields[6]) + 1;

      if (g_strv_length(fields) >= 9)
        if (! strcmp(fields[7], "o"))
            geom_obj->offset = strtoumax(fields[8], NULL, 10);

      if (g_strv_length(fields) >= 11)
        {
          if (! strcmp (fields[9], "ty"))
            {
              if (! strcmp (geom_obj->class, "GPT") ||
                  ! strcmp (geom_obj->class, "APPLE"))
                geom_obj->str_type = g_strdup(fields[10]);
	      else if (! strcmp (geom_obj->class, "PART"))
                {
		  geom_obj->str_type = g_strdup(fields[10]);
                  if (g_strv_length(fields) >= 15)
                    {
                      if (! strcmp(fields[13], "xt"))
                        {
                          geom_obj->type = atoi(fields[14]);
			  if (! strcmp(fields[11], "xs"))
                            {
                              g_free(geom_obj->class);
			      geom_obj->class = g_strdup(fields[12]);
			    }
			}
		    }
		}
	      else if (fields[10][0] == '!')
                {
                  char *nottype;

		  nottype = fields[10];
		  nottype++;
                  geom_obj->type = atoi(nottype);
		}
	      else
                geom_obj->type = atoi(fields[10]);
	    }
	}

      g_hash_table_insert (table, GUINT_TO_POINTER(hash), geom_obj);

      if (depth > curr_depth)
        {
          g_assert(child != NULL);
          parent = child;
        }
      else if (depth < curr_depth)
        {
          GNode *cur;
          int levels;

          levels = curr_depth - depth;
          for (cur = child; cur && levels > 0; cur = cur->parent, levels--)
            ;

          if (cur == NULL)
            parent = root;
          else
            parent = cur->parent;
        }

      child = g_node_append_data(parent, GUINT_TO_POINTER(hash));
      curr_depth = depth;

      if (! strcmp(geom_obj->class, "DISK"))
	{
	  if (disk)
	    disk->conf = g_string_free(disk_conf, FALSE);

	  disk = g_new(Disk, 1);
	  disk->name = g_strdup(geom_obj->dev);
	  disk_conf = g_string_new(NULL);
	  disks = g_slist_prepend(disks, disk);
	}
      g_strfreev(fields);

      if (disk)
	{
	  if (*disk_conf->str)
	    g_string_append_c(disk_conf, '\n');
	  g_string_append(disk_conf, lines[i]);
	}
    }
  g_strfreev(lines);

  if (disk)
    disk->conf = g_string_free(disk_conf, FALSE);

  if (hf_storage_geom_hash)
    g_hash_table_destroy(hf_storage_geom_hash);

  hf_storage_geom_hash = table;

  if (hf_storage_geom_tree)
    g_node_destroy(hf_storage_geom_tree);

  hf_storage_geom_tree = root;

  return disks;
}

static Disk *
hf_storage_find_disk (const GSList *disks, const char *name)
{
  const GSList *l;

  HF_LIST_FOREACH(l, disks)
    {
      Disk *disk = l->data;

      if (! strcmp(disk->name, name))
	return disk;
    }

  return NULL;
}

static void
hf_storage_disk_free (Disk *disk)
{
  g_return_if_fail(disk != NULL);

  g_free(disk->name);
  g_free(disk->conf);
  g_free(disk);
}

static void
hf_storage_device_rescan_real (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  /* remove all children */

  hf_device_remove_children(device);

  /* rescan */

  hf_storage_device_probe(device, TRUE);
}

static gboolean
hf_storage_devd_notify (const char *system,
		        const char *subsystem,
			const char *type,
			const char *data)
{
  static GSList *disks = NULL;
  static gboolean first = TRUE;
  gboolean handled = FALSE;
  char *conftxt;
  GSList *new_disks;

  if (strcmp(system, "DEVFS") || strcmp(subsystem, "CDEV") ||
      (strcmp(type, "CREATE") && strcmp(type, "DESTROY")))
    return FALSE;

  conftxt = hf_get_string_sysctl(NULL, "kern.geom.conftxt");
  new_disks = hf_storage_parse_conftxt(conftxt);
  g_free(conftxt);

  /* if this is not the initial check, handle changes */

  if (first)
    first = FALSE;
  else
    {
      GSList *l;

      /* check for new disks */

      HF_LIST_FOREACH(l, new_disks)
	{
	  Disk *disk = l->data;

	  if (! hf_storage_find_disk(disks, disk->name))
	    {
	      osspec_probe();	/* catch new disk(s) */
	      handled = TRUE;
	      break;
	    }
	}

      /* check for disks which have changed or have been removed */

      HF_LIST_FOREACH(l, disks)
	{
	  Disk *disk = l->data;
	  Disk *new_disk;
	  HalDevice *device;

	  new_disk = hf_storage_find_disk(new_disks, disk->name);
	  if (new_disk)
	    {
	      if (strcmp(disk->conf, new_disk->conf))
		{
		  /* disk changed */
		  device = hf_devtree_find_from_name(hald_get_gdl(), disk->name);
		  if (device && hal_device_has_capability(device, "storage") &&
		      ! hf_storage_device_has_addon(device))
		    {
		      hf_storage_device_rescan_real(device);
		      handled = TRUE;
		    }
		}
	    }
	  else
	    {
	      /* disk removed */
	      device = hf_devtree_find_from_name(hald_get_gdl(), disk->name);
	      if (device && hal_device_has_capability(device, "storage"))
		{
	          hf_device_remove_tree(device);
		  handled = TRUE;
		}
	    }
	}
    }

  g_slist_foreach(disks, (GFunc) hf_storage_disk_free, NULL);
  g_slist_free(disks);
  disks = new_disks;

  return handled;
}

#if __FreeBSD_version < 700110
static gboolean
hf_storage_conftxt_timeout_cb (gpointer data)
{
  if (hf_is_waiting)
    return TRUE;

  hf_storage_devd_notify("DEVFS", "CDEV", "CREATE", NULL);

  return TRUE;
}
#endif

static void
hf_storage_init_geom (gboolean force)
{
  char *conftxt;
  static gboolean inited = FALSE;
  GSList *disks;

  if (inited && ! force)
    return;

  conftxt = hf_get_string_sysctl(NULL, "kern.geom.conftxt");
  disks = hf_storage_parse_conftxt(conftxt);
  g_free(conftxt);

  g_slist_foreach(disks, (GFunc) hf_storage_disk_free, NULL);
  g_slist_free(disks);

  inited = TRUE;
}

static void
hf_storage_init (void)
{
  hf_storage_init_geom(FALSE);
#if __FreeBSD_version < 700110
  g_timeout_add(3000, hf_storage_conftxt_timeout_cb, NULL);
#endif
}

void
hf_storage_device_enable (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hal_device_property_set_string(device, "storage.bus", "platform");
  hal_device_property_set_string(device, "storage.drive_type", "disk");

  hal_device_property_set_bool(device, "storage.removable", FALSE);
  hal_device_property_set_bool(device, "storage.requires_eject", FALSE);
  hal_device_property_set_bool(device, "storage.hotpluggable", FALSE);
  hal_device_property_set_bool(device, "storage.media_check_enabled", FALSE);
  hal_device_property_set_bool(device, "storage.automount_enabled_hint", TRUE);
  hal_device_property_set_bool(device, "storage.no_partitions_hint", FALSE);
  hal_device_property_set_bool(device, "storage.removable.support_async_notification", FALSE);

  hal_device_property_set_string(device, "storage.originating_device", NULL);
  hal_device_property_set_string(device, "storage.model", NULL);
  hal_device_property_set_string(device, "storage.vendor", NULL);

  hal_device_add_capability(device, "storage");
  hal_device_property_set_string(device, "info.category", "storage");
}

void
hf_storage_device_enable_tape (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hal_device_property_set_string(device, "storage.drive_type", "tape");
  hal_device_property_set_bool(device, "storage.removable", TRUE);
}

void
hf_storage_device_enable_cdrom (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hal_device_add_capability(device, "storage.cdrom");
  hal_device_property_set_string(device, "info.category", "storage.cdrom");
  hal_device_property_set_string(device, "storage.drive_type", "cdrom");
  hal_device_property_set_bool(device, "storage.removable", TRUE);
  /* enable media checks */
  hal_device_property_set_bool(device, "storage.media_check_enabled", TRUE);
  /* CD-ROM discs most likely don't have a partition table */
  hal_device_property_set_bool(device, "storage.no_partitions_hint", TRUE);
  /* the linux backend sets this one */
  hal_device_property_set_bool(device, "storage.requires_eject", TRUE);
  /* allow the storage addon to watch for media changes */
  hal_device_property_set_bool(device, "storage.removable.support_async_notification", FALSE);

  /* some of these will be set by probe-storage */
  hal_device_property_set_bool(device, "storage.cdrom.cdr", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.cdrw", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvd", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdr", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdrw", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdram", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdplusr", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdplusrw", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdplusrdl", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.dvdplusrwdl", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.bd", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.bdr", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.bdre", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.hddvd", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.hddvdr", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.hddvdrw", FALSE);
  hal_device_property_set_bool(device, "storage.cdrom.support_media_changed", FALSE);
  hal_device_property_set_int(device, "storage.cdrom.read_speed", 0);
  hal_device_property_set_int(device, "storage.cdrom.write_speed", 0);
}

/* preprobe, probe and add */
void
hf_storage_device_add (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  if (hf_device_preprobe(device))
    {
      hf_storage_device_probe(device, FALSE);
      hf_device_add(device);
    }
}

GSList *
hf_storage_get_geoms (const char *devname)
{
  GNode *node;
  GSList *geom_list = NULL;
  int n_children, i;
  guint hash;

  g_return_val_if_fail(devname != NULL, NULL);

  hf_storage_init_geom(FALSE);

  hash = g_str_hash(devname);
  node = g_node_find(hf_storage_geom_tree, G_PRE_ORDER, G_TRAVERSE_ALL,
                     GUINT_TO_POINTER(hash));
  if (! node)
    return NULL;

  n_children = g_node_n_children(node);
  for (i = 0; i < n_children; i++)
    {
      GNode *child;
      Geom_Object *geom_obj;

      child = g_node_nth_child(node, i);
      geom_obj = (Geom_Object *) g_hash_table_lookup(hf_storage_geom_hash,
                                                     child->data);
      if (geom_obj)
        geom_list = g_slist_prepend(geom_list, g_strdup(geom_obj->dev));
    }

  return geom_list;
}

static gboolean
hf_storage_device_rescan (HalDevice *device)
{
  if (hal_device_has_capability(device, "storage"))
    {
      hf_storage_device_rescan_real(device);
      return TRUE;
    }
  else
    return FALSE;
}

static gboolean
hf_storage_device_has_addon (HalDevice *device)
{
  HalDeviceStrListIter iter;

  g_return_val_if_fail(device != NULL, FALSE);

  for (hal_device_property_strlist_iter_init(device, "info.addons", &iter);
       hal_device_property_strlist_iter_is_valid(&iter);
       hal_device_property_strlist_iter_next(&iter))
    {
      const char *addon;

      addon = hal_device_property_strlist_iter_get_value(&iter);

      if (! strcmp(addon, "hald-addon-storage"))
        return TRUE;
    }

  return FALSE;
}

HFHandler hf_storage_handler = {
  .init =		hf_storage_init,
  .probe =		hf_storage_probe,
  .device_rescan =	hf_storage_device_rescan
};

HFDevdHandler hf_storage_devd_handler = {
  .notify =	hf_storage_devd_notify
};
