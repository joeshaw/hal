/***************************************************************************
 * CVSID: $Id$
 *
 * libhal-storage.h : HAL convenience library for storage devices and volumes
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifndef LIBHAL_STORAGE_H
#define LIBHAL_STORAGE__H

#include <libhal.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * @addtogroup HAL Storage and Volume abstraction
 *
 * @{
 */

#ifndef DOXYGEN_SHOULD_SKIP_THIS
struct HalDrive_s;
typedef struct HalDrive_s HalDrive;
struct HalVolume_s;
typedef struct HalVolume_s HalVolume;
struct HalStoragePolicy_s;
typedef struct HalStoragePolicy_s HalStoragePolicy;
#endif /* DOXYGEN_SHOULD_SKIP_THIS */


typedef enum {
	HAL_STORAGE_ICON_DRIVE_REMOVABLE_DISK           = 0x10000,
	HAL_STORAGE_ICON_DRIVE_REMOVABLE_DISK_IDE       = 0x10001,
	HAL_STORAGE_ICON_DRIVE_REMOVABLE_DISK_SCSI      = 0x10002,
	HAL_STORAGE_ICON_DRIVE_REMOVABLE_DISK_USB       = 0x10003,
	HAL_STORAGE_ICON_DRIVE_REMOVABLE_DISK_IEEE1394  = 0x10004,
	HAL_STORAGE_ICON_DRIVE_DISK                     = 0x10100,
	HAL_STORAGE_ICON_DRIVE_DISK_IDE                 = 0x10101,
	HAL_STORAGE_ICON_DRIVE_DISK_SCSI                = 0x10102,
	HAL_STORAGE_ICON_DRIVE_DISK_USB                 = 0x10103,
	HAL_STORAGE_ICON_DRIVE_DISK_IEEE1394            = 0x10104,
	HAL_STORAGE_ICON_DRIVE_CDROM                    = 0x10200,
	HAL_STORAGE_ICON_DRIVE_CDROM_IDE                = 0x10201,
	HAL_STORAGE_ICON_DRIVE_CDROM_SCSI               = 0x10202,
	HAL_STORAGE_ICON_DRIVE_CDROM_USB                = 0x10203,
	HAL_STORAGE_ICON_DRIVE_CDROM_IEEE1394           = 0x10204,
	HAL_STORAGE_ICON_DRIVE_FLOPPY                   = 0x10300,
	HAL_STORAGE_ICON_DRIVE_FLOPPY_IDE               = 0x10301,
	HAL_STORAGE_ICON_DRIVE_FLOPPY_SCSI              = 0x10302,
	HAL_STORAGE_ICON_DRIVE_FLOPPY_USB               = 0x10303,
	HAL_STORAGE_ICON_DRIVE_FLOPPY_IEEE1394          = 0x10304,
	HAL_STORAGE_ICON_DRIVE_TAPE                     = 0x10400,
	HAL_STORAGE_ICON_DRIVE_COMPACT_FLASH            = 0x10500,
	HAL_STORAGE_ICON_DRIVE_MEMORY_STICK             = 0x10600,
	HAL_STORAGE_ICON_DRIVE_SMART_MEDIA              = 0x10700,
	HAL_STORAGE_ICON_DRIVE_SD_MMC                   = 0x10800,
	HAL_STORAGE_ICON_DRIVE_CAMERA                   = 0x10900,
	HAL_STORAGE_ICON_DRIVE_PORTABLE_AUDIO_PLAYER    = 0x10a00,

	HAL_STORAGE_ICON_VOLUME_REMOVABLE_DISK          = 0x20000,
	HAL_STORAGE_ICON_VOLUME_REMOVABLE_DISK_IDE      = 0x20001,
	HAL_STORAGE_ICON_VOLUME_REMOVABLE_DISK_SCSI     = 0x20002,
	HAL_STORAGE_ICON_VOLUME_REMOVABLE_DISK_USB      = 0x20003,
	HAL_STORAGE_ICON_VOLUME_REMOVABLE_DISK_IEEE1394 = 0x20004,
	HAL_STORAGE_ICON_VOLUME_DISK                    = 0x20100,
	HAL_STORAGE_ICON_VOLUME_DISK_IDE                = 0x20101,
	HAL_STORAGE_ICON_VOLUME_DISK_SCSI               = 0x20102,
	HAL_STORAGE_ICON_VOLUME_DISK_USB                = 0x20103,
	HAL_STORAGE_ICON_VOLUME_DISK_IEEE1394           = 0x20104,
	HAL_STORAGE_ICON_VOLUME_CDROM                   = 0x20200,
	HAL_STORAGE_ICON_VOLUME_CDROM_IDE               = 0x20201,
	HAL_STORAGE_ICON_VOLUME_CDROM_SCSI              = 0x20202,
	HAL_STORAGE_ICON_VOLUME_CDROM_USB               = 0x20203,
	HAL_STORAGE_ICON_VOLUME_CDROM_IEEE1394          = 0x20204,
	HAL_STORAGE_ICON_VOLUME_FLOPPY                  = 0x20300,
	HAL_STORAGE_ICON_VOLUME_FLOPPY_IDE              = 0x20301,
	HAL_STORAGE_ICON_VOLUME_FLOPPY_SCSI             = 0x20302,
	HAL_STORAGE_ICON_VOLUME_FLOPPY_USB              = 0x20303,
	HAL_STORAGE_ICON_VOLUME_FLOPPY_IEEE1394         = 0x20304,
	HAL_STORAGE_ICON_VOLUME_TAPE                    = 0x20400,
	HAL_STORAGE_ICON_VOLUME_COMPACT_FLASH           = 0x20500,
	HAL_STORAGE_ICON_VOLUME_MEMORY_STICK            = 0x20600,
	HAL_STORAGE_ICON_VOLUME_SMART_MEDIA             = 0x20700,
	HAL_STORAGE_ICON_VOLUME_SD_MMC                  = 0x20800,
	HAL_STORAGE_ICON_VOLUME_CAMERA                  = 0x20900,
	HAL_STORAGE_ICON_VOLUME_PORTABLE_AUDIO_PLAYER   = 0x20a00,

	HAL_STORAGE_ICON_DISC_CDROM                     = 0x30000,
	HAL_STORAGE_ICON_DISC_CDR                       = 0x30001,
	HAL_STORAGE_ICON_DISC_CDRW                      = 0x30002,
	HAL_STORAGE_ICON_DISC_DVDROM                    = 0x30003,
	HAL_STORAGE_ICON_DISC_DVDRAM                    = 0x30004,
	HAL_STORAGE_ICON_DISC_DVDR                      = 0x30005,
	HAL_STORAGE_ICON_DISC_DVDRW                     = 0x30006,
	HAL_STORAGE_ICON_DISC_DVDPLUSR                  = 0x30007,
	HAL_STORAGE_ICON_DISC_DVDPLUSRW                 = 0x30008
} HalStoragePolicyIcon;

typedef struct {
	HalStoragePolicyIcon icon;
	const char *icon_path;
} HalStoragePolicyIconPair;

HalStoragePolicy *hal_storage_policy_new              (void);
void              hal_storage_policy_free             (HalStoragePolicy *policy);

void              hal_storage_policy_set_icon_path    (HalStoragePolicy *policy, 
						       HalStoragePolicyIcon icon, const char *path);
void              hal_storage_policy_set_icon_mapping (HalStoragePolicy *policy, HalStoragePolicyIconPair *pairs);
const char       *hal_storage_policy_lookup_icon      (HalStoragePolicy *policy, HalStoragePolicyIcon icon);

typedef enum {
	HAL_DRIVE_BUS_UNKNOWN     = 0x00,
	HAL_DRIVE_BUS_IDE         = 0x01,
	HAL_DRIVE_BUS_SCSI        = 0x02,
	HAL_DRIVE_BUS_USB         = 0x03,
	HAL_DRIVE_BUS_IEEE1394    = 0x04
} HalDriveBus;

typedef enum {
	HAL_DRIVE_TYPE_REMOVABLE_DISK        = 0x00,
	HAL_DRIVE_TYPE_DISK                  = 0x01,
	HAL_DRIVE_TYPE_CDROM                 = 0x02,
	HAL_DRIVE_TYPE_FLOPPY                = 0x03,
	HAL_DRIVE_TYPE_TAPE                  = 0x04,
	HAL_DRIVE_TYPE_COMPACT_FLASH         = 0x05,
	HAL_DRIVE_TYPE_MEMORY_STICK          = 0x06,
	HAL_DRIVE_TYPE_SMART_MEDIA           = 0x07,
	HAL_DRIVE_TYPE_SD_MMC                = 0x08,
	HAL_DRIVE_TYPE_CAMERA                = 0x09,
	HAL_DRIVE_TYPE_PORTABLE_AUDIO_PLAYER = 0x0a
} HalDriveType;

typedef enum {
	HAL_DRIVE_CDROM_CAPS_CDROM      = 0x0001,
	HAL_DRIVE_CDROM_CAPS_CDR        = 0x0002,
	HAL_DRIVE_CDROM_CAPS_CDRW       = 0x0004,
	HAL_DRIVE_CDROM_CAPS_DVDRAM     = 0x0008,
	HAL_DRIVE_CDROM_CAPS_DVDROM     = 0x0010,
	HAL_DRIVE_CDROM_CAPS_DVDR       = 0x0020,
	HAL_DRIVE_CDROM_CAPS_DVDRW      = 0x0040,
	HAL_DRIVE_CDROM_CAPS_DVDPLUSR   = 0x0080,
	HAL_DRIVE_CDROM_CAPS_DVDPLUSRW  = 0x0100
} HalDriveCdromCaps;

HalDrive         *hal_drive_from_udi                    (LibHalContext *hal_ctx, const char *udi);
HalDrive         *hal_drive_from_device_file            (LibHalContext *hal_ctx, const char *device_file);
void              hal_drive_free                        (HalDrive      *drive);

dbus_bool_t       hal_drive_is_hotpluggable             (HalDrive      *drive);
dbus_bool_t       hal_drive_uses_removable_media        (HalDrive      *drive);
HalDriveType      hal_drive_get_type                    (HalDrive      *drive);
HalDriveBus       hal_drive_get_bus                     (HalDrive      *drive);
HalDriveCdromCaps hal_drive_get_cdrom_caps              (HalDrive      *drive);
unsigned int      hal_drive_get_device_major            (HalDrive      *drive);
unsigned int      hal_drive_get_device_minor            (HalDrive      *drive);
const char       *hal_drive_get_type_textual            (HalDrive      *drive);
const char       *hal_drive_get_device_file             (HalDrive      *drive);
const char       *hal_drive_get_udi                     (HalDrive      *drive);
const char       *hal_drive_get_serial                  (HalDrive      *drive);
const char       *hal_drive_get_firmware_version        (HalDrive      *drive);
const char       *hal_drive_get_model                   (HalDrive      *drive);
const char       *hal_drive_get_vendor                  (HalDrive      *drive);
const char       *hal_drive_get_physical_device_udi     (HalDrive      *drive);

char             *hal_drive_policy_compute_display_name (HalDrive      *drive, HalVolume *volume, HalStoragePolicy *policy);
char             *hal_drive_policy_compute_icon_name    (HalDrive      *drive, HalVolume *volume, HalStoragePolicy *policy);

char            **hal_drive_find_all_volumes            (LibHalContext *hal_ctx, HalDrive *drive, int *num_volumes);

typedef enum {
	HAL_VOLUME_USAGE_MOUNTABLE_FILESYSTEM,
	HAL_VOLUME_USAGE_PARTITION_TABLE,
	HAL_VOLUME_USAGE_RAID_MEMBER
} HalVolumeUsage;

typedef enum {
	HAL_VOLUME_DISC_TYPE_CDROM     = 0x00,
	HAL_VOLUME_DISC_TYPE_CDR       = 0x01,
	HAL_VOLUME_DISC_TYPE_CDRW      = 0x02,
	HAL_VOLUME_DISC_TYPE_DVDROM    = 0x03,
	HAL_VOLUME_DISC_TYPE_DVDRAM    = 0x04,
	HAL_VOLUME_DISC_TYPE_DVDR      = 0x05,
	HAL_VOLUME_DISC_TYPE_DVDRW     = 0x06,
	HAL_VOLUME_DISC_TYPE_DVDPLUSR  = 0x07,
	HAL_VOLUME_DISC_TYPE_DVDPLUSRW = 0x08
} HalVolumeDiscType;

HalVolume        *hal_volume_from_udi                      (LibHalContext *hal_ctx, const char *udi);
HalVolume        *hal_volume_from_device_file              (LibHalContext *hal_ctx, const char *device_file);
void              hal_volume_free                          (HalVolume     *volume);
dbus_uint64_t     hal_volume_get_size                      (HalVolume     *volume);

const char       *hal_volume_get_udi                       (HalVolume     *volume);
const char       *hal_volume_get_device_file               (HalVolume     *volume);
unsigned int      hal_volume_get_device_major              (HalVolume     *volume);
unsigned int      hal_volume_get_device_minor              (HalVolume     *volume);
const char       *hal_volume_get_fstype                    (HalVolume     *volume);
const char       *hal_volume_get_fsversion                 (HalVolume     *volume);
HalVolumeUsage    hal_volume_get_fsusage                   (HalVolume     *volume);
dbus_bool_t       hal_volume_is_mounted                    (HalVolume     *volume);
dbus_bool_t       hal_volume_is_partition                  (HalVolume     *volume);
dbus_bool_t       hal_volume_is_disc                       (HalVolume     *volume);
unsigned int      hal_volume_get_partition_number          (HalVolume     *volume);
const char       *hal_volume_get_label                     (HalVolume     *volume);
const char       *hal_volume_get_mount_point               (HalVolume     *volume);
const char       *hal_volume_get_uuid                      (HalVolume     *volume);
const char       *hal_volume_get_storage_device_udi        (HalVolume     *volume);

dbus_bool_t       hal_volume_disc_has_audio                (HalVolume     *volume);
dbus_bool_t       hal_volume_disc_has_data                 (HalVolume     *volume);
dbus_bool_t       hal_volume_disc_is_blank                 (HalVolume     *volume);
dbus_bool_t       hal_volume_disc_is_rewritable            (HalVolume     *volume);
dbus_bool_t       hal_volume_disc_is_appendable            (HalVolume     *volume);
HalVolumeDiscType hal_volume_get_disc_type                 (HalVolume     *volume);

char             *hal_volume_policy_compute_size_as_string (HalVolume     *volume);

char             *hal_volume_policy_compute_display_name   (HalDrive      *drive, HalVolume    *volume, HalStoragePolicy *policy);
char             *hal_volume_policy_compute_icon_name      (HalDrive      *drive, HalVolume    *volume, HalStoragePolicy *policy);

dbus_bool_t       hal_volume_policy_should_be_visible      (HalDrive      *drive, HalVolume    *volume, HalStoragePolicy *policy, const char *target_moint_point);


/** @} */

#if defined(__cplusplus)
}
#endif

#endif /* LIBHAL_STORAGE_H */
