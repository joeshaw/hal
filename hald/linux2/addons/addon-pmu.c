/***************************************************************************
 * CVSID: $Id$
 *
 * addon-pmu-lid.c : Poll the lid button for PMU on Apple computers
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <linux/adb.h>
#include <linux/pmu.h>

#include "libhal/libhal.h"

#include "../probing/shared.h"

int
main (int argc, char *argv[])
{
	int fd;
	char *udi;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	int rd;
	char buf[256];
	int state;
	int new_state;
	char *strstate;

	fd = -1;

	if ((getenv ("HALD_VERBOSE")) != NULL)
		is_verbose = TRUE;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);
	if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
		goto out;
	
	if ((ctx = libhal_ctx_new ()) == NULL)
		goto out;
	if (!libhal_ctx_set_dbus_connection (ctx, conn))
		goto out;
	if (!libhal_ctx_init (ctx, &error))
		goto out;

	/* initial state */
	if ((strstate = getenv ("HAL_PROP_BUTTON_STATE_VALUE")) == NULL) {
		dbg ("Cannot get HAL_PROP_BUTTON_STATE_VALUE");
		goto out;
	}
	if (strcmp (strstate, "true") == 0)
		state = TRUE;
	else
		state = FALSE;

	if ((fd = open ("/dev/adb", O_RDWR | O_NONBLOCK)) < 0) {
                dbg ("Cannot open /dev/adb");
                goto out;
	}

	while (1) {
		int n;

		buf[0] = PMU_PACKET;
		buf[1] = PMU_GET_COVER;

		n = write (fd, buf, 2);
		if (n == 2) {
			int i;

			rd = read (fd, buf, sizeof (buf));
			if (rd <= 0) {
				dbg ("Error reading from fd; read returned %d; err=%s", strerror (errno));
				goto out;
			}

#if 0
			dbg ("Read 0x%02x bytes", rd);				
			for (i = 0; i < rd; i++) {
				dbg ("%02x : 0x%02x", i, buf[i]);
			}
#endif

			if (rd >= 2) {
				new_state = (((buf[1]) & 0x01) != 0);

				if (new_state != state) {
					dbg ("lid state change: %d", new_state);
					dbus_error_init (&error);
					libhal_device_set_property_bool (
						ctx, udi, "button.state.value", new_state, &error);
					dbus_error_init (&error);
					libhal_device_emit_condition (ctx, udi, "ButtonPressed", "", &error);
				}

				state = new_state;
			}

			
		}

		sleep (1);
	}




out:
	if (fd >= 0)
		close (fd);

	return 0;
}
