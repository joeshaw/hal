/***************************************************************************
 *
 * hal-acl-tool.c : Manage ACL's on device nodes
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include <glib.h>
#include <libhal.h>
#include <polkit/polkit.h>

/* How this works (or "An introduction to this code")
 *
 * - all ACL's granted by this tool is kept in /var/run/hald/acl-list
 *   See below for the format.
 *
 * - every time tool is launched we read this file and keep each line
 *   as an ACLCurrent instance. These are kept in a list.
 *
 * - we do locking so only one instance of this tool is doing work
 *   at one time. This is essential as we maintain ACL's in a file.
 *
 * - there's an overarching --reconfigure method that basically
 *   - finds all devices of capability 'access_control'
 *   - computes what ACL's these devices should have
 *   - modifies the in-memory acl-current-list
 *     - ACL's to be removed are tagged with setting remove -> TRUE
 *     - ACL's to be added are appended to the list and add -> TRUE
 *   - we then compute the argument vector to setfacl(1) for adding /
 *     removing ACL's
 *   - we emit signals ACLAdded and ACLRemoved
 *   - if setfacl(1) succeeds (rc == 0) then we write the new acl-current-list
 *
 * Notably, the HAL daemon will invoke us with --reconfigure on every
 *  - session add
 *  - session remove
 *  - session inactive
 *  - session active
 *
 * event. Also, when devices are added we're invoked with --add-device
 * respectively --remove-device. When the HAL daemon starts we're invoked
 * with --remove-all.
 *
 * Optimizations
 *
 * - the HAL daemon exports the ConsoleKit seat + session
 *   configuration in CK_* environment variables. So we don't
 *   need to do IPC to the ConsoleKit daemon to learn about
 *   about seats and sessions.
 *
 * - special casing for --add-device and --remove-device; here we
 *   don't need roundtrips to the HAL daemon. Only --reconfigure
 *   requires that. As such no IPC is required for these cases
 *   which is fortunate as --add-device is invoked quite a lot
 *   on startup
 *
 */

/* Each entry here represents a line in the /var/run/hald/acl-list file
 * of ACL's that have been set by HAL and as such are currently
 * applied
 *
 *   <device-file>    <udi>    <type>    <uid-or-gid>
 *
 * where <type>='u'|'g' for respectively uid and gid (the spacing represent tabs).
 *
 * Example:
 *
 *   /dev/snd/controlC0    /org/freedesktop/Hal/devices/pci_8086_27d8_alsa_control__1    u    500
 *   /dev/snd/controlC0    /org/freedesktop/Hal/devices/pci_8086_27d8_alsa_control__1    u    501
 *   /dev/snd/controlC0    /org/freedesktop/Hal/devices/pci_8086_27d8_alsa_control__1    g    1001
 */
typedef struct ACLCurrent_s {
        char *udi;
	char *device;
	int type;
	union {
		uid_t uid;
		gid_t gid;
	} v;
	gboolean remove;
	gboolean add;
} ACLCurrent;

/* used for type member in ACLCurrent */
enum {
	HAL_ACL_UID,
	HAL_ACL_GID
};

static PolKitContext *pk_context = NULL;

static LibHalContext *hal_ctx = NULL;

static gboolean in_device_added = FALSE;

static gboolean
ensure_hal_ctx (void)
{
        gboolean ret;
	DBusError error;

        ret = FALSE;
        if (hal_ctx != NULL) {
                ret = TRUE;
                goto out;
        }

	dbus_error_init (&error);
	if ((hal_ctx = libhal_ctx_init_direct (&error)) == NULL) {
		printf ("%d: Cannot connect to hald: %s: %s\n", getpid (), error.name, error.message);
		LIBHAL_FREE_DBUS_ERROR (&error);
                goto out;
	}

        ret = TRUE;

out:
        return ret;
}

static void
hal_acl_free (ACLCurrent *ha)
{
        g_free (ha->udi);
	g_free (ha->device);
	g_free (ha);
}

static int
ha_sort (ACLCurrent *a, ACLCurrent *b)
{
	return strcmp (a->device, b->device);
}

static gboolean
acl_apply_changes (GSList *new_acl_list, gboolean only_update_acllist, gboolean missing_files_ok)
{
	GString *str;
	GString *setfacl_cmdline;
	char *new_acl_file_contents = NULL;
	char *setfacl_cmdline_str = NULL;
	GSList *i;
	gboolean ret;
	GError *error = NULL;
	int exit_status;
        gboolean messages_were_sent = FALSE;

	ret = FALSE;

	new_acl_list = g_slist_sort (new_acl_list, (GCompareFunc) ha_sort);

	/* first compute the contents of the new acl-file 
	 * and build up the command line for setfacl(1)
	 */
	str = g_string_new ("");
	setfacl_cmdline = g_string_new ("");
	for (i = new_acl_list; i != NULL; i = g_slist_next (i)) {
		ACLCurrent *ha = (ACLCurrent *) i->data;

		if (ha->remove) {
			if (setfacl_cmdline->len > 0) {
				g_string_append_c (setfacl_cmdline, ' ');
			}
			g_string_append_printf (setfacl_cmdline, "-x %c:%d %s",
						(ha->type == HAL_ACL_UID) ? 'u' : 'g',
						(ha->type == HAL_ACL_UID) ? ha->v.uid : ha->v.gid,
						ha->device);
		}

		if (ha->add) {
			if (setfacl_cmdline->len > 0) {
				g_string_append_c (setfacl_cmdline, ' ');
			}
			g_string_append_printf (setfacl_cmdline, "-m %c:%d:rw %s",
						(ha->type == HAL_ACL_UID) ? 'u' : 'g',
						(ha->type == HAL_ACL_UID) ? ha->v.uid : ha->v.gid,
						ha->device);

		}

                if (!ha->remove) {
                        g_string_append_printf (str, 
                                                "%s\t%s\t%c\t%d\n",
                                                ha->device,
                                                ha->udi,
                                                (ha->type == HAL_ACL_UID) ? 'u' : 'g',
                                                (ha->type == HAL_ACL_UID) ? ha->v.uid : ha->v.gid);
                }
	}
	new_acl_file_contents = g_string_free (str, FALSE);

	/* TODO FIXME NOTE XXX WARNING: 
	 *
	 * The variable 'only_update_acllist' is set to TRUE only on
	 * device_remove events. It effectively means "only update the
	 * /var/run/hald/acl-list, do not apply ACL's on disk". So this
	 * is done for systems where /dev is dynamic and we know for
	 * sure that the device file is gone. 
	 *
	 * So if you're running a static /dev you need to comment the
	 * only_update_acllist conditional out below so the ACL's are
	 * removed accordingly. As this is uncommon and against all
	 * recommendations for a system using HAL (we depend on recent
	 * udev versions!) we don't bother the syscall overhead of
	 * stat(2)'ing every file.
	 */

	if (setfacl_cmdline->len == 0 || only_update_acllist) {
		g_string_free (setfacl_cmdline, TRUE);
		setfacl_cmdline_str = NULL;
	} else {
		g_string_prepend (setfacl_cmdline, "setfacl ");
		setfacl_cmdline_str = g_string_free (setfacl_cmdline, FALSE);

		printf ("%d: invoking '%s'\n", getpid (), setfacl_cmdline_str);
		if (!g_spawn_command_line_sync (setfacl_cmdline_str, NULL, NULL, &exit_status, &error)) {
			printf ("%d: Error - couldn't invoke setfacl(1): %s\n", getpid (), error->message);
			g_error_free (error);
			goto out;
		}

		if (exit_status != 0) {
			if (!missing_files_ok) {
				printf ("%d: setfacl(1) exit code != 0 but OK as some missing files are expected\n",
					getpid ());
			} else {
				printf ("%d: Error - setfacl(1) failed\n", getpid ());
				goto out;
			}
		}
	}

	/* success; now atomically set the new list */
	g_file_set_contents (PACKAGE_LOCALSTATEDIR "/run/hald/acl-list", 
			     new_acl_file_contents, 
			     strlen (new_acl_file_contents),
			     NULL);

        /* now that we've added/removed ACL's: emit D-Bus signals.. we need to do this _after_ 
         * having updated the ACL - to avoid possible race conditions 
         */
        messages_were_sent = FALSE;
	for (i = new_acl_list; i != NULL; i = g_slist_next (i)) {
		ACLCurrent *ha = (ACLCurrent *) i->data;

                /* emit signal ACLGranted or ACLRemoved - but avoid doing it on device add because the device
                 * object doesn't exist just yet
                 */
                if (ha->type == HAL_ACL_UID && ha->udi != NULL && (ha->add || ha->remove) && !in_device_added) {
                        DBusMessage *message;
                        DBusConnection *connection;

                        printf ("Emmitting %s for udi %s, uid %d\n", 
                                ha->add ? "ACLAdded" : "ACLRemoved",
                                ha->udi,
                                ha->v.uid);

                        if (!ensure_hal_ctx ()) {
				printf ("%d: cannot get libhal context\n", getpid ());
                                goto no_dbus_signal;
                        }

                        connection = libhal_ctx_get_dbus_connection (hal_ctx);
                        if (connection == NULL) {
				printf ("%d: cannot get D-Bus connection\n", getpid());
                                goto no_dbus_signal;
                        }

                        message = dbus_message_new_signal (ha->udi,
                                                           "org.freedesktop.Hal.Device.AccessControl",
                                                           ha->add ? "ACLAdded" : "ACLRemoved");
                        if (message == NULL) {
				printf ("%d: cannot create message\n", getpid());
                                goto no_dbus_signal;
                        }

                        if (!dbus_message_append_args (message, 
                                                       DBUS_TYPE_UINT32, &(ha->v.uid),
                                                       DBUS_TYPE_INVALID)) {
				printf ("%d: cannot append to message\n", getpid());
                                dbus_message_unref (message);
                                goto no_dbus_signal;
                        }

                        if (!dbus_connection_send (connection, message, NULL)) {
				printf ("%d: cannot send message\n", getpid());
                                dbus_message_unref (message);
                                goto no_dbus_signal;
                        }
                        dbus_message_unref (message);

                        messages_were_sent = TRUE;
                }

        no_dbus_signal:
                ;
        }

        /* apparently we need to flush the signals - otherwise they
         * may be lost because we're exiting right after this
         */
        if (messages_were_sent) {
                if (ensure_hal_ctx ()) {
                        DBusConnection *connection;
                        connection = libhal_ctx_get_dbus_connection (hal_ctx);
                        if (connection != NULL) {
                                dbus_connection_flush (connection);
                        }
                }
        }

	ret = TRUE;

out:
	if (new_acl_file_contents != NULL)
		g_free (new_acl_file_contents);

	if (setfacl_cmdline_str != NULL)
		g_free (setfacl_cmdline_str);

	return ret;
}


static gboolean
get_current_acl_list (GSList **l)
{
	FILE *f;
	gboolean ret;
	char buf[1024];

	*l = NULL;
	f = NULL;
	ret = FALSE;

	f = fopen (PACKAGE_LOCALSTATEDIR "/run/hald/acl-list", "r");
	if (f == NULL) {
		printf ("%d: cannot open " PACKAGE_LOCALSTATEDIR "/run/hald/acl-list\n", getpid ());
		goto out;
	}

	while (fgets (buf, sizeof(buf), f) != NULL) {
		ACLCurrent *ha;
		char **val;
		char *endptr;
                gboolean old_format = FALSE;
                int n;

                /* supporting the old format is important; because at hald startup we call hal-acl-tool --remove-all */

		ha = g_new0 (ACLCurrent, 1);
		val = g_strsplit(buf, "\t", 0);
		if (g_strv_length (val) == 3) {
			printf ("Line is from old format: '%s'\n", buf);
                        old_format = TRUE;
		} else {
                        if (g_strv_length (val) != 4) {
                                printf ("Line is malformed: '%s'\n", buf);
                                g_strfreev (val);
                                goto out;
                        }
                }

                n = 0;

		ha->device = g_strdup (val[n++]);

                if (old_format) {
                        ha->udi = NULL;
                } else {
                        ha->udi = g_strdup (val[n++]);
                }

		if (strcmp (val[n], "u") == 0) {
			ha->type = HAL_ACL_UID;
			ha->v.uid = strtol (val[n + 1], &endptr, 10);
			if (*endptr != '\0' && *endptr != '\n') {
				printf ("Line is malformed: '%s'\n", buf);
				g_strfreev (val);
				goto out;
			}
		} else if (strcmp (val[n], "g") == 0) {
			ha->type = HAL_ACL_GID;
			ha->v.gid = strtol (val[n + 1], &endptr, 10);
			if (*endptr != '\0' && *endptr != '\n') {
				printf ("Line is malformed: '%s'\n", buf);
				g_strfreev (val);
				goto out;
			}
		} else {
			printf ("Line is malformed: '%s'\n", buf);
			g_strfreev (val);
			goto out;
		}
		g_strfreev (val);

		/*printf ("  ACL '%s' %d %d\n", ha->device, ha->type, ha->v.uid);*/

		*l = g_slist_prepend (*l, ha);
	}

	ret = TRUE;

out:
	if (f != NULL)
		fclose (f);
	return ret;
}

typedef void (*SeatSessionVisitor) (const char *seat_id, 
				    int num_sessions_on_seat,
				    const char *session_id,     /* may be NULL */
				    uid_t session_uid,
				    gboolean seat_is_local,
				    gboolean session_is_active, 
				    gpointer user_data);

/* Visits all seats and sessions.
 *
 * NOTE: when a seat is visited session_id will be NULL and session_uid, session_is_active are undefined.
 */
static gboolean
visit_seats_and_sessions (SeatSessionVisitor visitor_cb, gpointer user_data)
{
	int i;
	int j;
	char *s;
	char *p;
	char **seats;
	gboolean ret;

	ret = FALSE;

	if ((s = getenv ("CK_SEATS")) == NULL) {
		printf ("%d: CK_SEATS is not set!\n", getpid());
		goto out;
	}
	seats = g_strsplit (s, "\t", 0);
	/* for all seats */
	for (i = 0; seats[i] != NULL; i++) {
		char *seat = seats[i];
                char *seat_id;
		char **sessions;
		int num_sessions_on_seat;

		p = g_strdup_printf ("CK_SEAT_%s", seat);
		if ((s = getenv (p)) == NULL) {
			printf ("%d: CK_SEAT_%s is not set!\n", getpid(), seat);
			g_free (p);
			goto out;
		}
		g_free (p);
		sessions = g_strsplit (s, "\t", 0);
		num_sessions_on_seat = g_strv_length (sessions);

                seat_id = g_strdup_printf ("/org/freedesktop/ConsoleKit/%s", seat);
		visitor_cb (seat_id, num_sessions_on_seat, NULL, 0, FALSE, FALSE, user_data);

		/* for all sessions on seat */
		for (j = 0; sessions[j] != NULL; j++) {
			char *session = sessions[j];
                        char *session_id;
			gboolean session_is_local;
			gboolean session_is_active;
			uid_t session_uid;
			char *endptr;

			p = g_strdup_printf ("CK_SESSION_IS_LOCAL_%s", session);
			if ((s = getenv (p)) == NULL) {
				printf ("%d: CK_SESSION_IS_LOCAL_%s is not set!\n", getpid(), session);
				g_free (p);
				goto out;
			}
			g_free (p);
			session_is_local = (strcmp (s, "true") == 0);

			p = g_strdup_printf ("CK_SESSION_IS_ACTIVE_%s", session);
			if ((s = getenv (p)) == NULL) {
				printf ("%d: CK_SESSION_IS_ACTIVE_%s is not set!\n", getpid(), session);
				g_free (p);
				goto out;
			}
			g_free (p);
			session_is_active = (strcmp (s, "true") == 0);

			p = g_strdup_printf ("CK_SESSION_UID_%s", session);
			if ((s = getenv (p)) == NULL) {
				printf ("%d: CK_SESSION_UID_%s is not set!\n", getpid(), session);
				g_free (p);
				goto out;
			}
			g_free (p);
			session_uid = strtol (s, &endptr, 10);
			if (*endptr != '\0') {
				printf ("%d: CK_SESSION_UID_%s set to '%s' is malformed!\n", getpid(), session, s);
				goto out;
			}

                        session_id = g_strdup_printf ("/org/freedesktop/ConsoleKit/%s", session);

			visitor_cb (seat_id, num_sessions_on_seat, 
				    session_id, session_uid, session_is_local, session_is_active, user_data);

                        g_free (session_id);
		}
		g_strfreev (sessions);
                g_free (seat_id);
	}
	g_strfreev (seats);

	ret = TRUE;

out:
	return ret;
}

/* this data structure is for collecting what ACL's a device should have */
typedef struct {
	/* identifying the device */
	char *udi;      /* HAL UDI of device */
	char *device;   /* device file */
        char *type;     /* type of device, access_control.type - used by PolicyKit */

	/* will be set by the visitor */
	GSList *uid;    /* list of uid's (int) that should have access to this device */
	GSList *gid;    /* list of gid's (int) that should have access to this device */
} ACLForDevice;

static void
afd_grant_to_uid (ACLForDevice *afd, uid_t uid)
{
	GSList *i;

	for (i = afd->uid; i != NULL; i = g_slist_next (i)) {
		uid_t iuid = GPOINTER_TO_INT (i->data);
		if (uid == iuid)
			goto out;
	}
	afd->uid = g_slist_prepend (afd->uid, GINT_TO_POINTER (uid));
out:
	;
}

static void
afd_grant_to_gid (ACLForDevice *afd, gid_t gid)
{
	GSList *i;

	for (i = afd->gid; i != NULL; i = g_slist_next (i)) {
		gid_t igid = GPOINTER_TO_INT (i->data);
		if (gid == igid)
			goto out;
	}
	afd->gid = g_slist_prepend (afd->gid, GINT_TO_POINTER (gid));
out:
	;
}

static uid_t
util_name_to_uid (const char *username, gid_t *default_gid)
{
        int rc;
        uid_t res;
        char *buf = NULL;
        unsigned int bufsize;
        struct passwd pwd;
        struct passwd *pwdp;

        res = (uid_t) -1;

        bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
        buf = g_new0 (char, bufsize);
                
        rc = getpwnam_r (username, &pwd, buf, bufsize, &pwdp);
        if (rc != 0 || pwdp == NULL) {
                /*g_warning ("getpwnam_r() returned %d", rc);*/
                goto out;
        }

        res = pwdp->pw_uid;
        if (default_gid != NULL)
                *default_gid = pwdp->pw_gid;

out:
        g_free (buf);
        return res;
}

static gid_t 
util_name_to_gid (const char *groupname)
{
        int rc;
        gid_t res;
        char *buf = NULL;
        unsigned int bufsize;
        struct group gbuf;
        struct group *gbufp;

        res = (gid_t) -1;

        bufsize = sysconf (_SC_GETGR_R_SIZE_MAX);
        buf = g_new0 (char, bufsize);
                
        rc = getgrnam_r (groupname, &gbuf, buf, bufsize, &gbufp);
        if (rc != 0 || gbufp == NULL) {
                /*g_warning ("getgrnam_r() returned %d", rc);*/
                goto out;
        }

        res = gbufp->gr_gid;

out:
        g_free (buf);
        return res;
}

static void
afd_grant_to_uid_from_userlist (ACLForDevice *afd, char **sv)
{
	int i;
	uid_t uid;
	char *endptr;

	for (i = 0; sv[i] != NULL; i++) {
		uid = strtol (sv[i], &endptr, 10);
		if (*endptr != '\0') {
			uid = util_name_to_uid (sv[i], NULL);
			if ((int) uid == -1) {
				printf ("%d: warning; username '%s' is unknown\n", getpid (), sv[i]);
				continue;
			}
		}
		afd_grant_to_uid (afd, uid);
	}
}

static void
afd_grant_to_gid_from_grouplist (ACLForDevice *afd, char **sv)
{
	int i;
	gid_t gid;
	char *endptr;

	for (i = 0; sv[i] != NULL; i++) {
		gid = strtol (sv[i], &endptr, 10);
		if (*endptr != '\0') {
			gid = util_name_to_gid (sv[i]);
			if ((int) gid == -1) {
				printf ("%d: warning; group '%s' is unknown\n", getpid (), sv[i]);
				continue;
			}
		}
		afd_grant_to_gid (afd, gid);
	}
}

static ACLForDevice *
acl_for_device_new (const char *udi)
{
	ACLForDevice *afd;

	afd = g_new0 (ACLForDevice, 1);
	afd->udi = g_strdup (udi);

	return afd;
}

static void
acl_for_device_set_device (ACLForDevice *afd, const char *device)
{
	afd->device = g_strdup (device);
}

static void
acl_for_device_set_type (ACLForDevice *afd, const char *type)
{
	afd->type = g_strdup (type);
}

static void
acl_for_device_free (ACLForDevice* afd)
{
	g_free (afd->udi);
	g_free (afd->device);
	g_free (afd->type);
	g_slist_free (afd->uid);
	g_slist_free (afd->gid);
	g_free (afd);
}

static void 
acl_device_added_visitor (const char *seat_id, 
			  int num_sessions_on_seat,
			  const char *session_id, 
			  uid_t session_uid,
			  gboolean session_is_local,
			  gboolean session_is_active, 
			  gpointer user_data)
{
	GSList *i;
	GSList *afd_list = (GSList *) user_data;

#if 0
	if (session_id == NULL) {
		/* means we're just visiting the seat; each session on the seat will be visited accordingly */
		printf ("Visiting seat '%s' with %d sessions\n", 
			seat_id, num_sessions_on_seat);
	} else {
		printf ("  %s: Visiting session '%s' with uid %d  (is_local=%d) (is_active=%d)\n",
			seat_id, session_id, session_uid, session_is_local, session_is_active);
	}
#endif

	/* for each entry ACLForDevice in afd_list, add to the uid and
	 * gid lists for users and groups that should have access to
	 * the device in question
	 */
	for (i = afd_list; i != NULL; i = g_slist_next (i)) {
		ACLForDevice *afd = (ACLForDevice *) i->data;
                PolKitResult pk_result;
                PolKitSeat *pk_seat;
                PolKitSession *pk_session;
                PolKitAction *pk_action;
                char *priv_name;

		if (session_id == NULL) {
			/* we only grant access to sessions - someone suggested that if a device is tied
			 * to a seat.. the owner might want to give access to user 'dilbert' if, and only
			 * if, no sessions is occuring at that seat. Cuz then the user 'dilbert' could
			 * have a cron job running that takes a webcam shot every 5 minutes or so..
			 *
			 * this can be achieved by testing num_sessions_on_seat==0
			 */
			continue;
		}

                pk_seat = polkit_seat_new ();
                polkit_seat_set_ck_objref (pk_seat, seat_id);

                pk_session = polkit_session_new ();
                polkit_session_set_seat (pk_session, pk_seat);
                polkit_seat_unref (pk_seat);
                polkit_session_set_ck_objref (pk_session, session_id);
                polkit_session_set_uid (pk_session, session_uid);
                polkit_session_set_ck_is_active (pk_session, session_is_active);
                polkit_session_set_ck_is_local (pk_session, session_is_local);
                /* TODO: FIXME: polkit_session_set_ck_remote_host (pk_session, );*/ 

                pk_action = polkit_action_new();
                priv_name = g_strdup_printf ("org.freedesktop.hal.device-access.%s", afd->type);
                polkit_action_set_action_id (pk_action, priv_name);
                g_free (priv_name);

                /* Now ask PolicyKit if the given session should have access */
                pk_result = polkit_context_can_session_do_action (pk_context, 
                                                                  pk_action,
                                                                  pk_session);
                if (pk_result == POLKIT_RESULT_YES) {
			afd_grant_to_uid (afd, session_uid);
                }

                polkit_action_unref (pk_action);
                polkit_session_unref (pk_session);
	}

}

static void
acl_compute_changes (GSList *afd_list, gboolean only_update_acllist)
{
	GSList *current_acl_list = NULL;
	GSList *i;
	GSList *j;
	GSList *k;

	/* get the list of ACL's currently applied */
	if (!get_current_acl_list (&current_acl_list)) {
		printf ("Error getting ACL's currently applied\n");
	}

	/* for each entry in ACLForDevice, we need to modify current_acl_list
	 * such that it matches the entry. This is achieved by
	 *
	 *  - setting the 'remove' boolean to TRUE for existing
	 *    entries that should be removed
	 *
	 *  - adding a new entry with the 'add' boolean set to TRUE,
         *    in the front, for entries we need to have added
	 */
	for (i = afd_list; i != NULL; i = g_slist_next (i)) {
		ACLForDevice *afd = (ACLForDevice *) i->data;

		/* OK, so we have the ACL's we want for a device - this is expressed in afd
		 *
		 * go through all the ACL's that we've *already*
		 * added.. 
		 *
		 *   if an ACL we want is already there
		 *   we simply remove it from afd (the ACL's we want).
		 *
		 *   If we've already got an ACL that we don't want
		 *   tag the current list with this for removal.
		 *
		 * At the end the afd will express the ACL's that we
		 * need to add.. so make entries in the current_list
		 * with 'add' set to TRUE.
		 */
		for (j = current_acl_list; j != NULL; j = g_slist_next (j)) {
			ACLCurrent *ha = (ACLCurrent *) j->data;

			if (strcmp (afd->device, ha->device) == 0) {
				switch (ha->type) {
				case HAL_ACL_UID:
					/* see if this is already in the ACLForDevice entry */
					for (k = afd->uid; k != NULL; k = g_slist_next (k)) {
						uid_t uid = GPOINTER_TO_INT (k->data);
						if (uid == ha->v.uid) {
							/* yup, so we're all good - remove it from the afd_list
							 * since we don't need it to be added later...
							 */
							afd->uid = g_slist_delete_link (afd->uid, k);
							break;
						}
					}
					if (k == NULL) {
						/* nope, element wasn't there so this ACLCurrent should be removed */
						ha->remove = TRUE;
					}
					break;
				case HAL_ACL_GID:
					/* see if this is already in the ACLForDevice entry */
					for (k = afd->gid; k != NULL; k = g_slist_next (k)) {
						gid_t gid = GPOINTER_TO_INT (k->data);
						if (gid == ha->v.gid) {
							/* yup, so we're all good - remove it from the afd_list
							 * since we don't need it to be added later...
							 */
							afd->gid = g_slist_delete_link (afd->gid, k);
							break;
						}
					}
					if (k == NULL) {
						/* nope, element wasn't there so this ACLCurrent should be removed */
						ha->remove = TRUE;
					}
					break;
				}
			}

		}

		/* now go through remaining entries in afd->uid and afd->gid 
		 * and create ACLCurrent entries
		 */
		for (j = afd->uid; j != NULL; j = g_slist_next (j)) {
			ACLCurrent *ha;
                        uid_t uid;

                        uid = GPOINTER_TO_INT (j->data);
                        /* never grant ACL's to the super user */
                        if (uid == 0)
                                continue;

			ha = g_new0 (ACLCurrent, 1);
			ha->add = TRUE;
			ha->device = g_strdup (afd->device);
                        ha->udi = g_strdup (afd->udi);
			ha->type = HAL_ACL_UID;
			ha->v.uid = uid;
			current_acl_list = g_slist_prepend (current_acl_list, ha);
		}
		for (j = afd->gid; j != NULL; j = g_slist_next (j)) {
			ACLCurrent *ha;
			ha = g_new0 (ACLCurrent, 1);
			ha->add = TRUE;
			ha->device = g_strdup (afd->device);
                        ha->udi = g_strdup (afd->udi);
			ha->type = HAL_ACL_GID;
			ha->v.gid = GPOINTER_TO_INT (j->data);
			current_acl_list = g_slist_prepend (current_acl_list, ha);
		}
	}

#if 0
	printf ("====================================\n");
	for (i = current_acl_list; i != NULL; i = g_slist_next (i)) {
		ACLCurrent *ha = (ACLCurrent *) i->data;

		printf ("  ACL '%s' %d %d rem=%d add=%d\n", ha->device, ha->type, ha->v.uid, ha->remove, ha->add);
		
	}
	printf ("====================================\n");
#endif

	acl_apply_changes (current_acl_list, only_update_acllist, FALSE);

out:
	if (current_acl_list != NULL) {
		g_slist_foreach (current_acl_list, (GFunc) hal_acl_free, NULL);
		g_slist_free (current_acl_list);
	}
}


static void
acl_device_added (void)
{
	char *s;
	char *udi;
	char *device;
	char *type;
	GSList *afd_list = NULL;
	ACLForDevice *afd = NULL;

        in_device_added = TRUE;

	/* we can avoid round-trips to the HAL daemon by using what's in the environment */

	if ((udi = getenv ("UDI")) == NULL)
		goto out;

	if ((device = getenv ("HAL_PROP_ACCESS_CONTROL_FILE")) == NULL)
		goto out;

	if ((type = getenv ("HAL_PROP_ACCESS_CONTROL_TYPE")) == NULL)
		goto out;

	afd = acl_for_device_new (udi);
	acl_for_device_set_device (afd, device);
	acl_for_device_set_type (afd, type);
	afd_list = g_slist_prepend (NULL, afd);

	/* get ACL granting policy from HAL properties */
	if ((s = getenv ("HAL_PROP_ACCESS_CONTROL_GRANT_USER")) != NULL) {
		char **sv;
		sv = g_strsplit (s, "\t", 0);
		afd_grant_to_uid_from_userlist (afd, sv);
		g_strfreev (sv);
	}
	if ((s = getenv ("HAL_PROP_ACCESS_CONTROL_GRANT_GROUP")) != NULL) {
		char **sv;
		sv = g_strsplit (s, "\t", 0);
		afd_grant_to_gid_from_grouplist (afd, sv);
		g_strfreev (sv);
	}

	/* determine what ACL's we want to put on the given device
	 * files; e.g. apply the seat / session policy
	 *
	 * (entries in afd_list will be modified, see ACLForDevice
	 * data structure)
	 */
	if (!visit_seats_and_sessions (acl_device_added_visitor, (gpointer) afd_list)) {
		printf ("Error visiting seats and sessions\n");
		goto out;
	}
	
	printf ("%d: adding ACL's for %s\n", getpid (), device);

	acl_compute_changes (afd_list, FALSE);

out:
	if (afd != NULL)
		acl_for_device_free (afd);
	if (afd_list != NULL)
		g_slist_free (afd_list);
}

static void
acl_device_removed (void)
{
	char *udi;
	char *device;
	GSList *afd_list = NULL;
	ACLForDevice *afd = NULL;

	/* we can avoid round-trips to the HAL daemon by using what's in the environment */

	if ((udi = getenv ("UDI")) == NULL)
		goto out;

	if ((device = getenv ("HAL_PROP_ACCESS_CONTROL_FILE")) == NULL)
		goto out;

	afd = acl_for_device_new (udi);
	acl_for_device_set_device (afd, device);
	afd_list = g_slist_prepend (NULL, afd);

	/* since this device is to be removed don't set policy - this means "grant it to no-one"; and since
	 * there is no-one to grant it to.. we don't need to visit any seats
	 */
	printf ("%d: removing ACL's for %s\n", getpid (), device);

	/* only update the ACL list, don't invoke setfacl(1) on the
	 * files (see note in acl_apply_changes()) 
	 */
	acl_compute_changes (afd_list, TRUE);

out:
	if (afd != NULL)
		acl_for_device_free (afd);
	if (afd_list != NULL)
		g_slist_free (afd_list);
}

static void
acl_reconfigure_all (void)
{
	int i;
	int num_devices;
	char **udis;
	DBusError error;
	GSList *afd_list = NULL;

	printf ("%d: reconfiguring all ACL's\n", getpid ());

        if (!ensure_hal_ctx ())
                goto out;

	dbus_error_init (&error);
	if ((udis = libhal_find_device_by_capability (hal_ctx, "access_control", &num_devices, &error)) == NULL) {
		printf ("%d: Cannot get list of devices of capability 'acl'\n", getpid ());
		goto out;
	}

	for (i = 0; udis[i] != NULL; i++) {
		LibHalPropertySet *props;
		LibHalPropertySetIterator psi;
		char *device = NULL;
                char *type = NULL;
		ACLForDevice *afd;
		char **sv;

		if ((props = libhal_device_get_all_properties (hal_ctx, udis[i], &error)) == NULL) {
			printf ("%d: Cannot get list properties for '%s'\n", getpid (), udis[1]);
			goto out;
		}

		afd = acl_for_device_new (udis[i]);

		libhal_psi_init (&psi, props);
		while (libhal_psi_has_more (&psi)) {
			char *key;
			key = libhal_psi_get_key (&psi);
			if (strcmp (key, "access_control.file") == 0) {
				device = libhal_psi_get_string (&psi);
			} else if (strcmp (key, "access_control.type") == 0) {
				type = libhal_psi_get_string (&psi);
			} else if (strcmp (key, "access_control.grant_user") == 0) {
				sv = libhal_psi_get_strlist (&psi);
				afd_grant_to_uid_from_userlist (afd, sv);
			} else if (strcmp (key, "access_control.grant_group") == 0) {
				sv = libhal_psi_get_strlist (&psi);
				afd_grant_to_gid_from_grouplist (afd, sv);
			}
			libhal_psi_next (&psi);
		}

		if (device == NULL) {
			printf ("%d: access_control.file not set for '%s'\n", getpid (), udis[i]);
                        acl_for_device_free (afd);
                        goto skip;
		}

		if (type == NULL) {
			printf ("%d: access_control.type not set for '%s'\n", getpid (), udis[i]);
                        acl_for_device_free (afd);
                        goto skip;
		}

                acl_for_device_set_device (afd, device);
                acl_for_device_set_type (afd, type);
                afd_list = g_slist_prepend (afd_list, afd);
        skip:
		libhal_free_property_set (props);
	}
	libhal_free_string_array (udis);

	if (g_slist_length (afd_list) > 0) {
		if (!visit_seats_and_sessions (acl_device_added_visitor, (gpointer) afd_list)) {
			printf ("Error visiting seats and sessions\n");
			goto out;
		}
		acl_compute_changes (afd_list, FALSE);
	}

out:
	;
}

static void
acl_remove_all (void)
{
	GSList *current_acl_list = NULL;
	GSList *i;

	if (!get_current_acl_list (&current_acl_list)) {
		printf ("Error getting ACL's currently applied\n");
		goto out;
	}

	for (i = current_acl_list; i != NULL; i = g_slist_next (i)) {
		ACLCurrent *ha = (ACLCurrent *) i->data;
		ha->remove = TRUE;
	}

	acl_apply_changes (current_acl_list, FALSE, FALSE);

out:
	if (current_acl_list != NULL) {
		g_slist_foreach (current_acl_list, (GFunc) hal_acl_free, NULL);
		g_slist_free (current_acl_list);
	}
}

static int lock_acl_fd = -1;

static gboolean
acl_lock (void)
{
	if (lock_acl_fd >= 0)
		return TRUE;

	printf ("%d: attempting to get lock on " PACKAGE_LOCALSTATEDIR "/run/hald/acl-list\n", getpid ());

	lock_acl_fd = open (PACKAGE_LOCALSTATEDIR "/run/hald/acl-list", O_CREAT | O_RDWR, 0644);
	if (lock_acl_fd < 0) {
		printf ("%d: error opening/creating " PACKAGE_LOCALSTATEDIR "/run/hald/acl-list\n", getpid ());
		return FALSE;
	}
	

tryagain:
#if sun
	if (lockf (lock_acl_fd, F_LOCK, 0) != 0)
#else
	if (flock (lock_acl_fd, LOCK_EX) != 0)
#endif
	{
		if (errno == EINTR)
			goto tryagain;
		return FALSE;
	}

        printf ("\n");
        printf ("****************************************************\n");
	printf ("%d: got lock on " PACKAGE_LOCALSTATEDIR "/run/hald/acl-list\n", getpid ());
	return TRUE;
}

static void
acl_unlock (void)
{
        printf ("\n");
        printf ("****************************************************\n");
	printf ("%d: releasing lock on " PACKAGE_LOCALSTATEDIR "/run/hald/acl-list\n", getpid ());
#if sun
	lockf (lock_acl_fd, F_ULOCK, 0);
#else
	flock (lock_acl_fd, LOCK_UN);
#endif
	close (lock_acl_fd);
	lock_acl_fd = -1;
}

int
main (int argc, char *argv[])
{
        PolKitError *p_error;

	if (argc != 2) {
		printf ("hal-acl-tool should only be invoked by hald\n");
		goto out;
	}
	
	if (!acl_lock ()) {
		goto out;
	}

        p_error = NULL;
        pk_context = polkit_context_new ();
        if (!polkit_context_init (pk_context, &p_error)) {
                printf ("Could not init PolicyKit context: %s\n", polkit_error_get_error_message (p_error));
                goto out;
        }

	if (strcmp (argv[1], "--add-device") == 0) {
		acl_device_added ();
	} else if (strcmp (argv[1], "--remove-device") == 0) {
		acl_device_removed ();
	} else if (strcmp (argv[1], "--reconfigure") == 0) {
		acl_reconfigure_all ();
	} else if (strcmp (argv[1], "--remove-all") == 0) {
		acl_remove_all ();
	}

	acl_unlock ();

out:
	return 0;
}
