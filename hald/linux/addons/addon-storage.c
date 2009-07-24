/***************************************************************************
 * CVSID: $Id$
 *
 * addon-storage.c : Poll storage devices for media changes
 *
 * Copyright (C) 2004 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.1
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

#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>
#include <scsi/sg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"

#include "../../logger.h"
#include "../../util_helper.h"


static char *udi;
static char *device_file;
static int media_status;
static int is_cdrom;
static int support_media_changed;
static LibHalContext *ctx = NULL;
static DBusConnection *con = NULL;
static guint poll_timer = -1;
static GMainLoop *loop;
static gboolean system_is_idle = FALSE;
static gboolean check_lock_state = TRUE;

static gboolean polling_disabled = FALSE;

static void 
force_unmount (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	DBusMessage *msg = NULL;
	DBusMessage *reply = NULL;
	char **options = NULL;
	unsigned int num_options = 0;
	DBusConnection *dbus_connection;
	char *device_file;

	dbus_connection = libhal_ctx_get_dbus_connection (ctx);

	msg = dbus_message_new_method_call ("org.freedesktop.Hal", udi,
					    "org.freedesktop.Hal.Device.Volume",
					    "Unmount");
	if (msg == NULL) {
		HAL_ERROR (("Could not create dbus message for %s", udi));
		goto out;
	}


	options = calloc (1, sizeof (char *));
	if (options == NULL) {
		HAL_ERROR (("Could not allocate options array"));
		goto out;
	}

	options[0] = "lazy";
	num_options = 1;

	device_file = libhal_device_get_property_string (ctx, udi, "block.device", NULL);
	if (device_file != NULL) {
		HAL_INFO(("forcibly attempting to lazy unmount %s as media was removed", device_file));
		libhal_free_string (device_file);
	}

	if (!dbus_message_append_args (msg, 
				       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &options, num_options,
				       DBUS_TYPE_INVALID)) {
		HAL_ERROR (("Could not append args to dbus message for %s", udi));
		goto out;
	}
	
	dbus_error_init (&error);
	if (!(reply = dbus_connection_send_with_reply_and_block (dbus_connection, msg, -1, &error))) {
		HAL_ERROR (("Unmount failed for %s: %s : %s\n", udi, error.name, error.message));
		dbus_error_free (&error);
		goto out;
	}

	if (dbus_error_is_set (&error)) {
		HAL_ERROR (("Unmount failed for %s\n%s : %s\n", udi, error.name, error.message));
		dbus_error_free (&error);
		goto out;
	}

	HAL_DEBUG (("Succesfully unmounted udi '%s'", udi));

out:
	if (options != NULL)
		free (options);
	if (msg != NULL)
		dbus_message_unref (msg);
	if (reply != NULL)
		dbus_message_unref (reply);
}

static dbus_bool_t
unmount_cleartext_devices (LibHalContext *ctx, const char *udi)
{
	DBusError error;
	char **clear_devices;
	int num_clear_devices;
	dbus_bool_t ret;

	ret = FALSE;

	/* check if the volume we back is mounted.. if it is.. unmount it */
	dbus_error_init (&error);
	clear_devices = libhal_manager_find_device_string_match (ctx,
								 "volume.crypto_luks.clear.backing_volume",
								 udi,
								 &num_clear_devices,
								 &error);

	if (clear_devices != NULL && num_clear_devices > 0) {
		int i;

		ret = TRUE;

		for (i = 0; i < num_clear_devices; i++) {
			char *clear_udi;
			clear_udi = clear_devices[i];
			LIBHAL_FREE_DBUS_ERROR (&error);
			if (libhal_device_get_property_bool (ctx, clear_udi, "volume.is_mounted", &error)) {
				HAL_DEBUG (("Forcing unmount of child '%s' (crypto)", clear_udi));
				force_unmount (ctx, clear_udi);
			}
		}
		libhal_free_string_array (clear_devices);
	}

	LIBHAL_FREE_DBUS_ERROR (&error);
	return ret;
}

static void 
unmount_childs (LibHalContext *ctx, const char *udi)
{
	int num_volumes;
	char **volumes;
	DBusError error;

	/* need to force unmount all partitions */
	dbus_error_init (&error);
	if ((volumes = libhal_manager_find_device_string_match (ctx, "block.storage_device", udi, &num_volumes, &error)) != NULL) {
		int i;

		for (i = 0; i < num_volumes; i++) {
			char *vol_udi;

			vol_udi = volumes[i];
			LIBHAL_FREE_DBUS_ERROR (&error);

			if (libhal_device_get_property_bool (ctx, vol_udi, "block.is_volume", &error)) {
				dbus_bool_t is_crypto;

				/* unmount all cleartext devices associated with us */
				is_crypto = unmount_cleartext_devices (ctx, vol_udi);

				LIBHAL_FREE_DBUS_ERROR (&error);
				if (libhal_device_get_property_bool (ctx, vol_udi, "volume.is_mounted", &error)) {
					HAL_DEBUG (("Forcing unmount of child '%s'", vol_udi));
					force_unmount (ctx, vol_udi);
				}

				/* teardown crypto */
				if (is_crypto) {
					DBusMessage *msg = NULL;
					DBusMessage *reply = NULL;

					/* tear down mapping */
					HAL_DEBUG (("Teardown crypto for '%s'", vol_udi));

					msg = dbus_message_new_method_call ("org.freedesktop.Hal", vol_udi,
									    "org.freedesktop.Hal.Device.Volume.Crypto",
									    "Teardown");
					if (msg == NULL) {
						HAL_ERROR (("Could not create dbus message for %s", vol_udi));
						goto teardown_failed;
					}

					LIBHAL_FREE_DBUS_ERROR (&error);

					if (!(reply = dbus_connection_send_with_reply_and_block (
						      libhal_ctx_get_dbus_connection (ctx), msg, -1, &error)) || 
					    dbus_error_is_set (&error)) {
						HAL_DEBUG (("Teardown failed for %s: %s : %s\n", udi, error.name, error.message));
						dbus_error_free (&error);
					}

				teardown_failed:
					if (msg != NULL)
						dbus_message_unref (msg);
					if (reply != NULL)
						dbus_message_unref (reply);
				}

			}

		}
		libhal_free_string_array (volumes);
	}
	LIBHAL_FREE_DBUS_ERROR (&error);
}

/** Check if a filesystem on a special device file is mounted
 *
 *  @param  device_file         Special device file, e.g. /dev/cdrom
 *  @return                     TRUE iff there is a filesystem system mounted
 *                              on the special device file
 */
static dbus_bool_t
is_mounted (const char *device_file)
{
	FILE *f;
	dbus_bool_t rc;
	struct mntent mnt;
	char buf[512];

	rc = FALSE;

	if ((f = setmntent ("/etc/mtab", "r")) == NULL)
		goto out;

	while (getmntent_r (f, &mnt, buf, sizeof(buf)) != NULL) {
		if (strcmp (device_file, mnt.mnt_fsname) == 0) {
			rc = TRUE;
			goto out1;
		}
	}

out1:
	endmntent (f);
out:
	return rc;
}


enum {
	MEDIA_STATUS_UNKNOWN = 0,
	MEDIA_STATUS_GOT_MEDIA = 1,
	MEDIA_STATUS_NO_MEDIA = 2
};

static gboolean poll_for_media (gpointer user_data);
static gboolean poll_for_media_force (void);

static int interval_in_seconds = 2;

static gboolean is_locked_by_hal = FALSE;
static gboolean is_locked_via_o_excl = FALSE;

static void
update_proc_title (void)
{

        if (polling_disabled) {
                hal_set_proc_title ("hald-addon-storage: no polling on %s because it is explicitly disabled", device_file);
        } else if (is_locked_by_hal) {
                if (is_locked_via_o_excl) {
                        hal_set_proc_title ("hald-addon-storage: no polling because %s is locked via HAL and O_EXCL", device_file);
                } else {
                        hal_set_proc_title ("hald-addon-storage: no polling because %s is locked via HAL", device_file);
                }
        } else if (is_locked_via_o_excl) {
                hal_set_proc_title ("hald-addon-storage: no polling because %s is locked via O_EXCL", device_file);
        } else {
                hal_set_proc_title ("hald-addon-storage: polling %s (every %d sec)", device_file, interval_in_seconds);
        }
}

static void
update_polling_interval (void)
{
	/* TODO: ideally we want all things that do polling to do it
	 * at the same time.. such as to minimize battery
	 * usage. Suppose we want to wake up to do poll_for_media()
	 * every N seconds.  Suppose M is the number of seconds since
	 * epoch. The fix is to wake up at exactly when M is divisible
	 * by N... plus some system wide offset (ideally read from
	 * /sys so kernel threads can use the same offset) to make Xen
	 * + friends work.... 
	 *
	 * Also, the polling intervals should be powers of two to
	 * ensure that wakeup's with different intervals happen at the
	 * same time when possible.
	 *
	 * This is sorta-kinda what g_timeout_source_new_seconds()
	 * tries to achieve, not enough I think, and it's only
	 * available in glib >= 2.14. Too little, too late? *shrug*
	 */

	if (system_is_idle)
		interval_in_seconds = 16;
	else
		interval_in_seconds = 2;

	if (poll_timer > 0)
		g_source_remove (poll_timer);

#ifdef HAVE_GLIB_2_14
	poll_timer = g_timeout_add_seconds (interval_in_seconds, poll_for_media, NULL);
#else
	poll_timer = g_timeout_add (interval_in_seconds * 1000, poll_for_media, NULL);
#endif

        update_proc_title ();
}

static gboolean
poll_for_media (gpointer user_data)
{
        if (check_lock_state) {
                DBusError error;
                dbus_bool_t should_poll;

                check_lock_state = FALSE;

                HAL_INFO (("Checking whether device %s is locked on HAL", device_file));
                dbus_error_init (&error);
                if (libhal_device_is_locked_by_others (ctx, udi, "org.freedesktop.Hal.Device.Storage", &error)) {
                        HAL_INFO (("... device %s is locked on HAL", device_file));
                        is_locked_by_hal = TRUE;
                        update_proc_title ();
			LIBHAL_FREE_DBUS_ERROR (&error);
                        goto skip_check;
                } else {
                        HAL_INFO (("... device %s is not locked on HAL", device_file));
                        is_locked_by_hal = FALSE;
                }

		LIBHAL_FREE_DBUS_ERROR (&error);

                should_poll = libhal_device_get_property_bool (ctx, udi, "storage.media_check_enabled", &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		polling_disabled = !should_poll;
		update_proc_title ();
        }

        /* TODO: we could remove the timeout completely... */
        if (is_locked_by_hal || polling_disabled)
                goto skip_check;

        poll_for_media_force ();

skip_check:
	return TRUE;
}

/* returns: whether the state changed */
static gboolean
poll_for_media_force (void)
{
        int fd;
        int got_media;
        int old_media_status;

	got_media = FALSE;

        old_media_status = media_status;
	if (is_cdrom) {
		int drive;
		
		fd = open (device_file, O_RDONLY | O_NONBLOCK | O_EXCL);
		
		if (fd < 0 && errno == EBUSY) {
			/* this means the disc is mounted or some other app,
			 * like a cd burner, has already opened O_EXCL */
			
			/* HOWEVER, when starting hald, a disc may be
			 * mounted; so check /etc/mtab to see if it
			 * actually is mounted. If it is we retry to open
			 * without O_EXCL
			 */
			if (!is_mounted (device_file)) {
                                if (!is_locked_via_o_excl) {
                                        is_locked_via_o_excl = TRUE;
                                        update_proc_title ();
                                } else {
                                        is_locked_via_o_excl = TRUE;
                                }
				goto skip_check;
                        }
			
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
		}
		
		if (fd < 0) {
			HAL_ERROR (("open failed for %s: %s", device_file, strerror (errno))); 
			goto skip_check;
		}

                if (is_locked_via_o_excl) {
                        is_locked_via_o_excl = FALSE;
                        update_proc_title ();
                }
		
		
		/* Check if a disc is in the drive
		 *
		 * @todo Use MMC-2 API if applicable
		 */
		drive = ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
		switch (drive) {
		case CDS_NO_INFO:
		case CDS_NO_DISC:
		case CDS_TRAY_OPEN:
		case CDS_DRIVE_NOT_READY:
			break;
			
		case CDS_DISC_OK:
			/* some CD-ROMs report CDS_DISK_OK even with an open
			 * tray; if media check has the same value two times in
			 * a row then this seems to be the case and we must not
			 * report that there is a media in it. */
			if (support_media_changed &&
			    ioctl (fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT) && 
			    ioctl (fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT)) {
			} else {
				got_media = TRUE;
			}
			break;
			
		case -1:
			HAL_ERROR (("CDROM_DRIVE_STATUS failed: %s\n", strerror(errno)));
			break;
			
		default:
			break;
		}
		
		/* check if eject button was pressed */
		if (got_media) {
			unsigned char cdb[10] = { 0x4a, 1, 0, 0, 16, 0, 0, 0, 8, 0};
			unsigned char buffer[8];
			struct sg_io_hdr sg_h;
			int retval;
			
			memset(buffer, 0, sizeof(buffer));
			memset(&sg_h, 0, sizeof(struct sg_io_hdr));
			sg_h.interface_id = 'S';
			sg_h.cmd_len = sizeof(cdb);
			sg_h.dxfer_direction = SG_DXFER_FROM_DEV;
			sg_h.dxfer_len = sizeof(buffer);
			sg_h.dxferp = buffer;
			sg_h.cmdp = cdb;
			sg_h.timeout = 5000;
			retval = ioctl(fd, SG_IO, &sg_h);
			if (retval == 0 && sg_h.status == 0 && (buffer[4] & 0x0f) == 0x01) {
				DBusError error;
				
				/* emit signal from drive device object */
				dbus_error_init (&error);
				libhal_device_emit_condition (ctx, udi, "EjectPressed", "", &error);
				LIBHAL_FREE_DBUS_ERROR (&error);
			}
		}
		close (fd);
	} else {
		fd = open (device_file, O_RDONLY);
		if (fd < 0 && errno == ENOMEDIUM) {
			got_media = FALSE;
		} else if (fd >= 0) {
			got_media = TRUE;
			close (fd);
		} else {
			HAL_ERROR (("open failed for %s: %s", device_file, strerror (errno))); 
			goto skip_check;
		}
	}
	
	/* set correct state on startup, this avoid endless loops if there was a media in the device on startup */
	if (media_status == MEDIA_STATUS_UNKNOWN) {
		if (got_media) 
			media_status = MEDIA_STATUS_NO_MEDIA;
		else 
			media_status = MEDIA_STATUS_GOT_MEDIA;	
	}

	switch (media_status) {
	case MEDIA_STATUS_GOT_MEDIA:
		if (!got_media) {
			DBusError error;
			
			HAL_DEBUG (("Media removal detected on %s", device_file));
			libhal_device_set_property_bool (ctx, udi, "storage.removable.media_available", FALSE, NULL);
			libhal_device_set_property_string (ctx, udi, "storage.partitioning_scheme", "", NULL);
			
			
			/* attempt to unmount all childs */
			unmount_childs (ctx, udi);
			
			/* could have a fs on the main block device; do a rescan to remove it */
			dbus_error_init (&error);
			libhal_device_rescan (ctx, udi, &error);
			LIBHAL_FREE_DBUS_ERROR (&error);
			
			/* have to this to trigger appropriate hotplug events */
			fd = open (device_file, O_RDONLY | O_NONBLOCK);
			if (fd >= 0) {
				ioctl (fd, BLKRRPART);
				close (fd);
			}
		}
		break;
		
	case MEDIA_STATUS_NO_MEDIA:
		if (got_media) {
			DBusError error;
			
			HAL_DEBUG (("Media insertion detected on %s", device_file));
			
			/* our probe will trigger the appropriate hotplug events */
			libhal_device_set_property_bool (
				ctx, udi, "storage.removable.media_available", TRUE, NULL);
			
			/* could have a fs on the main block device; do a rescan to add it */
			dbus_error_init (&error);
			libhal_device_rescan (ctx, udi, &error);
			LIBHAL_FREE_DBUS_ERROR (&error);
		}
		break;
		
	case MEDIA_STATUS_UNKNOWN:
	default:
		break;
	}
	
	/* update our current status */
	if (got_media)
		media_status = MEDIA_STATUS_GOT_MEDIA;
	else
		media_status = MEDIA_STATUS_NO_MEDIA;
	
	/*HAL_DEBUG (("polling %s; got media=%d", device_file, got_media));*/
	
skip_check:
	return old_media_status != media_status;
}

#ifdef HAVE_CONKIT
static gboolean
get_system_idle_from_ck (void)
{
       gboolean ret;
       DBusError error;
       DBusMessage *message;
       DBusMessage *reply;

       ret = FALSE;

       message = dbus_message_new_method_call ("org.freedesktop.ConsoleKit", 
                                               "/org/freedesktop/ConsoleKit/Manager",
                                               "org.freedesktop.ConsoleKit.Manager",
                                               "GetSystemIdleHint");
       dbus_error_init (&error);
       reply = dbus_connection_send_with_reply_and_block (con, message, -1, &error);
       if (reply == NULL || dbus_error_is_set (&error)) {
               HAL_ERROR (("Error doing Manager.GetSystemIdleHint on ConsoleKit: %s: %s", error.name, error.message));
               dbus_message_unref (message);
               if (reply != NULL)
                       dbus_message_unref (reply);
               goto error;
       }
       if (!dbus_message_get_args (reply, NULL,
                                   DBUS_TYPE_BOOLEAN, &(system_is_idle),
                                   DBUS_TYPE_INVALID)) {
               HAL_ERROR (("Invalid GetSystemIdleHint reply from CK"));
               goto error;
       }
       dbus_message_unref (message);
       dbus_message_unref (reply);

       ret = TRUE;

error:
       LIBHAL_FREE_DBUS_ERROR (&error);
       return ret;
}
#endif /* HAVE_CONKIT */


static DBusHandlerResult
direct_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	if (dbus_message_is_method_call (message, 
					 "org.freedesktop.Hal.Device.Storage.Removable", 
					 "CheckForMedia")) {
                DBusMessage *reply;
                dbus_bool_t call_had_sideeffect;

                HAL_INFO (("Forcing poll for media becusse CheckForMedia() was called"));

                call_had_sideeffect = poll_for_media_force ();

                reply = dbus_message_new_method_return (message);
                dbus_message_append_args (reply,
                                          DBUS_TYPE_BOOLEAN, &call_had_sideeffect,
                                          DBUS_TYPE_INVALID);
                dbus_connection_send (connection, reply, NULL);
                dbus_message_unref (reply);
        }

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
dbus_filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
#ifdef HAVE_CONKIT
	gboolean system_is_idle_new;

	if (dbus_message_is_signal (message, 
				    "org.freedesktop.ConsoleKit.Manager", 
				    "SystemIdleHintChanged")) {
		if (!dbus_message_get_args (message, NULL,
					    DBUS_TYPE_BOOLEAN, &system_is_idle_new,
					    DBUS_TYPE_INVALID)) {
			HAL_ERROR (("Invalid SystemIdleHintChanged signal from CK"));
			goto out;
		}

		if (system_is_idle_new != system_is_idle) {
			system_is_idle = system_is_idle_new;
			update_polling_interval ();
		}
	}

out:
#endif /* HAVE_CONKIT */

        /* Check, just before the next poll, whether lock state have changed; 
         * 
         * Note that we get called on at least these signals
         *
         * 1. CK.Manager  - SystemIdleHintChanged
         * 2. CK.Seat     - ActiveSessionChanged
         * 2. HAL.Manager - GlobalLockAcquired, GlobalLockReleased
         * 3. HAL.Device  - LockAcquired, LockReleased
         *
         * meaning that every time the locking situation changes, we
         * will get updated.
         */
        check_lock_state = TRUE;

	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
	DBusError error;
        LibHalContext *ctx_direct;
        DBusConnection *con_direct;
	char *bus;
	char *drive_type;
	char *support_media_changed_str;
        char *str;

	hal_set_proc_title_init (argc, argv);

	/* We could drop privs if we know that the haldaemon user is
	 * to be able to access block devices...
	 */
        /*drop_privileges (1);*/

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
	if ((bus = getenv ("HAL_PROP_STORAGE_BUS")) == NULL)
		goto out;
	if ((drive_type = getenv ("HAL_PROP_STORAGE_DRIVE_TYPE")) == NULL)
		goto out;

	setup_logger ();

	support_media_changed_str = getenv ("HAL_PROP_STORAGE_CDROM_SUPPORT_MEDIA_CHANGED");
	if (support_media_changed_str != NULL && strcmp (support_media_changed_str, "true") == 0)
		support_media_changed = TRUE;
	else
		support_media_changed = FALSE;

	dbus_error_init (&error);
	con = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (con == NULL) {
		HAL_ERROR (("Cannot connect to system bus"));
		goto out;
	}
	loop = g_main_loop_new (NULL, FALSE);
	dbus_connection_setup_with_g_main (con, NULL);
	dbus_connection_set_exit_on_disconnect (con, 0);

	if ((ctx_direct = libhal_ctx_init_direct (&error)) == NULL) {
		HAL_ERROR (("Cannot connect to hald"));
                goto out;
	}
	if (!libhal_device_addon_is_ready (ctx_direct, udi, &error)) {
                goto out;
	}
	con_direct = libhal_ctx_get_dbus_connection (ctx_direct);
	dbus_connection_setup_with_g_main (con_direct, NULL);
	dbus_connection_set_exit_on_disconnect (con_direct, 0);
	dbus_connection_add_filter (con_direct, direct_filter_function, NULL, NULL);


	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	if (!libhal_device_addon_is_ready (ctx, udi, &error))
		goto out;

	HAL_DEBUG (("**************************************************"));
	HAL_DEBUG (("Doing addon-storage for %s (bus %s) (drive_type %s) (udi %s)", device_file, bus, drive_type, udi));
	HAL_DEBUG (("**************************************************"));

	if (strcmp (drive_type, "cdrom") == 0)
		is_cdrom = 1;
	else
		is_cdrom = 0;

	media_status = MEDIA_STATUS_UNKNOWN;


#ifdef HAVE_CONKIT
	/* TODO: ideally we should track the sessions on the seats on
	 * which the device belongs to. But right now we don't really
	 * do multi-seat so I'm going to punt on this for now.
	 */
	get_system_idle_from_ck ();

	dbus_bus_add_match (con,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Manager'"
			    ",sender='org.freedesktop.ConsoleKit'"
                            ",member='SystemIdleHintChanged'",
			    NULL);
	dbus_bus_add_match (con,
			    "type='signal'"
			    ",interface='org.freedesktop.ConsoleKit.Seat'"
			    ",sender='org.freedesktop.ConsoleKit'"
                            ",member='ActiveSessionChanged'",
			    NULL);
#endif

        /* this is a bit weird; but we want to listen to signals about
         * locking from hald.. and signals are not pushed over direct
         * connections (for a good reason).
         */
	dbus_bus_add_match (con,
			    "type='signal'"
			    ",interface='org.freedesktop.Hal.Manager'"
			    ",sender='org.freedesktop.Hal'",
			    NULL);
	dbus_bus_add_match (con,
			    "type='signal'"
			    ",interface='org.freedesktop.Hal.Manager'"
			    ",sender='org.freedesktop.Hal'",
			    NULL);
        str = g_strdup_printf ("type='signal'"
                               ",interface='org.freedesktop.Hal.Device'"
                               ",sender='org.freedesktop.Hal'"
                               ",path='%s'",
                               udi);
	dbus_bus_add_match (con,
                            str,
			    NULL);
        g_free (str);
	dbus_connection_add_filter (con, dbus_filter_function, NULL, NULL);

	if (!libhal_device_claim_interface (ctx,
					    udi, 
					    "org.freedesktop.Hal.Device.Storage.Removable", 
					    "    <method name=\"CheckForMedia\">\n"
					    "      <arg name=\"call_had_sideeffect\" direction=\"out\" type=\"b\"/>\n"
					    "    </method>\n",
					    &error)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.Storage.Removable'"));
                goto out;
	}


	update_polling_interval ();
	g_main_loop_run (loop);

out:
	HAL_DEBUG (("An error occured, exiting cleanly"));

	LIBHAL_FREE_DBUS_ERROR (&error);

	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return 0;
}
