/***************************************************************************
 * CVSID: $Id$
 *
 * libhal-storage.c : HAL convenience library for storage devices and volumes
 *
 * Copyright (C) 2004 Red Hat, Inc.
 *
 * Author: David Zeuthen <davidz@redhat.com>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307	 USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

#include "../libhal/libhal.h"
#include "libhal-storage.h"


#ifdef ENABLE_NLS
# include <libintl.h>
# define _(String) dgettext (GETTEXT_PACKAGE, String)
# ifdef gettext_noop
#   define N_(String) gettext_noop (String)
# else
#   define N_(String) (String)
# endif
#else
/* Stubs that do something close enough.  */
# define textdomain(String) (String)
# define gettext(String) (String)
# define dgettext(Domain,Message) (Message)
# define dcgettext(Domain,Message,Type) (Message)
# define bindtextdomain(Domain,Directory) (Domain)
# define _(String)
# define N_(String) (String)
#endif

/**
 * @defgroup LibHalStorage HAL Storage and Volumes library
 * @brief HAL Storage and Volumes library
 *
 *  @{
 */

typedef struct IconMappingEntry_s {
	HalStoragePolicyIcon icon;
	char *path;
	struct IconMappingEntry_s *next;
} IconMappingEntry;

struct HalStoragePolicy_s {
	IconMappingEntry *icon_mappings;
};

HalStoragePolicy *
hal_storage_policy_new ()
{
	HalStoragePolicy *p;

	p = malloc (sizeof (HalStoragePolicy));
	if (p == NULL)
		goto out;

	p->icon_mappings = NULL;
out:
	return p;
}

void
hal_storage_policy_free (HalStoragePolicy *policy)
{
	IconMappingEntry *i;
	IconMappingEntry *j;

	/* free all icon mappings */
	for (i = policy->icon_mappings; i != NULL; i = j) {
		j = i->next;
		free (i->path);
		free (i);
	}

	free (policy);
}

void
hal_storage_policy_set_icon_path (HalStoragePolicy *policy, HalStoragePolicyIcon icon, const char *path)
{
	IconMappingEntry *i;

	/* see if it already exist */
	for (i = policy->icon_mappings; i != NULL; i = i->next) {
		if (i->icon == icon) {
			free (i->path);
			i->path = strdup (path);
			goto out;
		}
	}

	i = malloc (sizeof (IconMappingEntry));
	if (i == NULL)
		goto out;
	i->icon = icon;
	i->path = strdup (path);
	i->next = policy->icon_mappings;
	policy->icon_mappings = i;

out:
	return;
}

void
hal_storage_policy_set_icon_mapping (HalStoragePolicy *policy, HalStoragePolicyIconPair *pairs)
{
	HalStoragePolicyIconPair *i;

	for (i = pairs; i->icon != 0x00; i++) {
		hal_storage_policy_set_icon_path (policy, i->icon, i->icon_path);
	}
}

const char *
hal_storage_policy_lookup_icon (HalStoragePolicy *policy, HalStoragePolicyIcon icon)
{
	IconMappingEntry *i;
	const char *path;

	printf ("looking up 0x%08x\n", icon);

	path = NULL;
	for (i = policy->icon_mappings; i != NULL; i = i->next) {
		if (i->icon == icon) {
			path = i->path;
			goto out;
		}
	}
out:
	return path;
}


#define MAX_STRING_SZ 256

char *
hal_volume_policy_compute_size_as_string (HalVolume *volume)
{
	dbus_uint64_t size;
	char *result;
	char* sizes_str[] = {"K", "M", "G", "T", NULL};
	dbus_uint64_t cur = 1000L;
	dbus_uint64_t base = 10L;
	dbus_uint64_t step = 10L*10L*10L;
	int cur_str = 0;
	char buf[MAX_STRING_SZ];

	result = NULL;

	size = hal_volume_get_size (volume);

	do {
		if (sizes_str[cur_str+1] == NULL || size < cur*step) {
			/* found the unit, display a comma number if result is a single digit */
			if (size < cur*base) {
				snprintf (buf, MAX_STRING_SZ, "%.01f%s", 
					  ((double)size)/((double)cur), sizes_str[cur_str]);
				result = strdup (buf);
			} else {
				snprintf (buf, MAX_STRING_SZ, "%lld%s", size / cur, sizes_str[cur_str]);
				result = strdup (buf);
				}
			goto out;
		}

		cur *= step;
		cur_str++;
	} while (1);

out:
	return result;
}

static void
fixup_string (char *s)
{
	/* TODO: first strip leading and trailing whitespace */
	/*g_strstrip (s);*/

	/* TODO: could do nice things on all-upper case strings */
}

/* volume may be NULL (e.g. if drive supports removable media) */
char *
hal_drive_policy_compute_display_name (HalDrive *drive, HalVolume *volume, HalStoragePolicy *policy)
{
	char *name;
	char *size_str;
	char *vendormodel_str;
	const char *model;
	const char *vendor;
	HalDriveType drive_type;
	dbus_bool_t drive_is_hotpluggable;
	dbus_bool_t drive_is_removable;
	HalDriveCdromCaps drive_cdrom_caps;
	char buf[MAX_STRING_SZ];

	model = hal_drive_get_model (drive);
	vendor = hal_drive_get_vendor (drive);
	drive_type = hal_drive_get_type (drive);
	drive_is_hotpluggable = hal_drive_is_hotpluggable (drive);
	drive_is_removable = hal_drive_uses_removable_media (drive);
	drive_cdrom_caps = hal_drive_get_cdrom_caps (drive);

	if (volume != NULL)
		size_str = hal_volume_policy_compute_size_as_string (volume);
	else
		size_str = NULL;

	if (vendor == NULL || strlen (vendor) == 0) {
		if (model == NULL || strlen (model) == 0)
			vendormodel_str = strdup ("");
		else
			vendormodel_str = strdup (model);
	} else {
		if (model == NULL || strlen (model) == 0)
			vendormodel_str = strdup (vendor);
		else {
			snprintf (buf, MAX_STRING_SZ, "%s %s", vendor, model);
			vendormodel_str = strdup (buf);
		}
	}

	fixup_string (vendormodel_str);

	if (drive_type==HAL_DRIVE_TYPE_CDROM) {

		/* Optical drive handling */
		char *first;
		char *second;


		first = "CD-ROM";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_CDROM)
			first = "CD-R";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_CDRW)
			first = "CD-RW";

		second = "";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDROM)
			second = "/DVD-ROM";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDPLUSR)
			second = "/DVD+R";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDPLUSRW)
			second = "/DVD+RW";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDR)
			second = "/DVD-R";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDRW)
			second = "/DVD-RW";
		if (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDRAM)
			second = "/DVD-RAM";
		if ((drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDR) &&
		    (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDPLUSR))
			second = "/DVD±R";
		if ((drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDRW) &&
		    (drive_cdrom_caps & HAL_DRIVE_CDROM_CAPS_DVDPLUSRW))
			second = "/DVD±RW";


		if (drive_is_hotpluggable) {
			snprintf (buf, MAX_STRING_SZ, _("External %s%s Drive"), first, second);
			name = strdup (buf);
		} else {
			snprintf (buf, MAX_STRING_SZ, _("%s%s Drive"), first, second);
			name = strdup (buf);
		}
			
	} else if (drive_type==HAL_DRIVE_TYPE_FLOPPY) {

		/* Floppy Drive handling */

		if (drive_is_hotpluggable)
			name = strdup (_("External Floppy Drive"));
		else
			name = strdup (_("Floppy Drive"));
	} else if (drive_type==HAL_DRIVE_TYPE_DISK && !drive_is_removable) {

		/* Harddisks */

		if (size_str != NULL) {
			if (drive_is_hotpluggable) {
				snprintf (buf, MAX_STRING_SZ, _("%s External Hard Drive"), size_str);
				name = strdup (buf);
			} else {
				snprintf (buf, MAX_STRING_SZ, _("%s Hard Drive"), size_str);
				name = strdup (buf);
			}
		} else {
			if (drive_is_hotpluggable)
				name = strdup (_("External Hard Drive"));
			else
				name = strdup (_("Hard Drive"));
		}
	} else {

		/* The rest - includes drives with removable Media */

		if (strlen (vendormodel_str) > 0)
			name = strdup (vendormodel_str);
		else
			name = strdup (_("Drive"));
	}

	free (vendormodel_str);
	free (size_str);

	return name;
}

char *
hal_volume_policy_compute_display_name (HalDrive *drive, HalVolume *volume, HalStoragePolicy *policy)
{
	char *name;
	char *size_str;
	const char *volume_label;
	const char *model;
	const char *vendor;
	HalDriveType drive_type;
	dbus_bool_t drive_is_hotpluggable;
	dbus_bool_t drive_is_removable;
	HalDriveCdromCaps drive_cdrom_caps;
	char buf[MAX_STRING_SZ];

	volume_label = hal_volume_get_label (volume);
	model = hal_drive_get_model (drive);
	vendor = hal_drive_get_vendor (drive);
	drive_type = hal_drive_get_type (drive);
	drive_is_hotpluggable = hal_drive_is_hotpluggable (drive);
	drive_is_removable = hal_drive_uses_removable_media (drive);
	drive_cdrom_caps = hal_drive_get_cdrom_caps (drive);

	size_str = hal_volume_policy_compute_size_as_string (volume);

	/* If the volume label is available use that 
	 *
	 * TODO: If label is a fully-qualified UNIX path don't use that
	 */
	if (volume_label != NULL) {
		name = strdup (volume_label);
		goto out;
	}

	/* Handle media in optical drives */
	if (drive_type==HAL_DRIVE_TYPE_CDROM) {
		switch (hal_volume_get_disc_type (volume)) {

		default:
			/* explict fallthrough */
		case HAL_VOLUME_DISC_TYPE_CDROM:
			name = strdup (_("CD-ROM Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_CDR:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank CD-R Disc"));
			else
				name = strdup (_("CD-R Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_CDRW:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank CD-RW Disc"));
			else
				name = strdup (_("CD-RW Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_DVDROM:
			name = strdup (_("DVD-ROM Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_DVDRAM:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank DVD-RAM Disc"));
			else
				name = strdup (_("DVD-RAM Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_DVDR:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank DVD-R Disc"));
			else
				name = strdup (_("DVD-R Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_DVDRW:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank DVD-RW Disc"));
			else
				name = strdup (_("DVD-RW Disc"));

			break;

		case HAL_VOLUME_DISC_TYPE_DVDPLUSR:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank DVD+R Disc"));
			else
				name = strdup (_("DVD+R Disc"));
			break;
			
		case HAL_VOLUME_DISC_TYPE_DVDPLUSRW:
			if (hal_volume_disc_is_blank (volume))
				name = strdup (_("Blank DVD+RW Disc"));
			else
				name = strdup (_("DVD+RW Disc"));
			break;
		}
		
		/* Special case for pure audio disc */
		if (hal_volume_disc_has_audio (volume) && !hal_volume_disc_has_data (volume)) {
			free (name);
			name = strdup (_("Audio Disc"));
		}

		goto out;
	}

	/* Fallback: size of media */
	if (drive_is_removable) {
		snprintf (buf, MAX_STRING_SZ, _("%s Removable Media"), size_str);
		name = strdup (buf);
	} else {
		snprintf (buf, MAX_STRING_SZ, _("%s Media"), size_str);
		name = strdup (buf);
	}

	/* Fallback: Use drive name */
	/*name = hal_drive_policy_compute_display_name (drive, volume);*/

out:
	free (size_str);
	return name;
}

char *
hal_drive_policy_compute_icon_name (HalDrive *drive, HalVolume *volume, HalStoragePolicy *policy)
{
	const char *name;
	HalDriveBus bus;
	HalDriveType drive_type;

	bus        = hal_drive_get_bus (drive);
	drive_type = hal_drive_get_type (drive);

	/* by design, the enums are laid out so we can do easy computations */

	switch (drive_type) {
	case HAL_DRIVE_TYPE_REMOVABLE_DISK:
	case HAL_DRIVE_TYPE_DISK:
	case HAL_DRIVE_TYPE_CDROM:
	case HAL_DRIVE_TYPE_FLOPPY:
		name = hal_storage_policy_lookup_icon (policy, 0x10000 + drive_type*0x100 + bus);
		break;

	default:
		name = hal_storage_policy_lookup_icon (policy, 0x10000 + drive_type*0x100);
	}

	if (name != NULL)
		return strdup (name);
	else
		return NULL;
}

char *
hal_volume_policy_compute_icon_name (HalDrive *drive, HalVolume *volume, HalStoragePolicy *policy)
{
	const char *name;
	HalDriveBus bus;
	HalDriveType drive_type;
	HalVolumeDiscType disc_type;

	/* by design, the enums are laid out so we can do easy computations */

	if (hal_volume_is_disc (volume)) {
		disc_type = hal_volume_get_disc_type (volume);
		name = hal_storage_policy_lookup_icon (policy, 0x30000 + disc_type);
		goto out;
	}

	if (drive == NULL) {
		name = hal_storage_policy_lookup_icon (policy, HAL_STORAGE_ICON_VOLUME_REMOVABLE_DISK);
		goto out;
	}

	bus        = hal_drive_get_bus (drive);
	drive_type = hal_drive_get_type (drive);

	switch (drive_type) {
	case HAL_DRIVE_TYPE_REMOVABLE_DISK:
	case HAL_DRIVE_TYPE_DISK:
	case HAL_DRIVE_TYPE_CDROM:
	case HAL_DRIVE_TYPE_FLOPPY:
		name = hal_storage_policy_lookup_icon (policy, 0x20000 + drive_type*0x100 + bus);
		break;

	default:
		name = hal_storage_policy_lookup_icon (policy, 0x20000 + drive_type*0x100);
	}
out:
	if (name != NULL)
		return strdup (name);
	else
		return NULL;
}

/** Policy function to determine if a volume should be visible in a desktop 
 *  environment. This is useful to hide certain system volumes as bootstrap
 *  partitions, the /usr partition, swap partitions and other volumes that
 *  a unprivileged desktop user shouldn't know even exists.
 *
 *  @param  drive               Drive that the volume is stemming from
 *  @param  volume              Volume
 *  @param  policy              Policy object
 *  @param  target_mount_point  The mount point that the volume is expected to
 *                              be mounted at if not already mounted. This may
 *                              e.g. stem from /etc/fstab. If this is NULL the
 *                              then mount point isn't taking into account when
 *                              evaluating whether the volume should be visible
 *  @return                     Whether the volume should be shown in a desktop
 *                              environment.
 */
dbus_bool_t
hal_volume_policy_should_be_visible (HalDrive *drive, HalVolume *volume, HalStoragePolicy *policy, 
				     const char *target_mount_point)
{
	unsigned int i;
	dbus_bool_t is_visible;
	const char *label;
	const char *mount_point;
	const char *fstype;
	const char *fhs23_toplevel_mount_points[] = {
		"/",
		"/bin",
		"/boot",
		"/dev",
		"/etc",
		"/home",
		"/lib",
		"/lib64",
		"/media",
		"/mnt",
		"/opt",
		"/root",
		"/sbin",
		"/srv",
		"/tmp",
		"/usr",
		"/var",
		"/proc",
		"/sbin",
		NULL
	};

	is_visible = FALSE;

	/* skip if hal says it's not used as a filesystem */
	if (hal_volume_get_fsusage (volume) != HAL_VOLUME_USAGE_MOUNTABLE_FILESYSTEM)
		goto out;

	label = hal_volume_get_label (volume);
	mount_point = hal_volume_get_mount_point (volume);
	fstype = hal_volume_get_fstype (volume);

	/* use target mount point if we're not mounted yet */
	if (mount_point == NULL)
		mount_point = target_mount_point;

	/* bail out if we don't know the filesystem */
	if (fstype == NULL)
		goto out;

	/* blacklist fhs2.3 top level mount points */
	if (mount_point != NULL) {
		for (i = 0; fhs23_toplevel_mount_points[i] != NULL; i++) {
			if (strcmp (mount_point, fhs23_toplevel_mount_points[i]) == 0)
				goto out;
		}
	}

	/* blacklist partitions with name 'bootstrap' of type HFS (Apple uses that) */
	if (label != NULL && strcmp (label, "bootstrap") == 0 && strcmp (fstype, "hfs") == 0)
		goto out;

	/* only the real lucky mount points will make it this far :-) */
	is_visible = TRUE;

out:
	return is_visible;
}

/*************************************************************************/



struct HalDrive_s {
	char *udi;

	int device_major;
	int device_minor;
	char *device_file;

	HalDriveBus bus;
	char *vendor;             /* may be "", is never NULL */
	char *model;              /* may be "", is never NULL */
	dbus_bool_t is_hotpluggable;
	dbus_bool_t is_removable;

	HalDriveType type;
	char *type_textual;

	char *physical_device;  /* UDI of physical device, e.g. the 
				 * IDE, USB, IEEE1394 device */

	char *serial;
	char *firmware_version;
	HalDriveCdromCaps cdrom_caps;
};

struct HalVolume_s {
	char *udi;

	int device_major;
	int device_minor;
	char *device_file;
	char *volume_label; /* may be NULL, is never "" */
	dbus_bool_t is_mounted;
	char *mount_point;  /* NULL iff !is_mounted */
	char *fstype;       /* NULL iff !is_mounted or unknown */
	char *fsversion;
	char *uuid;
	char *storage_device;

	HalVolumeUsage fsusage;

	dbus_bool_t is_partition;
	unsigned int partition_number;
	

	dbus_bool_t is_disc;
	HalVolumeDiscType disc_type;
	dbus_bool_t disc_has_audio;
	dbus_bool_t disc_has_data;
	dbus_bool_t disc_is_appendable;
	dbus_bool_t disc_is_blank;
	dbus_bool_t disc_is_rewritable;

	unsigned int block_size;
	unsigned int num_blocks;
};

/** Free all resources used by a HalDrive object.
 *
 *  @param  drive               Object to free
 */
void
hal_drive_free (HalDrive *drive)
{
	if (drive == NULL )
		return;

	free (drive->udi);
	hal_free_string (drive->device_file);
	hal_free_string (drive->vendor);
	hal_free_string (drive->model);
	hal_free_string (drive->type_textual);
	hal_free_string (drive->physical_device);
	hal_free_string (drive->serial);
	hal_free_string (drive->firmware_version);
}


/** Free all resources used by a HalVolume object.
 *
 *  @param  volume              Object to free
 */
void
hal_volume_free (HalVolume *vol)
{
	if (vol == NULL )
		return;

	free (vol->udi);
	hal_free_string (vol->device_file);
	hal_free_string (vol->volume_label);
	hal_free_string (vol->fstype);
	hal_free_string (vol->mount_point);
	hal_free_string (vol->fsversion);
	hal_free_string (vol->uuid);
}


/* ok, hey, so this is a bit ugly */

#define HAL_PROP_EXTRACT_BEGIN if (FALSE)
#define HAL_PROP_EXTRACT_END ;
#define HAL_PROP_EXTRACT_INT(_property_, _where_) else if (strcmp (key, _property_) == 0 && type == DBUS_TYPE_INT32) _where_ = hal_psi_get_int (&it)
#define HAL_PROP_EXTRACT_STRING(_property_, _where_) else if (strcmp (key, _property_) == 0 && type == DBUS_TYPE_STRING) _where_ = (hal_psi_get_string (&it) != NULL && strlen (hal_psi_get_string (&it)) > 0) ? strdup (hal_psi_get_string (&it)) : NULL
#define HAL_PROP_EXTRACT_BOOL(_property_, _where_) else if (strcmp (key, _property_) == 0 && type == DBUS_TYPE_BOOLEAN) _where_ = hal_psi_get_bool (&it)
#define HAL_PROP_EXTRACT_BOOL_BITFIELD(_property_, _where_, _field_) else if (strcmp (key, _property_) == 0 && type == DBUS_TYPE_BOOLEAN) _where_ |= hal_psi_get_bool (&it) ? _field_ : 0


/** Given a UDI for a HAL device of capability 'storage', this
 *  function retrieves all the relevant properties into convenient
 *  in-process data structures.
 *
 *  @param  hal_ctx             libhal context
 *  @param  udi                 HAL UDI
 *  @return                     HalDrive object or NULL if UDI is invalid
 */
HalDrive *
hal_drive_from_udi (LibHalContext *hal_ctx, const char *udi)
{
	char *bus_textual;
	HalDrive *drive;
	LibHalPropertySet *properties;
	LibHalPropertySetIterator it;

	drive = NULL;
	properties = NULL;
	bus_textual = NULL;

	if (!hal_device_query_capability (hal_ctx, udi, "storage"))
		goto error;

	drive = malloc (sizeof (HalDrive));
	if (drive == NULL)
		goto error;
	memset (drive, 0x00, sizeof (HalDrive));

	drive->udi = strdup (udi);
	if (drive->udi == NULL)
		goto error;

	properties = hal_device_get_all_properties (hal_ctx, udi);
	if (properties == NULL)
		goto error;

	/* we can count on hal to give us all these properties */
	for (hal_psi_init (&it, properties); hal_psi_has_more (&it); hal_psi_next (&it)) {
		int type;
		char *key;
		
		type = hal_psi_get_type (&it);
		key = hal_psi_get_key (&it);

		HAL_PROP_EXTRACT_BEGIN;

		HAL_PROP_EXTRACT_INT    ("block.minor",               drive->device_minor);
		HAL_PROP_EXTRACT_INT    ("block.major",               drive->device_major);
		HAL_PROP_EXTRACT_STRING ("block.device",              drive->device_file);
		HAL_PROP_EXTRACT_STRING ("storage.bus",               bus_textual);
		HAL_PROP_EXTRACT_STRING ("storage.vendor",            drive->vendor);
		HAL_PROP_EXTRACT_STRING ("storage.model",             drive->model);
		HAL_PROP_EXTRACT_STRING ("storage.drive_type",        drive->type_textual);

		HAL_PROP_EXTRACT_BOOL   ("storage.hotpluggable",      drive->is_hotpluggable);
		HAL_PROP_EXTRACT_BOOL   ("storage.removable",         drive->is_removable);

		HAL_PROP_EXTRACT_STRING ("storage.physical_device",   drive->physical_device);
		HAL_PROP_EXTRACT_STRING ("storage.firmware_version",  drive->firmware_version);
		HAL_PROP_EXTRACT_STRING ("storage.serial",            drive->serial);

		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.cdr", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_CDR);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.cdrw", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_CDRW);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.dvd", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_DVDROM);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.dvdplusr", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_DVDPLUSR);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.dvdplusrw", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_DVDPLUSRW);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.dvdr", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_DVDR);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.dvdrw", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_DVDRW);
		HAL_PROP_EXTRACT_BOOL_BITFIELD ("storage.cdrom.dvdram", drive->cdrom_caps, HAL_DRIVE_CDROM_CAPS_DVDRAM);

		HAL_PROP_EXTRACT_END;
	}

	if (drive->type_textual != NULL) {
		if (strcmp (drive->type_textual, "cdrom") == 0) {
			drive->cdrom_caps |= HAL_DRIVE_CDROM_CAPS_CDROM;
			drive->type = HAL_DRIVE_TYPE_CDROM;
		} else if (strcmp (drive->type_textual, "floppy") == 0) {
			drive->type = HAL_DRIVE_TYPE_FLOPPY;
		} else if (strcmp (drive->type_textual, "disk") == 0) {
			if (drive->is_removable)
				drive->type = HAL_DRIVE_TYPE_REMOVABLE_DISK;
			else
				drive->type = HAL_DRIVE_TYPE_DISK;				
		} else if (strcmp (drive->type_textual, "tape") == 0) {
			drive->type = HAL_DRIVE_TYPE_TAPE;
		} else if (strcmp (drive->type_textual, "compact_flash") == 0) {
			drive->type = HAL_DRIVE_TYPE_COMPACT_FLASH;
		} else if (strcmp (drive->type_textual, "memory_stick") == 0) {
			drive->type = HAL_DRIVE_TYPE_MEMORY_STICK;
		} else if (strcmp (drive->type_textual, "smart_media") == 0) {
			drive->type = HAL_DRIVE_TYPE_SMART_MEDIA;
		} else if (strcmp (drive->type_textual, "sd_mmc") == 0) {
			drive->type = HAL_DRIVE_TYPE_SD_MMC;
		}
	}

	/* check if physical device is a camera or mp3 player */
	if (drive->physical_device != NULL) {
		char *category;

		category = hal_device_get_property_string (hal_ctx, drive->physical_device, "info.category");
		if (category != NULL) {
			if (strcmp (category, "portable_audio_player") == 0) {
				drive->type = HAL_DRIVE_TYPE_PORTABLE_AUDIO_PLAYER;
			} else if (strcmp (category, "camera") == 0) {
				drive->type = HAL_DRIVE_TYPE_CAMERA;
			}

			hal_free_string (category);
		}
	}

	if (bus_textual != NULL) {
		if (strcmp (bus_textual, "usb") == 0) {
			drive->bus = HAL_DRIVE_BUS_USB;
		} else if (strcmp (bus_textual, "ieee1394") == 0) {
			drive->bus = HAL_DRIVE_BUS_IEEE1394;
		} else if (strcmp (bus_textual, "ide") == 0) {
			drive->bus = HAL_DRIVE_BUS_IDE;
		} else if (strcmp (bus_textual, "scsi") == 0) {
			drive->bus = HAL_DRIVE_BUS_SCSI;
		}
	}

	hal_free_string (bus_textual);
	hal_free_property_set (properties);

	return drive;

error:
	hal_free_string (bus_textual);
	hal_free_property_set (properties);
	hal_drive_free (drive);
	return NULL;
}

const char *
hal_volume_get_storage_device_udi (HalVolume *volume)
{
	return volume->storage_device;
}

const char *hal_drive_get_physical_device_udi (HalDrive *drive)
{
	return drive->physical_device;
}

/** Given a UDI for a HAL device of capability 'volume', this
 *  function retrieves all the relevant properties into convenient
 *  in-process data structures.
 *
 *  @param  hal_ctx             libhal context
 *  @param  udi                 HAL UDI
 *  @return                     HalVolume object or NULL if UDI is invalid
 */
HalVolume *
hal_volume_from_udi (LibHalContext *hal_ctx, const char *udi)
{
	char *disc_type_textual;
	HalVolume *vol;
	LibHalPropertySet *properties;
	LibHalPropertySetIterator it;

	vol = NULL;
	properties = NULL;
	disc_type_textual = NULL;

	if (!hal_device_query_capability (hal_ctx, udi, "volume"))
		goto error;

	vol = malloc (sizeof (HalVolume));
	if (vol == NULL)
		goto error;
	memset (vol, 0x00, sizeof (HalVolume));

	vol->udi = strdup (udi);

	properties = hal_device_get_all_properties (hal_ctx, udi);
	if (properties == NULL)
		goto error;

	/* we can count on hal to give us all these properties */
	for (hal_psi_init (&it, properties); hal_psi_has_more (&it); hal_psi_next (&it)) {
		int type;
		char *key;
		
		type = hal_psi_get_type (&it);
		key = hal_psi_get_key (&it);

		HAL_PROP_EXTRACT_BEGIN;

		HAL_PROP_EXTRACT_INT    ("block.minor",               vol->device_minor);
		HAL_PROP_EXTRACT_INT    ("block.major",               vol->device_major);
		HAL_PROP_EXTRACT_STRING ("block.device",              vol->device_file);

		HAL_PROP_EXTRACT_STRING ("block.storage_device",      vol->storage_device);

		HAL_PROP_EXTRACT_INT    ("volume.block_size",         vol->block_size);
		HAL_PROP_EXTRACT_INT    ("volume.num_blocks",         vol->num_blocks);
		HAL_PROP_EXTRACT_STRING ("volume.label",              vol->volume_label);
		HAL_PROP_EXTRACT_STRING ("volume.mount_point",        vol->mount_point);
		HAL_PROP_EXTRACT_STRING ("volume.fstype",             vol->fstype);
		HAL_PROP_EXTRACT_BOOL   ("volume.is_mounted",         vol->is_mounted);

		HAL_PROP_EXTRACT_BOOL   ("volume.is_disc",            vol->is_disc);
		HAL_PROP_EXTRACT_STRING ("volume.disc.type",          disc_type_textual);
		HAL_PROP_EXTRACT_BOOL   ("volume.disc.has_audio",     vol->disc_has_audio);
		HAL_PROP_EXTRACT_BOOL   ("volume.disc.has_data",      vol->disc_has_data);
		HAL_PROP_EXTRACT_BOOL   ("volume.disc.is_appendable", vol->disc_is_appendable);
		HAL_PROP_EXTRACT_BOOL   ("volume.disc.is_blank",      vol->disc_is_blank);
		HAL_PROP_EXTRACT_BOOL   ("volume.disc.is_rewritable", vol->disc_is_rewritable);

		HAL_PROP_EXTRACT_END;
	}

	if (disc_type_textual != NULL) {
		if (strcmp (disc_type_textual, "cd_rom") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_CDROM;
		} else if (strcmp (disc_type_textual, "cd_r") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_CDR;
		} else if (strcmp (disc_type_textual, "cd_rw") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_CDRW;
		} else if (strcmp (disc_type_textual, "dvd_rom") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_DVDROM;
		} else if (strcmp (disc_type_textual, "dvd_ram") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_DVDRAM;
		} else if (strcmp (disc_type_textual, "dvd_r") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_DVDR;
		} else if (strcmp (disc_type_textual, "dvd_rw") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_DVDRW;
		} else if (strcmp (disc_type_textual, "dvd_plusr") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_DVDPLUSR;
		} else if (strcmp (disc_type_textual, "dvd_plusrw") == 0) {
			vol->disc_type = HAL_VOLUME_DISC_TYPE_DVDPLUSRW;
		}
	}

	hal_free_string (disc_type_textual);
	hal_free_property_set (properties);
	return vol;
error:
	hal_free_string (disc_type_textual);
	hal_free_property_set (properties);
	hal_volume_free (vol);
	return NULL;
}

/***********************************************************************/

/** Get the drive object that either is (when given e.g. /dev/sdb) or contains
 *  (when given e.g. /dev/sdb1) the given device file.
 *
 *  @param  hal_ctx             libhal context to use
 *  @param  device_file         Name of special device file, e.g. '/dev/hdc'
 *  @return                     HalDrive object or NULL if it doesn't exist
 */
HalDrive *
hal_drive_from_device_file (LibHalContext *hal_ctx, const char *device_file)
{
	int i;
	char **hal_udis;
	int num_hal_udis;
	HalDrive *result;
	char *found_udi;

	result = NULL;
	found_udi = NULL;

	if ((hal_udis = hal_manager_find_device_string_match (hal_ctx, "block.device", 
							      device_file, &num_hal_udis)) == NULL)
		goto out;

	for (i = 0; i < num_hal_udis; i++) {
		char *udi;
		char *storage_udi;
		udi = hal_udis[i];
		if (hal_device_query_capability (hal_ctx, udi, "volume")) {

			storage_udi = hal_device_get_property_string (hal_ctx, udi, "block.storage_device");
			if (storage_udi == NULL)
				continue;
			found_udi = strdup (storage_udi);
			hal_free_string (storage_udi);
			break;
		} else if (hal_device_query_capability (hal_ctx, udi, "storage")) {
			found_udi = strdup (udi);
		}
	}

	hal_free_string_array (hal_udis);

	if (found_udi != NULL)
		result = hal_drive_from_udi (hal_ctx, found_udi);

	free (found_udi);
out:
	return result;
}


/** Get the volume object for a given device file.
 *
 *  @param  hal_ctx             libhal context to use
 *  @param  device_file         Name of special device file, e.g. '/dev/hda5'
 *  @return                     HalVolume object or NULL if it doesn't exist
 */
HalVolume *
hal_volume_from_device_file (LibHalContext *hal_ctx, const char *device_file)
{
	int i;
	char **hal_udis;
	int num_hal_udis;
	HalVolume *result;
	char *found_udi;

	result = NULL;
	found_udi = NULL;

	if ((hal_udis = hal_manager_find_device_string_match (hal_ctx, "block.device", 
							      device_file, &num_hal_udis)) == NULL)
		goto out;

	for (i = 0; i < num_hal_udis; i++) {
		char *udi;
		udi = hal_udis[i];
		if (hal_device_query_capability (hal_ctx, udi, "volume")) {
			found_udi = strdup (udi);
			break;
		}
	}

	hal_free_string_array (hal_udis);

	if (found_udi != NULL)
		result = hal_volume_from_udi (hal_ctx, found_udi);

	free (found_udi);
out:
	return result;
}

dbus_uint64_t
hal_volume_get_size (HalVolume *volume)
{
	return ((dbus_uint64_t)volume->block_size) * ((dbus_uint64_t)volume->num_blocks);
}


dbus_bool_t
hal_drive_is_hotpluggable (HalDrive *drive)
{
	return drive->is_hotpluggable;
}

dbus_bool_t
hal_drive_uses_removable_media (HalDrive *drive)
{
	return drive->is_removable;
}

HalDriveType
hal_drive_get_type (HalDrive *drive)
{
	return drive->type;
}

HalDriveBus
hal_drive_get_bus (HalDrive *drive)
{
	return drive->bus;
}

HalDriveCdromCaps
hal_drive_get_cdrom_caps (HalDrive *drive)
{
	return drive->cdrom_caps;
}

unsigned int
hal_drive_get_device_major (HalDrive *drive)
{
	return drive->device_major;
}

unsigned int
hal_drive_get_device_minor (HalDrive *drive)
{
	return drive->device_minor;
}

const char *
hal_drive_get_type_textual (HalDrive *drive)
{
	return drive->type_textual;
}

const char *
hal_drive_get_device_file (HalDrive *drive)
{
	return drive->device_file;
}

const char *
hal_drive_get_udi (HalDrive *drive)
{
	return drive->udi;
}

const char *
hal_drive_get_serial (HalDrive *drive)
{
	return drive->serial;
}

const char *
hal_drive_get_firmware_version (HalDrive *drive)
{
	return drive->firmware_version;
}

const char *
hal_drive_get_model (HalDrive *drive)
{
	return drive->model;
}

const char *
hal_drive_get_vendor (HalDrive *drive)
{
	return drive->vendor;
}

/*****************************************************************************/

const char *
hal_volume_get_udi (HalVolume *volume)
{
	return volume->udi;
}

const char *
hal_volume_get_device_file (HalVolume *volume)
{
	return volume->device_file;
}

unsigned int hal_volume_get_device_major (HalVolume *volume)
{
	return volume->device_major;
}

unsigned int hal_volume_get_device_minor (HalVolume *volume)
{
	return volume->device_minor;
}

const char *
hal_volume_get_fstype (HalVolume *volume)
{
	return volume->fstype;
}

const char *
hal_volume_get_fsversion (HalVolume *volume)
{
	return volume->fsversion;
}

HalVolumeUsage 
hal_volume_get_fsusage (HalVolume *volume)
{
	return volume->fsusage;
}

dbus_bool_t 
hal_volume_is_mounted (HalVolume *volume)
{
	return volume->is_mounted;
}

dbus_bool_t 
hal_volume_is_partition (HalVolume *volume)
{
	return volume->is_partition;
}

dbus_bool_t
hal_volume_is_disc (HalVolume *volume)
{
	return volume->is_disc;
}

unsigned int
hal_volume_get_partition_number (HalVolume *volume)
{
	return volume->partition_number;
}

const char *
hal_volume_get_label (HalVolume *volume)
{
	return volume->volume_label;
}

const char *
hal_volume_get_mount_point (HalVolume *volume)
{
	return volume->mount_point;
}

const char *
hal_volume_get_uuid (HalVolume *volume)
{
	return volume->uuid;
}

dbus_bool_t
hal_volume_disc_has_audio (HalVolume *volume)
{
	return volume->disc_has_audio;
}

dbus_bool_t
hal_volume_disc_has_data (HalVolume *volume)
{
	return volume->disc_has_data;
}

dbus_bool_t
hal_volume_disc_is_blank (HalVolume *volume)
{
	return volume->disc_is_blank;
}

dbus_bool_t
hal_volume_disc_is_rewritable (HalVolume *volume)
{
	return volume->disc_is_rewritable;
}

dbus_bool_t
hal_volume_disc_is_appendable (HalVolume *volume)
{
	return volume->disc_is_appendable;
}

HalVolumeDiscType
hal_volume_get_disc_type (HalVolume *volume)
{
	return volume->disc_type;
}

char ** 
hal_drive_find_all_volumes (LibHalContext *hal_ctx, HalDrive *drive, int *num_volumes)
{
	int i;
	char **udis;
	int num_udis;
	const char *drive_udi;
	char **result;

	udis = NULL;
	result = NULL;
	*num_volumes = 0;

	drive_udi = hal_drive_get_udi (drive);
	if (drive_udi == NULL)
		goto out;

	/* get initial list... */
	if ((udis = hal_manager_find_device_string_match (hal_ctx, "block.storage_device", 
							  drive_udi, &num_udis)) == NULL)
		goto out;

	result = malloc (sizeof (char *) * num_udis);
	if (result == NULL)
		goto out;

	/* ...and filter out the single UDI that is the drive itself */
	for (i = 0; i < num_udis; i++) {
		if (strcmp (udis[i], drive_udi) == 0)
			continue;
		result[*num_volumes] = strdup (udis[i]);
		*num_volumes = (*num_volumes) + 1;
	}

out:
	hal_free_string_array (udis);
	return result;
}


/** @} */
