/***************************************************************************
 * CVSID: $Id$
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
#include <mntent.h>
#include <sys/types.h>
#include <unistd.h>

#include <libhal/libhal.h>
#include <libhal-storage/libhal-storage.h>
#include <libpolkit.h>

/*#define DEBUG*/
#define DEBUG

static void
usage (void)
{
	fprintf (stderr, "This script should only be started by hald.\n");
	exit (1);
}

static void
unknown_error (void)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.UnknownError\n");
	fprintf (stderr, "An unexpected error occured\n");
	exit (1);
}

static void
permission_denied_volume_ignore (const char *device)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.PermissionDenied\n");
	fprintf (stderr, "Device has %s volume.ignore set to TRUE. Refusing to mount.\n", device);
	exit (1);
}

static void
permission_denied_etc_fstab (const char *device)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.PermissionDenied\n");
	fprintf (stderr, "Device %s is listed in /etc/fstab. Refusing to mount.\n", device);
	exit (1);
}

static void
already_mounted (const char *device)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.AlreadyMounted\n");
	fprintf (stderr, "Device %s is already mounted.\n", device);
	exit (1);
}

static void
invalid_mount_option (const char *option, const char *uid)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.InvalidMountOption\n");
	fprintf (stderr, "The option '%s' is not allowed for uid=%s\n", option, uid);
	exit (1);
}

static void
unknown_filesystem (const char *filesystem)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.UnknownFilesystemType\n");
	fprintf (stderr, "Unknown file system '%s'\n", filesystem);
	exit (1);
}

static void
invalid_mount_point (const char *mount_point)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.InvalidMountpoint\n");
	fprintf (stderr, "The mount point '%s' is invalid\n", mount_point);
	exit (1);
}

static void
mount_point_not_available (const char *mount_point)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.Volume.MountPointNotAvailable\n");
	fprintf (stderr, "The mount point '%s' is already occupied\n", mount_point);
	exit (1);
}


static void
permission_denied_privilege (const char *privilege, const char *uid)
{
	fprintf (stderr, "org.freedesktop.Hal.Device.PermissionDeniedByPolicy\n");
	fprintf (stderr, "%s refused uid %s\n", privilege, uid);
	exit (1);
}



/* borrowed from gtk/gtkfilesystemunix.c in GTK+ on 02/23/2006 */
static void
canonicalize_filename (gchar *filename)
{
	gchar *p, *q;
	gboolean last_was_slash = FALSE;
	
	p = filename;
	q = filename;
	
	while (*p)
	{
		if (*p == G_DIR_SEPARATOR)
		{
			if (!last_was_slash)
				*q++ = G_DIR_SEPARATOR;
			
			last_was_slash = TRUE;
		}
		else
		{
			if (last_was_slash && *p == '.')
			{
				if (*(p + 1) == G_DIR_SEPARATOR ||
				    *(p + 1) == '\0')
				{
					if (*(p + 1) == '\0')
						break;
					
					p += 1;
				}
				else if (*(p + 1) == '.' &&
					 (*(p + 2) == G_DIR_SEPARATOR ||
					  *(p + 2) == '\0'))
				{
					if (q > filename + 1)
					{
						q--;
						while (q > filename + 1 &&
						       *(q - 1) != G_DIR_SEPARATOR)
							q--;
					}
					
					if (*(p + 2) == '\0')
						break;
					
					p += 2;
				}
				else
				{
					*q++ = *p;
					last_was_slash = FALSE;
				}
			}
			else
			{
				*q++ = *p;
				last_was_slash = FALSE;
			}
		}
		
		p++;
	}
	
	if (q > filename + 1 && *(q - 1) == G_DIR_SEPARATOR)
		q--;
	
	*q = '\0';
}

static char *
resolve_symlink (const char *file)
{
	GError *error;
	char *dir;
	char *link;
	char *f;
	char *f1;

	f = g_strdup (file);

	while (g_file_test (f, G_FILE_TEST_IS_SYMLINK)) {
		link = g_file_read_link (f, &error);
		if (link == NULL) {
			g_warning ("Cannot resolve symlink %s: %s", f, error->message);
			g_error_free (error);
			g_free (f);
			f = NULL;
			goto out;
		}
		
		dir = g_path_get_dirname (f);
		f1 = g_strdup_printf ("%s/%s", dir, link);
		g_free (dir);
		g_free (link);
		g_free (f);
		f = f1;
	}

out:
	if (f != NULL)
		canonicalize_filename (f);
	return f;
}

static void
bailout_if_in_fstab (const char *device)
{
	FILE *fstab;
	struct mntent *mnt;

	/* check if /etc/fstab mentions this device... (with symlinks etc) */
	fstab = fopen ("/etc/fstab", "r");
	if (fstab == NULL) {
		printf ("cannot open /etc/fstab\n");
		unknown_error ();		
	}
	while ((mnt = getmntent (fstab)) != NULL) {
		char *resolved;

		resolved = resolve_symlink (mnt->mnt_fsname);
#ifdef DEBUG
		printf ("/etc/fstab: device %s -> %s \n", mnt->mnt_fsname, resolved);
#endif
		if (strcmp (device, resolved) == 0) {
			printf ("%s (-> %s) found in /etc/fstab. Not mounting.\n", mnt->mnt_fsname, resolved);
			permission_denied_etc_fstab (device);
		}

		g_free (resolved);
	}
	fclose (fstab);
}

static void
bailout_if_mounted (const char *device)
{
	FILE *mtab;
	struct mntent *mnt;

	/* check if /proc/mounts mentions this device... (with symlinks etc) */
	mtab = fopen ("/proc/mounts", "r");
	if (mtab == NULL) {
		printf ("cannot open /proc/mounts\n");
		unknown_error ();		
	}
	while ((mnt = getmntent (mtab)) != NULL) {
		char *resolved;

		resolved = resolve_symlink (mnt->mnt_fsname);
#ifdef DEBUG
		printf ("/proc/mounts: device %s -> %s \n", mnt->mnt_fsname, resolved);
#endif
		if (strcmp (device, resolved) == 0) {
			printf ("%s (-> %s) found in /proc/mounts. Not mounting.\n", mnt->mnt_fsname, resolved);
			already_mounted (device);
		}

		g_free (resolved);
	}
	fclose (mtab);
}


static void
handle_mount (LibHalContext *hal_ctx, LibPolKitContext *pol_ctx, const char *udi,
	      LibHalVolume *volume, LibHalDrive *drive, const char *device, 
	      const char *invoked_by_uid, pid_t invoked_by_pid)
{
	int i, j;
	DBusError error;
	char mount_point[256];
	char mount_fstype[256];
	char mount_options[1024];
	char **allowed_options;
	char **given_options;
	gboolean wants_to_change_uid;
	char *mount_dir;
	char *cbh_path;
	FILE *cbh;
	GError *err = NULL;
	char *sout = NULL;
	char *serr = NULL;
	int exit_status;
	char *args[10];
	int na;
	GString *mount_option_str;
	gboolean pol_is_fixed;
	gboolean pol_change_uid;
	char *privilege;
	gboolean allowed_by_privilege;
	gboolean is_temporary_privilege;
	gboolean explicit_mount_point_given;
	const char *end;

#ifdef DEBUG
	printf ("device         = %s\n", device);
	printf ("invoked by uid = %s\n", invoked_by_uid);
	printf ("invoked by pid = %d\n", invoked_by_pid);
#endif

	if (volume != NULL) {
		if (libhal_volume_is_mounted (volume)) {
			already_mounted (device);
		}
	} else {
		bailout_if_mounted (device);
	}

	if (volume != NULL) {
		dbus_error_init (&error);
		if (libhal_device_get_property_bool (hal_ctx, udi, "volume.ignore", &error) || 
		    dbus_error_is_set (&error)) {
			permission_denied_volume_ignore (device);
		}
	}

	bailout_if_in_fstab (device);

	/* TODO: sanity check that what hal exports is correct (cf. Martin Pitt's email) */

	/* read from stdin */
	fgets (mount_point,   sizeof (mount_point),   stdin);
	fgets (mount_fstype,  sizeof (mount_fstype),  stdin);
	fgets (mount_options, sizeof (mount_options), stdin);
	if (strlen (mount_point) > 0)
		mount_point   [strlen (mount_point)   - 1] = '\0';
	if (strlen (mount_fstype) > 0)
		mount_fstype  [strlen (mount_fstype)  - 1] = '\0';
	if (strlen (mount_options) > 0)
		mount_options [strlen (mount_options) - 1] = '\0';
	/* validate that input from stdin is UTF-8 */
	if (!g_utf8_validate (mount_point, -1, &end))
		unknown_error ();
	if (!g_utf8_validate (mount_fstype, -1, &end))
		unknown_error ();
	if (!g_utf8_validate (mount_options, -1, &end))
		unknown_error ();
#ifdef DEBUG
	printf ("mount_point    = '%s'\n", mount_point);
	printf ("mount_fstype   = '%s'\n", mount_fstype);
	printf ("mount_options  = '%s'\n", mount_options);
#endif

	/* delete any trailing whitespace options from splitting the string */
	given_options = g_strsplit (mount_options, "\t", 0);
	for (i = g_strv_length (given_options) - 1; i >= 0; --i) {
		if (strlen (given_options[i]) > 0)
			break;
		given_options[i] = NULL;
	}

	/* figure out mount point if no mount point is given... */
	explicit_mount_point_given = FALSE;
	if (strlen (mount_point) == 0) {
		char *p;
		const char *label;

		if (volume != NULL)
			label = libhal_volume_get_label (volume);
		else
			label = NULL;

		if (label != NULL) {
			/* best - use label */
			g_strlcpy (mount_point, label, sizeof (mount_point));

			/* TODO: use drive type */

		} else {
			/* fallback - use "disk" */
			g_snprintf (mount_point, sizeof (mount_point), "disk");
		}

		/* sanitize computed mount point name, e.g. replace invalid chars with '-' */
		p = mount_point;
		while (TRUE) {
			p = g_utf8_strchr (mount_point, -1, G_DIR_SEPARATOR);
			if (p == NULL)
				break;
			*p = '-';
		};

	} else {
		explicit_mount_point_given = TRUE;
	}

	/* check mount point name - only forbid separators */
	if (g_utf8_strchr (mount_point, -1, G_DIR_SEPARATOR) != NULL) {
		printf ("'%s' is an invalid mount point\n", mount_point);
		invalid_mount_point (mount_point);
	}

	/* check if mount point is available - append number to mount point */
	i = 0;
	mount_dir = NULL;
	while (TRUE) {
		g_free (mount_dir);
		if (i == 0)
			mount_dir = g_strdup_printf ("/media/%s", mount_point);
		else
			mount_dir = g_strdup_printf ("/media/%s-%d", mount_point, i);

#ifdef DEBUG
		printf ("trying dir %s\n", mount_dir);
#endif

		if (!g_file_test (mount_dir, G_FILE_TEST_EXISTS)) {
			break;
		}

		if (explicit_mount_point_given) {
			mount_point_not_available (mount_dir);
		}

		i++;
	}

	/* TODO: possible race here... need to have only one hal-storage-mount copy run at a time */

	dbus_error_init (&error);
	allowed_options = libhal_device_get_property_strlist (hal_ctx, udi, "volume.mount.valid_options", &error);
	if (dbus_error_is_set (&error)) {
		unknown_error ();
	}

#ifdef DEBUG
	for (i = 0; given_options[i] != NULL; i++)
		printf ("given_options[%d] = '%s'\n", i, given_options[i]);
	for (i = 0; allowed_options[i] != NULL; i++)
		printf ("allowed_options[%d] = '%s'\n", i, allowed_options[i]);
#endif

	wants_to_change_uid = FALSE;

	/* check mount options */
	for (i = 0; given_options[i] != NULL; i++) {
		char *given = given_options[i];

		for (j = 0; allowed_options[j] != NULL; j++) {
			char *allow = allowed_options[j];
			int allow_len = strlen (allow);

			if (strcmp (given, allow) == 0) {
				goto option_ok;
			}

			if ((allow[allow_len - 1] == '=') && 
			    (strncmp (given, allow, allow_len) == 0) &&
			    (int) strlen (given) > allow_len) {

				/* option matched allowed ending in '=', e.g.
				 * given == "umask=foobar" and allowed == "umask="
				 */
				if (strcmp (allow, "uid=") == 0) {
					uid_t uid;
					char *endp;
					/* check for uid=, it requires special handling */
					uid = (uid_t) strtol (given + allow_len, &endp, 10);
					if (*endp != '\0') {
						printf ("'%s' is not a number?\n", given);
						unknown_error ();
					}
#ifdef DEBUG
					printf ("%s with uid %d\n", allow, uid);
#endif
					wants_to_change_uid = TRUE;

					goto option_ok;
				} else {

					goto option_ok;
				}
			}
		}

		/* apparently option was not ok */
		invalid_mount_option (given, invoked_by_uid);

	option_ok:
		;
	}

	/* Check privilege */
	pol_is_fixed = TRUE;
	if (libhal_drive_is_hotpluggable (drive) || libhal_drive_uses_removable_media (drive))
		pol_is_fixed = FALSE;

	pol_change_uid = FALSE;
	/* don't consider uid= on non-pollable drives for the purpose of policy 
	 * (since these drives normally use vfat)
	 */
	if (volume != NULL) {
		/* don't consider uid= on vfat change-uid for the purpose of policy
		 * (since vfat doesn't contain uid/gid bits) 
		 */
		if (strcmp (libhal_volume_get_fstype (volume), "vfat") != 0) {
			pol_change_uid = wants_to_change_uid;
		}
	}

	if (pol_is_fixed) {
		if (pol_change_uid) {
			privilege = "hal-storage-fixed-mount-change-uid";
		} else {
			privilege = "hal-storage-fixed-mount";
		}
	} else {
		if (pol_change_uid) {
			privilege = "hal-storage-removable-mount-change-uid";
		} else {
			privilege = "hal-storage-removable-mount";
		}
	}

#ifdef DEBUG
	printf ("using privilege %s for uid %s, pid %d\n", privilege, invoked_by_uid, invoked_by_pid);
#endif

	if (libpolkit_is_uid_allowed_for_privilege (pol_ctx, 
						    invoked_by_pid,
						    invoked_by_uid,
						    privilege,
						    udi,
						    &allowed_by_privilege,
						    &is_temporary_privilege) != LIBPOLKIT_RESULT_OK) {
		printf ("cannot lookup privilege\n");
		unknown_error ();
	}

	if (!allowed_by_privilege) {
		printf ("caller don't possess privilege\n");
		permission_denied_privilege (privilege, invoked_by_uid);
	}

#ifdef DEBUG
	printf ("passed privilege\n");
#endif

	/* create directory and the .created-by-hal file */
	if (g_mkdir (mount_dir, 0700) != 0) {
		printf ("Cannot create '%s'\n", mount_dir);
		unknown_error ();
	}

	cbh_path = g_strdup_printf ("%s/.created-by-hal", mount_dir);
	cbh = fopen (cbh_path, "w");
	if (cbh == NULL) {
		printf ("Cannot create '%s'\n", cbh_path);
		g_rmdir (mount_dir);
		unknown_error ();
	}
	fclose (cbh);

	/* construct arguments to mount */
	na = 0;
	args[na++] = "/bin/mount";
	if (strlen (mount_fstype) > 0) {
		args[na++] = "-t";
		args[na++] = mount_fstype;
	} else if (volume == NULL) {
		/* non-pollable drive; force auto */
		args[na++] = "-t";
		args[na++] = "auto";
	} else if (libhal_volume_get_fstype (volume) != NULL && strlen (libhal_volume_get_fstype (volume)) > 0) {
		args[na++] = "-t";
		args[na++] = (char *) libhal_volume_get_fstype (volume);
	}

	args[na++] = "-o";
	mount_option_str = g_string_new("noexec,nosuid,nodev");
	for (i = 0; given_options[i] != NULL; i++) {
		g_string_append (mount_option_str, ",");
		g_string_append (mount_option_str, given_options[i]);
	}
	args[na++] = g_string_free (mount_option_str, FALSE); /* leak! */
	args[na++] = (char *) device;
	args[na++] = mount_dir;
	args[na++] = NULL;

	/* now try to mount */
	if (!g_spawn_sync ("/",
			   args,
			   NULL,
			   0,
			   NULL,
			   NULL,
			   &sout,
			   &serr,
			   &exit_status,
			   &err)) {
		printf ("Cannot execute /bin/mount\n");
		g_unlink (cbh_path);
		g_rmdir (mount_dir);
		unknown_error ();
	}


	if (exit_status != 0) {
		char errstr[] = "mount: unknown filesystem type";

		printf ("/bin/mount error %d, stdout='%s', stderr='%s'\n", exit_status, sout, serr);

		g_unlink (cbh_path);
		g_rmdir (mount_dir);

		if (strncmp (errstr, serr, sizeof (errstr) - 1) == 0) {
			unknown_filesystem (strlen (mount_fstype) > 0 ? 
					    mount_fstype : 
					    (volume != NULL ? libhal_volume_get_fstype (volume) : "") );
		}
		unknown_error ();
	}

	dbus_error_init (&error);
	libhal_device_set_property_string (hal_ctx, udi, 
					   "info.hal_mount.created_mount_point",
					   mount_dir,
					   &error);

	dbus_error_init (&error);
	libhal_device_set_property_int (hal_ctx, udi, 
					"info.hal_mount.mounted_by_uid",
					(dbus_int32_t) atoi (invoked_by_uid),
					&error);

	g_free (sout);
	g_free (serr);
	g_free (cbh_path);
	g_free (mount_dir);
	libhal_free_string_array (allowed_options);
	g_strfreev (given_options);
}


int
main (int argc, char *argv[])
{
	char *udi;
	char *device;
	LibHalVolume *volume;
	DBusError error;
	LibHalContext *hal_ctx = NULL;
	DBusConnection *system_bus = NULL;
	LibPolKitContext *pol_ctx = NULL;
	char *invoked_by_uid;
	char *invoked_by_pid_str;
	pid_t invoked_by_pid;

	device = getenv ("HAL_PROP_BLOCK_DEVICE");
	if (device == NULL)
		usage ();

	udi = getenv ("HAL_PROP_INFO_UDI");
	if (udi == NULL)
		usage ();

	invoked_by_uid = getenv ("HAL_METHOD_INVOKED_BY_UID");

	invoked_by_pid_str = getenv ("HAL_METHOD_INVOKED_BY_PID");
	if (invoked_by_pid_str != NULL)
		invoked_by_pid = atoi (invoked_by_pid_str);
	else
		invoked_by_pid = -1;

	dbus_error_init (&error);
	if ((hal_ctx = libhal_ctx_init_direct (&error)) == NULL) {
		printf ("Cannot connect to hald\n");
		usage ();
	}

	dbus_error_init (&error);
	system_bus = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (system_bus == NULL) {
		printf ("Cannot connect to the system bus\n");
		usage ();
	}
	pol_ctx = libpolkit_new_context (system_bus);
	if (pol_ctx == NULL) {
		printf ("Cannot get libpolkit context\n");
		unknown_error ();
	}

	volume = libhal_volume_from_udi (hal_ctx, udi);
	if (volume == NULL) {
		LibHalDrive *drive;

		drive = libhal_drive_from_udi (hal_ctx, udi);
		if (drive == NULL) {
			usage ();
		} else {
			handle_mount (hal_ctx, pol_ctx, udi, NULL, drive, device, invoked_by_uid, invoked_by_pid);
		}

	} else {
		const char *drive_udi;
		LibHalDrive *drive;

		drive_udi = libhal_volume_get_storage_device_udi (volume);
		
		if (drive_udi == NULL)
			unknown_error ();
		drive = libhal_drive_from_udi (hal_ctx, drive_udi);
		if (drive == NULL)
			unknown_error ();

		handle_mount (hal_ctx, pol_ctx, udi, volume, drive, device, invoked_by_uid, invoked_by_pid);

	}

	return 0;
}


