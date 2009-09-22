/***************************************************************************
 * CVSID: $Id$
 *
 * probe-volume.c : volume prober
 *
 * Copyright (C) 2006, 2007 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <sys/cdio.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#include <libufs.h>
#include <isofs/cd9660/iso.h>
#include <glib.h>
#include <libvolume_id.h>

#include "libhal/libhal.h"

#include "../libprobe/hfp.h"

#include "freebsd_dvd_rw_utils.h"

#define ISO_VOLDESC_SEC        16

struct iso_path_table_entry
{
  char name_len               [ISODCL(1,1)];
  char ext_attr_len           [ISODCL(2,2)];
  char extent                 [ISODCL(3,6)];
  char parent_no              [ISODCL(7,8)];
  char name[1];
};
#define ISO_PATH_TABLE_ENTRY_SIZE         8

#if (__FreeBSD_version < 600101) && (__FreeBSD_kernel_version < 600101)
static uint32_t
isonum_731(unsigned char *p)
{
  return (p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24);
}

static uint32_t
isonum_732(unsigned char *p)
{
  return (p[3] | p[2] << 8 | p[1] << 16 | p[0] << 24);
}
#endif

static uintmax_t
hf_probe_volume_getenv_uintmax (const char *name)
{
  char *str;

  g_return_val_if_fail(name != NULL, 0);

  str = getenv(name);

  return str ? strtoumax(str, NULL, 10) : 0;
}

static int
hf_probe_volume_getenv_int (const char *name)
{
  char *str;

  g_return_val_if_fail(name != NULL, 0);

  str = getenv(name);

  return str ? atoi(str) : 0;
}

static char *
hf_probe_volume_get_label (const struct volume_id *vid)
{
  char *label = NULL;

  if (vid && *vid->label)
    {
      if (g_utf8_validate(vid->label, -1, NULL))
	label = g_strdup(vid->label);
      else				/* assume ISO8859-1 */
	label = g_convert(vid->label, -1, "UTF-8", "ISO8859-1", NULL, NULL, NULL);
    }

  return label;
}

static void
hf_probe_volume_get_disc_info (int fd,
			       gboolean *has_audio,
			       gboolean *has_data)
{
  struct ioc_toc_header toc_header;
  int n_tracks;
  struct cd_toc_entry *buffer = NULL;
  struct ioc_read_toc_entry read_toc_entry;
  int i;

  g_return_if_fail(has_audio != NULL);
  g_return_if_fail(has_data != NULL);

  *has_audio = FALSE;
  *has_data = FALSE;

  if (ioctl(fd, CDIOREADTOCHEADER, &toc_header) < 0)
    return;

  n_tracks = toc_header.ending_track - toc_header.starting_track + 1;

  buffer = g_new(struct cd_toc_entry, n_tracks + 1);

  read_toc_entry.address_format = CD_MSF_FORMAT;
  read_toc_entry.starting_track = 0;
  read_toc_entry.data_len = (n_tracks + 1) * sizeof(struct cd_toc_entry);
  read_toc_entry.data = buffer;

  if (ioctl(fd, CDIOREADTOCENTRYS, &read_toc_entry) < 0)
    goto end;

  for (i = 0; i < n_tracks; i++)
    {
      if ((buffer[i].control & 4) != 0)
	*has_data = TRUE;
      else
	*has_audio = TRUE;
    }

 end:
  g_free(buffer);
}

static void
hf_probe_volume_advanced_disc_detect (int fd)
{
  size_t secsz = ISO_DEFAULT_BLOCK_SIZE;
  int readoff;
  struct iso_primary_descriptor desc;
  u_char *ptbl;
  int ptbl_size, ptbl_bsize;
  int ptbl_lbn;
  struct iso_path_table_entry *pte;
  int pte_len;
  int name_len;
  int off;

  readoff = ISO_VOLDESC_SEC * secsz;
  do
    {
      if (pread(fd, &desc, sizeof desc, readoff) != sizeof desc)
        {
          hfp_warning("volume descriptor read error");
	  return;
	}
      if (isonum_711(desc.type) == ISO_VD_END)
        return;
      readoff += secsz;
    }
  while (isonum_711(desc.type) != ISO_VD_PRIMARY);

  ptbl_size = isonum_733(desc.path_table_size);
#if BYTE_ORDER == LITTLE_ENDIAN
  ptbl_lbn = isonum_731(desc.type_l_path_table);
#else
  ptbl_lbn = isonum_732(desc.type_m_path_table);
#endif
  ptbl_bsize = (ptbl_size + secsz - 1) / secsz * secsz;
  ptbl = g_malloc(ptbl_bsize);
  if (ptbl == NULL)
    {
      hfp_warning("path table allocation failure");
      return;
    }
  readoff = ptbl_lbn * secsz;
  if (pread(fd, ptbl, ptbl_bsize, readoff) != ptbl_bsize)
    {
      g_free(ptbl);
      hfp_warning("path table read error");
    }
  for (off = 0; off < ptbl_size; off += pte_len)
    {
      pte = (struct iso_path_table_entry *)(ptbl + off);
      name_len = *pte->name_len;
      pte_len = ISO_PATH_TABLE_ENTRY_SIZE + name_len + (name_len % 2);
      if (*(short *)pte->parent_no != 1)
        continue;
      if (name_len == 8 && strncmp(pte->name, "VIDEO_TS", 8) == 0)
        {
          libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_videodvd", TRUE, &hfp_error);
	  break;
	}
      else if (name_len == 3 && strncmp(pte->name, "VCD", 8) == 0)
        {
          libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_vcd", TRUE, &hfp_error);
	  break;
	}
      else if (name_len == 4 && strncmp(pte->name, "SVCD", 4) == 0)
        {
          libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_svcd", TRUE, &hfp_error);
	  break;
	}
    }
  g_free(ptbl);
}

static gboolean
hf_probe_volume_get_partition_info (const char *geom_class,
				    const char *devfile,
				    int *number,
				    char **type,
				    char **scheme,
				    guint64 *mediasize,
				    guint64 *offset)
{
  g_return_val_if_fail(geom_class != NULL, FALSE);
  g_return_val_if_fail(devfile != NULL, FALSE);
  g_return_val_if_fail(number != NULL, FALSE);
  g_return_val_if_fail(type != NULL, FALSE);
  g_return_val_if_fail(scheme != NULL, FALSE);
  g_return_val_if_fail(mediasize != NULL, FALSE);
  g_return_val_if_fail(offset != NULL, FALSE);

  if (strcmp(geom_class, "MBR") &&
      strcmp(geom_class, "MBREXT") &&
      strcmp(geom_class, "GPT") &&
      strcmp(geom_class, "SUN") &&
      strcmp(geom_class, "APPLE"))
    return FALSE;

  *mediasize = hf_probe_volume_getenv_uintmax("HF_VOLUME_SIZE");
  if (*mediasize == 0)
    return FALSE;

  *offset = hf_probe_volume_getenv_uintmax("HF_VOLUME_OFFSET");

  *number = hf_probe_volume_getenv_int("HF_VOLUME_PART_INDEX");
  if (*number == 0)
    {
      size_t len;
      char *partno;

      partno = strrchr(devfile, 's');
      if (! partno)
        return FALSE;

      len = strlen(partno) - 1;
      if (len > 0 && strspn(partno + 1, "0123456789") == len)
        *number = atoi(partno);
      else
        return FALSE;
    }

  *scheme = g_ascii_strdown(geom_class, -1);
  if (! strcmp(*scheme, "apple"))
    {
      g_free(*scheme);
      *scheme = g_strdup("apm");
    }

  if (! strcmp(*scheme, "mbrext"))
    {
      g_free(*scheme);
      *scheme = g_strdup("embr");
    }

  if (! strcmp(*scheme, "mbr") || ! strcmp(*scheme, "embr"))
    *type = g_strdup_printf("0x%x",
                            hf_probe_volume_getenv_int("HF_VOLUME_PART_TYPE"));
  else
    {
      char *parttype;

      parttype = getenv("HF_VOLUME_PART_TYPE");

      if (parttype)
        *type = g_strdup(parttype);
      else
        *type = g_strdup("");
    }

  return TRUE;
}

int
main (int argc, char **argv)
{
  char *device_file;
  char *parent_udi;
  char *grandparent_udi;
  char *parent_drive_type;
  int fd = -1;
  struct volume_id *vid = NULL;
  int ret = 1;
  gboolean has_children;
  gboolean is_swap;
  gboolean is_cdrom;
  gboolean is_partition = FALSE;
  gboolean has_audio = FALSE;
  gboolean has_data = FALSE;
  gboolean is_blank = FALSE;
  const char *usage;
  char *label;
  unsigned int sector_size = 0;
  off_t media_size = 0;

  if (! hfp_init(argc, argv))
    goto end;

  device_file = getenv("HAL_PROP_BLOCK_DEVICE");
  if (! device_file)
    goto end;

  parent_udi = getenv("HAL_PROP_INFO_PARENT");
  if (! parent_udi)
    goto end;

  /* give a meaningful process title for ps(1) */
  setproctitle("%s", device_file);

  has_children = hfp_getenv_bool("HF_HAS_CHILDREN");
  is_swap = hfp_getenv_bool("HF_IS_SWAP");

  fd = open(device_file, O_RDONLY);
  if (fd < 0)
    goto end;

  parent_drive_type = libhal_device_get_property_string(hfp_ctx, parent_udi, "storage.drive_type", &hfp_error);
  dbus_error_free(&hfp_error);

  grandparent_udi = libhal_device_get_property_string(hfp_ctx, parent_udi, "info.parent", &hfp_error);
  dbus_error_free(&hfp_error);

  is_cdrom = parent_drive_type && ! strcmp(parent_drive_type, "cdrom");
  g_free(parent_drive_type);

  if (is_cdrom)
    {
      hf_probe_volume_get_disc_info(fd, &has_audio, &has_data);
      is_blank = (! has_audio && ! has_data);
    }

  ioctl(fd, DIOCGMEDIASIZE, &media_size);

  /*
   * We only check for filesystems if the volume has no children,
   * otherwise volume_id might find a filesystem in what is actually
   * the first child partition of the volume.
   *
   * If hald (which has looked at the partition type) reports that it
   * is a swap partition, we probe it nevertheless in case the
   * partition type is incorrect.
   */
  if (! has_children && ! (is_cdrom && ! has_data))
    {
      vid = volume_id_open_fd(fd);
      if (vid)
	{
	  if (volume_id_probe_all(vid, 0, media_size) == 0)
	    has_data = TRUE;
	  else
	    {
	      volume_id_close(vid);
	      vid = NULL;
	    }
	}
    }

  if (! has_children && ! is_swap && ! has_audio && ! has_data && ! is_blank)
    goto end;

  libhal_device_add_capability(hfp_ctx, hfp_udi, "volume", &hfp_error);
  if (is_cdrom)
    {
      HFPCDROM *cdrom;
      int type;
      guint64 capacity;

      libhal_device_set_property_string(hfp_ctx, hfp_udi, "info.category", "volume.disc", &hfp_error);
      libhal_device_add_capability(hfp_ctx, hfp_udi, "volume.disc", &hfp_error);

      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.has_audio", has_audio, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.has_data", has_data, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_vcd", FALSE, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_svcd", FALSE, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_videodvd", FALSE, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_appendable", FALSE, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_blank", is_blank, &hfp_error);
      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", FALSE, &hfp_error);
      libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "unknown", &hfp_error);

      /* the following code was adapted from linux's probe-volume.c */

      cdrom = hfp_cdrom_new_from_fd(fd, device_file, grandparent_udi);
      if (cdrom)
	{
	  type = get_disc_type(cdrom);
	  if (type != -1)
	    switch (type)
	      {
	      case 0x08: /* CD-ROM */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "cd_rom", &hfp_error);
		break;
	      case 0x09: /* CD-R */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "cd_r", &hfp_error);
		break;
	      case 0x0a: /* CD-RW */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "cd_rw", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      case 0x10: /* DVD-ROM */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_rom", &hfp_error);
		break;
	      case 0x11: /* DVD-R Sequential */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_r", &hfp_error);
		break;
	      case 0x12: /* DVD-RAM */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_ram", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      case 0x13: /* DVD-RW Restricted Overwrite */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_rw", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      case 0x14: /* DVD-RW Sequential */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_rw", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      case 0x1A: /* DVD+RW */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_plus_rw", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      case 0x1B: /* DVD+R */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_plus_r", &hfp_error);
		break;
	      case 0x2B: /* DVD+R Double Layer */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "dvd_plus_r_dl", &hfp_error);
		break;
	      case 0x40: /* BD-ROM  */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "bd_rom", &hfp_error);
		break;
	      case 0x41: /* BD-R Sequential */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "bd_r", &hfp_error);
		break;
	      case 0x42: /* BD-R Random */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "bd_r", &hfp_error);
		break;
	      case 0x43: /* BD-RE */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "bd_re", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      case 0x50: /* HD DVD-ROM */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "hddvd_rom", &hfp_error);
		break;
	      case 0x51: /* HD DVD-R */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "hddvd_r", &hfp_error);
		break;
	      case 0x52: /* HD DVD-Rewritable */
		libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.disc.type", "hddvd_rw", &hfp_error);
		libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
		break;
	      }

	  if (get_disc_capacity_for_type(cdrom, type, &capacity) == 0)
	    libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "volume.disc.capacity", capacity, &hfp_error);

	  /*
	   * linux's probe-volume.c: "on some hardware the get_disc_type
	   * call fails, so we use this as a backup".
	   */
	  if (disc_is_rewritable(cdrom))
	    libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.disc.is_rewritable", TRUE, &hfp_error);
	  if (disc_is_appendable(cdrom))
	    libhal_device_set_property_bool (hfp_ctx, hfp_udi, "volume.disc.is_appendable", TRUE, &hfp_error);

	  hfp_cdrom_free(cdrom);
	}

      if (has_data && vid && (! strcmp(vid->type, "iso9660") ||
          ! strcmp(vid->type, "udf")))
        hf_probe_volume_advanced_disc_detect(fd);
    }
  else
    {
      libhal_device_set_property_string(hfp_ctx, hfp_udi, "info.category", "volume", &hfp_error);

      if (libhal_device_query_capability(hfp_ctx, parent_udi, "storage", &hfp_error))
	{
	  char *geom_class;
	  char *type;
	  char *scheme;
	  int number;
	  guint64 mediasize;
	  guint64 offset;

	  geom_class = getenv("HF_VOLUME_GEOM_CLASS");

	  if (geom_class)
            {
              if (hf_probe_volume_get_partition_info(geom_class, device_file, &number, &type, &scheme, &mediasize, &offset))
                {
                  is_partition = TRUE;

		  libhal_device_set_property_int(hfp_ctx, hfp_udi, "volume.partition.number", number, &hfp_error);
		  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.partition.scheme", scheme, &hfp_error);
		  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.partition.type", type, &hfp_error);

		  /* FIXME We need to fill in the supported partition flags. */

		  libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "volume.partition.media_size", mediasize, &hfp_error);
		  libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "volume.partition.start", offset, &hfp_error);

		  if (! strcmp(scheme, "gpt"))
                    libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.partition.uuid", type, &hfp_error);

		  if (! strcmp(scheme, "gpt") || ! strcmp(scheme, "apm"))
                    libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.partition.label", "", &hfp_error);

		  g_free(type);
		  g_free(scheme);
		}
	    }
	}
      else
	dbus_error_free(&hfp_error);
    }

  libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.is_disc", is_cdrom, &hfp_error);
  libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.is_partition", is_partition, &hfp_error);

  libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.ignore", has_children || is_swap, &hfp_error);

  if (vid && ! strcmp (vid->type, "ufs"))
    {
      struct uufsd ufsdisk;

      if (ufs_disk_fillout(&ufsdisk, device_file) == 0)
        {
	  char ufsid[64];
	  char **ufs_devs = NULL;
	  int num_udis;
	  int i;

	  snprintf(ufsid, sizeof(ufsid), "%08x%08x", ufsdisk.d_fs.fs_id[0], ufsdisk.d_fs.fs_id[1]);
	  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.freebsd.ufsid", ufsid, &hfp_error);
	  ufs_devs = libhal_manager_find_device_string_match(hfp_ctx,
			  				     "volume.freebsd.ufsid",
							     ufsid,
							     &num_udis,
							     &hfp_error);
	  dbus_error_free(&hfp_error);
	  for (i = 0; i < num_udis; i++)
            {
              if (ufs_devs[i] != NULL)
                {
                  gboolean mounted;

		  mounted = libhal_device_get_property_bool(hfp_ctx, ufs_devs[i], "volume.is_mounted", &hfp_error);
		  dbus_error_free(&hfp_error);
		  if (mounted)
		    {
                      libhal_device_set_property_bool(hfp_ctx, hfp_udi, "volume.ignore", TRUE, &hfp_error);
		      dbus_error_free(&hfp_error);
		    }
		}
	    }
	  if (ufs_devs)
	    libhal_free_string_array(ufs_devs);
	  ufs_disk_close(&ufsdisk);
	}
    }

  if (has_children)
    usage = "partitiontable";
  else if (is_swap)
    usage = "other";
  else
    switch (vid ? vid->usage_id : (enum volume_id_usage) -1)
      {
      case VOLUME_ID_FILESYSTEM:	usage = "filesystem"; break;
      case VOLUME_ID_DISKLABEL:		usage = "disklabel"; break;
      case VOLUME_ID_OTHER:		usage = "other"; break;
      case VOLUME_ID_RAID:		usage = "raid"; break;
      case VOLUME_ID_CRYPTO:		usage = "crypto"; break;
      case VOLUME_ID_UNUSED:		usage = "unused"; break;
      default:				usage = "unknown"; break;
      }

  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.fsusage", usage, &hfp_error);
  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.fstype", vid ? vid->type: "", &hfp_error);
  if (vid && *vid->type_version)
    libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.fsversion", vid->type_version, &hfp_error);

  label = hf_probe_volume_get_label(vid);
  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.label", label ? label : "", &hfp_error);
  g_free(label);

  libhal_device_set_property_string(hfp_ctx, hfp_udi, "volume.uuid", vid ? vid->uuid : "", &hfp_error);

  ioctl(fd, DIOCGSECTORSIZE, &sector_size);

  if (sector_size != 0)
    libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "volume.block_size", sector_size, &hfp_error);
  if (media_size != 0)
    libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "volume.size", media_size, &hfp_error);
  if (sector_size != 0 && media_size != 0)
    libhal_device_set_property_uint64(hfp_ctx, hfp_udi, "volume.num_blocks", media_size / sector_size, &hfp_error);

  ret = 0;			/* is a volume */

 end:
  return ret;
}
