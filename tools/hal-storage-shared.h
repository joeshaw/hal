/***************************************************************************
 * CVSID: $Id: hal-storage-mount.c,v 1.7 2006/06/21 00:44:03 david Exp $
 *
 * hal-storage-mount.c : Mount wrapper
 *
 * Copyright (C) 2006 David Zeuthen, <david@fubar.dk>
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

#ifndef HAL_STORAGE_SHARED_H
#define HAL_STORAGE_SHARED_H

#include <libhal.h>
#include <libhal-storage.h>

/*#define DEBUG*/
#define DEBUG

gboolean mtab_open (gpointer *handle);
char *mtab_next (gpointer handle, char **mount_point);
void mtab_close (gpointer handle);

gboolean fstab_open (gpointer *handle);
char *fstab_next (gpointer handle, char **mount_point);
void fstab_close (gpointer handle);

gboolean lock_hal_mtab (void);
void unlock_hal_mtab (void);

void unknown_error (const char *detail);

void bailout_if_drive_is_locked (LibHalContext *hal_ctx, LibHalDrive *drive, const char *invoked_by_syscon_name);

void handle_unmount (LibHalContext *hal_ctx, 
		     const char *udi,
		     LibHalVolume *volume, LibHalDrive *drive, const char *device, 
		     const char *invoked_by_uid, const char *invoked_by_syscon_name,
		     gboolean option_lazy, gboolean option_force);

void handle_eject (DBusConnection *system_bus,
                   LibHalContext *hal_ctx, 
		   const char *udi,
		   LibHalDrive *drive, const char *device, 
		   const char *invoked_by_uid, const char *invoked_by_syscon_name,
		   gboolean closetray);

#endif /* HAL_STORAGE_SHARED_H */

