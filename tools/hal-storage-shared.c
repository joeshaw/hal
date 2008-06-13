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


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifdef __FreeBSD__
#include <fstab.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <limits.h>
#include <pwd.h>
#elif sun
#include <fcntl.h>
#include <sys/mnttab.h>
#include <sys/vfstab.h>
#else
#include <mntent.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#include <sys/file.h>
#include <errno.h>
#include <syslog.h>

#include "hal-storage-shared.h"

#ifdef __FreeBSD__
struct mtab_handle
{
  struct statfs	*mounts;
  int		n_mounts;
  int		iter;
};
#endif


gboolean
mtab_open (gpointer *handle)
{
#ifdef __FreeBSD__
	struct mtab_handle *mtab;

	mtab = g_new0 (struct mtab_handle, 1);
	mtab->n_mounts = getmntinfo (&mtab->mounts, MNT_NOWAIT);
	if (mtab->n_mounts == 0) {
		g_free (mtab);
		return FALSE;
	}

	*handle = mtab;
	return TRUE;
#elif sun
	*handle = fopen (MNTTAB, "r");
	return *handle != NULL;
#else
	*handle = fopen ("/proc/mounts", "r");
	return *handle != NULL;
#endif
}

char *
mtab_next (gpointer handle, char **mount_point)
{
#ifdef __FreeBSD__
	struct mtab_handle *mtab = handle;

	if (mtab->iter < mtab->n_mounts) {
		if (mount_point != NULL) {
			*mount_point = g_strdup (mtab->mounts[mtab->iter].f_mntonname);
		}
		return mtab->mounts[mtab->iter++].f_mntfromname;
	} else {
		return NULL;
	}
#elif sun
	static struct mnttab mnt;

	if (getmntent (handle, &mnt) == 0) {
		if (mount_point != NULL) {
			*mount_point = g_strdup (mnt.mnt_mountp);
		}
		return mnt.mnt_special;
	} else {
		return NULL;
	}
#else
	struct mntent *mnt;

	mnt = getmntent (handle);

	if (mnt != NULL) {
		if (mount_point != NULL) {
			*mount_point = g_strdup (mnt->mnt_dir);
		}
		return mnt->mnt_fsname;
	} else {
		return NULL;
	}
#endif
}

void
mtab_close (gpointer handle)
{
#ifdef __FreeBSD__
	g_free (handle);
#else
	fclose (handle);
#endif
}



gboolean
fstab_open (gpointer *handle)
{
#ifdef __FreeBSD__
	return setfsent () == 1;
#elif sun
	*handle = fopen (VFSTAB, "r");
	return *handle != NULL;
#else
	*handle = fopen ("/etc/fstab", "r");
	return *handle != NULL;
#endif
}

char *
fstab_next (gpointer handle, char **mount_point)
{
#ifdef __FreeBSD__
	struct fstab *fstab;

	fstab = getfsent ();

	/* TODO: fill out mount_point */
	if (mount_point != NULL && fstab != NULL) {
		*mount_point = fstab->fs_file;
	}

	return fstab ? fstab->fs_spec : NULL;
#elif sun
	static struct vfstab v;

	return getvfsent (handle, &v) == 0 ? v.vfs_special : NULL;
#else
	struct mntent *mnt;

	mnt = getmntent (handle);

	if (mount_point != NULL && mnt != NULL) {
		*mount_point = mnt->mnt_dir;
	}

	return mnt ? mnt->mnt_fsname : NULL;
#endif
}

void
fstab_close (gpointer handle)
{
#ifdef __FreeBSD__
	endfsent ();
#else
	fclose (handle);
#endif
}

#ifdef __FreeBSD__
#define UMOUNT		"/sbin/umount"
#elif sun
#define UMOUNT		"/sbin/umount"
#else
#define UMOUNT		"/bin/umount"
#endif

void
bailout_if_drive_is_locked (LibHalContext *hal_ctx, LibHalDrive *drive, const char *invoked_by_syscon_name)
{
        DBusError error;

        if (drive != NULL && invoked_by_syscon_name != NULL) {
                dbus_error_init (&error);
                if (libhal_device_is_caller_locked_out (hal_ctx, 
                                                        libhal_drive_get_udi (drive),
                                                        "org.freedesktop.Hal.Device.Storage",
                                                        invoked_by_syscon_name,
                                                        &error)) {
                        fprintf (stderr, "org.freedesktop.Hal.Device.InterfaceLocked\n");
                        fprintf (stderr, "The enclosing drive for the volume is locked\n");
                        exit (1);
                }
        }
}


void
unknown_error (const char *detail)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.UnknownFailure\n");
	fprintf (stderr, "%s\n", detail);
	exit (1);
}


static void
device_busy (const char *detail)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.Busy\n");
	fprintf (stderr, "%s\n", detail);
	exit (1);
}


static void
not_mounted (const char *detail)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.NotMounted\n");
	fprintf (stderr, "%s\n", detail);
	exit (1);
}


static void
not_mounted_by_hal (const char *detail)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.NotMountedByHal\n");
	fprintf (stderr, "%s\n", detail);
	exit (1);
}

static void
permission_denied_privilege (const char *privilege, const char *result)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.PermissionDeniedByPolicy\n");
	fprintf (stderr, "%s %s <-- (privilege, result)\n", privilege, result);
	exit (1);
}

static void
permission_denied_volume_ignore (const char *device)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.PermissionDenied\n");
	fprintf (stderr, "Device has %s volume.ignore set to TRUE. Refusing to mount.\n", device);
	exit (1);
}

void
handle_unmount (LibHalContext *hal_ctx, 
		const char *udi,
		LibHalVolume *volume, LibHalDrive *drive, const char *device, 
		const char *invoked_by_uid, const char *invoked_by_syscon_name,
		gboolean option_lazy, gboolean option_force)
{
	int i, j;
	DBusError error;
	GError *err = NULL;
	char *sout = NULL;
	char *serr = NULL;
	int exit_status;
	char *args[10];
	int na;
	FILE *hal_mtab_orig;
	int hal_mtab_orig_len;
	int num_read;
	char *hal_mtab_buf;
	char **lines;
	char *mount_point_to_unmount;
	gboolean mounted_by_other_uid;
	FILE *hal_mtab_new;

#ifdef DEBUG
	printf ("device                           = %s\n", device);
	printf ("invoked by uid                   = %s\n", invoked_by_uid);
	printf ("invoked by system bus connection = %s\n", invoked_by_syscon_name);
#endif

	if (volume != NULL) {
		dbus_error_init (&error);
		if (libhal_device_get_property_bool (hal_ctx, udi, "volume.ignore", &error)) { 
			permission_denied_volume_ignore (device);
		}

		if (dbus_error_is_set (&error)) {
			dbus_error_free(&error);
			unknown_error("Error while get volume.ignore");
		}

		if (!libhal_volume_is_mounted (volume)) {
			not_mounted ("According to HAL, the volume is not mounted");
		}
	}

        bailout_if_drive_is_locked (hal_ctx, drive, invoked_by_syscon_name);

	/* check hal's mtab file to verify the device to unmount is actually mounted by hal */
	hal_mtab_orig = fopen ("/media/.hal-mtab", "r");
	if (hal_mtab_orig == NULL) {
		unknown_error ("Cannot open /media/.hal-mtab");
	}
	if (fseek (hal_mtab_orig, 0L, SEEK_END) != 0) {
		unknown_error ("Cannot seek to end of /media/.hal-mtab");
	}
	hal_mtab_orig_len = ftell (hal_mtab_orig);
	if (hal_mtab_orig_len < 0) {
		unknown_error ("Cannot determine size of /media/.hal-mtab");
	}
	rewind (hal_mtab_orig);
	hal_mtab_buf = g_new0 (char, hal_mtab_orig_len + 1);
	num_read = fread (hal_mtab_buf, 1, hal_mtab_orig_len, hal_mtab_orig);
	if (num_read != hal_mtab_orig_len) {
		unknown_error ("Cannot read from /media/.hal-mtab");
	}
	fclose (hal_mtab_orig);

#ifdef DEBUG
	printf ("hal_mtab = '%s'\n", hal_mtab_buf);
#endif

	lines = g_strsplit (hal_mtab_buf, "\n", 0);
	g_free (hal_mtab_buf);

	mount_point_to_unmount = NULL;
	mounted_by_other_uid = TRUE;

	/* find the entry we're going to unmount */
	for (i = 0; lines[i] != NULL; i++) {
		char **line_elements;

#ifdef DEBUG
		printf (" line = '%s'\n", lines[i]);
#endif

		if ((lines[i])[0] == '#')
			continue;

		line_elements = g_strsplit (lines[i], "\t", 6);
		if (g_strv_length (line_elements) == 6) {

#ifdef DEBUG
			printf ("  devfile     = '%s'\n", line_elements[0]);
			printf ("  uid         = '%s'\n", line_elements[1]);
			printf ("  session id  = '%s'\n", line_elements[2]);
			printf ("  fs          = '%s'\n", line_elements[3]);
			printf ("  options     = '%s'\n", line_elements[4]);
			printf ("  mount_point = '%s'\n", line_elements[5]);
#endif

			if (strcmp (line_elements[0], device) == 0) {
				char *line_to_free;

				if (strcmp (line_elements[1], invoked_by_uid) == 0)
					mounted_by_other_uid = FALSE;

				mount_point_to_unmount = g_strdup (line_elements[5]);

				line_to_free = lines[i];

				for (j = i; lines[j] != NULL; j++) {
					lines[j] = lines[j+1];
				}
				lines[j] = NULL;

				g_free (line_to_free);

				g_strfreev (line_elements);
				goto line_found;

			}

		}

		g_strfreev (line_elements);
	}
line_found:

	if (mount_point_to_unmount == NULL) {
		not_mounted_by_hal ("Device to unmount is not in /media/.hal-mtab so it is not mounted by HAL");
	}

        /* NOTE: it doesn't make sense to require a privilege a'la
         * "hal-storage-unmount" because we only allow user to unmount
         * volumes mounted by himself in the first place... and it
         * would be odd to allow Mount() but disallow Unmount()...
         */

	/* if mounted_by_other_uid==TRUE: bail out, unless if we got the 
         *                                "hal-storage-unmount-volumes-mounted-by-others"
	 *
	 * We allow uid 0 to actually ensure that Unmount(options=["lazy"], "/dev/blah") works from addon-storage.
	 */
	if ((strcmp (invoked_by_uid, "0") != 0) && mounted_by_other_uid) {
                const char *action = "org.freedesktop.hal.storage.unmount-others";
#ifdef HAVE_POLKIT
                if (invoked_by_syscon_name != NULL) {
                        char *polkit_result;
                        dbus_error_init (&error);
                        polkit_result = libhal_device_is_caller_privileged (hal_ctx,
                                                                            udi,
                                                                            action,
                                                                            invoked_by_syscon_name,
                                                                            &error);
                        if (polkit_result == NULL){
                                unknown_error ("IsCallerPrivileged() failed");
                        }
                        if (strcmp (polkit_result, "yes") != 0) {
                                permission_denied_privilege (action, polkit_result);
                        }
                        libhal_free_string (polkit_result);
                }
#else
                permission_denied_privilege (action, "no");
#endif
	}

	/* create new .hal-mtab~ file without the entry we're going to unmount */
	hal_mtab_new = fopen ("/media/.hal-mtab~", "w");
	if (hal_mtab_new == NULL) {
		unknown_error ("Cannot create /media/.hal-mtab~");
	}
	for (i = 0; lines[i] != NULL; i++) {
		if (i > 0) {
			char anewl[2] = "\n\0";
			if (fwrite (anewl, 1, 1, hal_mtab_new) != 1) {
				unknown_error ("Cannot write to /media/.hal-mtab~");
			}
		}

		if (fwrite (lines[i], 1, strlen (lines[i]), hal_mtab_new) != strlen (lines[i])) {
			unknown_error ("Cannot write to /media/.hal-mtab~");
		}

	}
	fclose (hal_mtab_new);

	g_strfreev (lines);

	/* construct arguments to /bin/umount */
	na = 0;
	args[na++] = UMOUNT;
#ifndef __FreeBSD__
	if (option_lazy)
		args[na++] = "-l";
#endif
	if (option_force)
		args[na++] = "-f";
	args[na++] = (char *) device;
	args[na++] = NULL;

#ifdef DEBUG
	printf ("will umount %s (mounted at '%s'), mounted_by_other_uid=%d\n", 
		device, mount_point_to_unmount, mounted_by_other_uid);
#endif

	/* invoke /bin/umount */
	if (!g_spawn_sync ("/",
			   args,
			   NULL,
			   G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
			   NULL,
			   NULL,
			   &sout,
			   &serr,
			   &exit_status,
			   &err)) {
		printf ("Cannot execute %s\n", UMOUNT);
		unlink ("/media/.hal-mtab~");
		unknown_error ("Cannot spawn " UMOUNT);
	}

	/* check if unmount was succesful */
	if (exit_status != 0) {
		printf ("%s error %d, stdout='%s', stderr='%s'\n", UMOUNT, exit_status, sout, serr);

		if (strstr (serr, "device is busy") != NULL) {
			unlink ("/media/.hal-mtab~");
			device_busy (serr);
		} else {
			unlink ("/media/.hal-mtab~");
			unknown_error (serr);
		}
	}

	/* unmount was succesful, remove directory we created in Mount() */
	if (g_rmdir (mount_point_to_unmount) != 0) {
		unlink ("/media/.hal-mtab~");
		unknown_error ("Cannot remove directory");
	}

	/* set new .hal-mtab file */
	if (rename ("/media/.hal-mtab~", "/media/.hal-mtab") != 0) {
		unlink ("/media/.hal-mtab~");
		unknown_error ("Cannot rename /media/.hal-mtab~ to /media/.hal-mtab");
	}

#ifdef DEBUG
	printf ("done unmounting\n");
#endif
	openlog ("hald", 0, LOG_DAEMON);
	syslog (LOG_INFO, "unmounted %s from '%s' on behalf of uid %s", device, mount_point_to_unmount, invoked_by_uid);
	closelog ();

	g_free (sout);
	g_free (serr);
	g_free (mount_point_to_unmount);
}

void
handle_eject (DBusConnection *system_bus,
              LibHalContext *hal_ctx, 
	      const char *udi,
	      LibHalDrive *drive, const char *device, 
	      const char *invoked_by_uid, const char *invoked_by_syscon_name,
	      gboolean closetray)
{
	GError *err = NULL;
	char *sout = NULL;
	char *serr = NULL;
	int exit_status;
	char *args[10];
	int na;
	int fd;
	int num_excl_tries;
        DBusError error;

	/* When called here all the file systems from this device are
	 * already unmounted. That's actually guaranteed; see
	 * tools/hal-storage-eject.c for details.
	 *
	 * Next thing to check is that we can open the device
	 * exclusively; if we can't, it means that some app is holding
	 * a file descriptor open using O_EXCL (we've already unmounted
	 * all file systems). 
	 * 
	 * And that means our polling won't work. Thus ejecting the
	 * disc will mean that the hal device database isn't
	 * updated. 
	 *
	 * Sigh. 
	 *
	 * So better check that we can we can open O_EXCL.
	 *
	 * This is RH #207177 - https://bugzilla.redhat.com/bugzilla/show_bug.cgi?id=207177
	 *
	 * Credit goes to Bill Nottingham for suggesting this check.
	 */

	num_excl_tries = 5;
try_open_excl_again:
	fd = open (device, O_RDONLY | O_NONBLOCK | O_EXCL);
	if (fd < 0 && errno == EBUSY) {

		/* Hey, so this might just be because we're colliding
		 * with the polling addon that also opens O_EXCL. We
		 * know this only every two seconds (or less frequent)
		 * so try to open the device a few times...
		 */
		usleep (100 * 1000); /* sleep 100 ms between each attempt */
		if (num_excl_tries-- > 0)
			goto try_open_excl_again;

		/* Some other app, like gnome-cd, has already opened with O_EXCL. Refuse to eject.
		 */
		device_busy ("Some application is using the device");
	}
	close (fd);

#ifdef HAVE_POLKIT
        if (invoked_by_syscon_name != NULL) {
                char *polkit_result;
                const char *action = "org.freedesktop.hal.storage.eject";
                dbus_error_init (&error);
                polkit_result = libhal_device_is_caller_privileged (hal_ctx,
                                                                    udi,
                                                                    action,
                                                                    invoked_by_syscon_name,
                                                                    &error);
                if (polkit_result == NULL){
                        unknown_error ("IsCallerPrivileged() failed");
                }
                if (strcmp (polkit_result, "yes") != 0) {
                        permission_denied_privilege (action, polkit_result);
                }
                libhal_free_string (polkit_result);
        }
#endif

#ifdef DEBUG
	printf ("device                           = %s\n", device);
	printf ("invoked by uid                   = %s\n", invoked_by_uid);
	printf ("invoked by system bus connection = %s\n", invoked_by_syscon_name);
#endif

        bailout_if_drive_is_locked (hal_ctx, drive, invoked_by_syscon_name);

	/* construct arguments to EJECT_PROGRAM (e.g. /usr/bin/eject) */
	na = 0;
	args[na++] = EJECT_PROGRAM;
#ifdef __FreeBSD__
	args[na++] = "-f";
	args[na++] = (char *) device;
	if (closetray)
		args[na++] = "close";
	else
		args[na++] = "eject";
#else
	if (closetray) {
		args[na++] = "-t";
	}
	args[na++] = (char *) device;
#endif
	args[na++] = NULL;

#ifdef DEBUG
	printf ("will eject %s\n", device);
#endif

	/* invoke eject command */
	if (!g_spawn_sync ("/",
			   args,
			   NULL,
			   G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
			   NULL,
			   NULL,
			   &sout,
			   &serr,
			   &exit_status,
			   &err)) {
		printf ("Cannot execute %s\n", EJECT_PROGRAM);
		unknown_error ("Cannot spawn " EJECT_PROGRAM);
	}

	/* check if eject was succesful */
	if (exit_status != 0) {
		printf ("%s error %d, stdout='%s', stderr='%s'\n", EJECT_PROGRAM, exit_status, sout, serr);

		unknown_error (serr);
	}

	/* eject was succesful... now.. if we're not polling the drive
         * invoke CheckForMedia() to make sure the device database is
         * properly updated...
         */
        if (!libhal_drive_is_media_detection_automatic (drive)) {
                DBusMessage *message;
                DBusMessage *reply;

                message = dbus_message_new_method_call ("org.freedesktop.Hal",
                                                        udi,
                                                        "org.freedesktop.Hal.Device.Storage.Removable",
                                                        "CheckForMedia");
                dbus_error_init (&error);
                reply = dbus_connection_send_with_reply_and_block (system_bus, message, -1, &error);
        }

#ifdef DEBUG
	printf ("done ejecting\n");
#endif

	g_free (sout);
	g_free (serr);
}


static int lock_mtab_fd = -1;

gboolean
lock_hal_mtab (void)
{
	if (lock_mtab_fd >= 0)
		return TRUE;

	printf ("%d: XYA attempting to get lock on /media/.hal-mtab-lock\n", getpid ());

	lock_mtab_fd = open ("/media/.hal-mtab-lock", O_CREAT | O_RDWR, 0600);

	if (lock_mtab_fd < 0)
		return FALSE;

	fcntl(lock_mtab_fd, F_SETFD, FD_CLOEXEC);

tryagain:
#if sun
	if (lockf (lock_mtab_fd, F_LOCK, 0) != 0) {
#else
	if (flock (lock_mtab_fd, LOCK_EX) != 0) {
#endif
		if (errno == EINTR)
			goto tryagain;
		return FALSE;
	}

	printf ("%d: XYA got lock on /media/.hal-mtab-lock\n", getpid ());


	return TRUE;
}

void 
unlock_hal_mtab (void)
{
#if sun
	lockf (lock_mtab_fd, F_ULOCK, 0);
#else
	flock (lock_mtab_fd, LOCK_UN);
#endif
	close (lock_mtab_fd);
	lock_mtab_fd = -1;
	printf ("%d: XYA released lock on /media/.hal-mtab-lock\n", getpid ());
}
