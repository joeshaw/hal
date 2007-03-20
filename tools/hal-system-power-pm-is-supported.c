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
#include <unistd.h>

#include <libhal.h>
#include <glib.h>

int
main (int argc, char *argv[])
{
        int ret;
	char *udi;
	DBusError error;
	LibHalContext *hal_ctx = NULL;
        LibHalChangeSet *cs;
        gboolean can_suspend = FALSE;
        gboolean can_suspend_hybrid = FALSE;
        gboolean can_hibernate = FALSE;
        int exit_status;

        ret = 1;

	udi = getenv ("HAL_PROP_INFO_UDI");
	if (udi == NULL)
                goto out;

	dbus_error_init (&error);
	if ((hal_ctx = libhal_ctx_init_direct (&error)) == NULL) {
		printf ("Cannot connect to hald\n");
		if (dbus_error_is_set (&error)) {
			dbus_error_free (&error);
		}
                goto out;
	}

	cs = libhal_device_new_changeset (udi);
	if (cs == NULL) {
		printf ("Cannot initialize changeset\n");
		goto out;
	}

        g_spawn_command_line_sync ("/usr/bin/pm-is-supported --suspend", NULL, NULL, &exit_status, NULL);
        can_suspend = (exit_status == 0);
        g_spawn_command_line_sync ("/usr/bin/pm-is-supported --suspend-hybrid", NULL, NULL, &exit_status, NULL);
        can_suspend_hybrid = (exit_status == 0);
        g_spawn_command_line_sync ("/usr/bin/pm-is-supported --hibernate", NULL, NULL, &exit_status, NULL);
        can_hibernate = (exit_status == 0);

        libhal_changeset_set_property_bool (cs, "power_management.can_suspend", can_suspend);
        libhal_changeset_set_property_bool (cs, "power_management.can_suspend_hybrid", can_suspend_hybrid);
        libhal_changeset_set_property_bool (cs, "power_management.can_hibernate", can_hibernate);

        libhal_device_commit_changeset (hal_ctx, cs, &error);
        libhal_device_free_changeset (cs);

        ret = 0;
out:
	return ret;
}
