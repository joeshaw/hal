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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/adb.h>
#include <linux/pmu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhal/libhal.h"

#include "../../logger.h"
#include "../../util_helper.h"

int
main (int argc, char *argv[])
{
	int fd;
	char *udi;
	LibHalContext *ctx = NULL;
	DBusError error;
	int rd;
	char buf[256];
	int state;
	int new_state;
	char *strstate;

	fd = -1;

	setup_logger ();

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);

	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	if (!libhal_device_addon_is_ready (ctx, udi, &error)) {
		goto out;
	}


	/* initial state */
	if ((strstate = getenv ("HAL_PROP_BUTTON_STATE_VALUE")) == NULL) {
		HAL_ERROR (("Cannot get HAL_PROP_BUTTON_STATE_VALUE"));
		goto out;
	}
	if (strcmp (strstate, "true") == 0)
		state = TRUE;
	else
		state = FALSE;

	if ((fd = open ("/dev/adb", O_RDWR)) < 0) {
                HAL_ERROR (("Cannot open /dev/adb"));
                goto out;
	}

        drop_privileges (0);

	while (1) {
		int n;

		buf[0] = PMU_PACKET;
		buf[1] = PMU_GET_COVER;

		n = write (fd, buf, 2);
		if (n == 2) {
			rd = read (fd, buf, sizeof (buf));
			if (rd <= 0) {
				HAL_ERROR (("Error reading from fd; read returned %d; err=%s", errno, strerror (errno)));
				goto out;
			}

#if 0
			int i;
			
			HAL_DEBUG (("Read 0x%02x bytes", rd));				
			for (i = 0; i < rd; i++) {
				dbg ("%02x : 0x%02x", i, buf[i]);
			}
#endif

			if (rd >= 2) {
				new_state = (((buf[1]) & 0x01) != 0);

				if (new_state != state) {
					HAL_DEBUG (("lid state change: %d", new_state));
					libhal_device_set_property_bool (ctx, udi, "button.state.value", new_state, &error);
					LIBHAL_FREE_DBUS_ERROR (&error);
					libhal_device_emit_condition (ctx, udi, "ButtonPressed", "", &error);
				}

				state = new_state;
			}

			
		}
		LIBHAL_FREE_DBUS_ERROR (&error);
		usleep (1000 * 1000);
	}

out:
	if (fd >= 0)
		close (fd);

        LIBHAL_FREE_DBUS_ERROR (&error);

        if (ctx != NULL) {
                libhal_ctx_shutdown (ctx, &error);
                LIBHAL_FREE_DBUS_ERROR (&error);
                libhal_ctx_free (ctx);
        }

	return 0;
}
