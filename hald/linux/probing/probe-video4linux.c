/***************************************************************************
 * CVSID: $Id$
 *
 * probe-video4linux.c : Probe video4linux devices
 *
 * Copyright (C) 2007 Nokia Corporation
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

#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/videodev.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libhal/libhal.h"
#include "../../logger.h"

int
main (int argc, char *argv[])
{
	int fd = -1;
	int ret = -1;
	char *udi;
	char *device_file;
	struct video_capability v1cap;
	struct v4l2_capability v2cap;
	LibHalContext *ctx = NULL;
	LibHalChangeSet *cset;
	DBusError error;

	setup_logger ();

	device_file = getenv ("HAL_PROP_VIDEO4LINUX_DEVICE");
	if (device_file == NULL)
		goto out;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	dbus_error_init (&error);
	ctx = libhal_ctx_init_direct (&error);
	if (ctx == NULL)
		goto out;

	cset = libhal_device_new_changeset (udi);

	HAL_DEBUG (("Doing probe-video4linux for %s (udi=%s)", device_file, udi));

	fd = open (device_file, O_RDONLY);
	if (fd < 0) {
		HAL_ERROR (("Cannot open %s: %s", device_file, strerror (errno)));
		goto out;
	}

	if (ioctl (fd, VIDIOC_QUERYCAP, &v2cap) == 0) {
		libhal_changeset_set_property_string (cset,
		                                      "video4linux.version", "2");

		libhal_changeset_set_property_string (cset,
		                                      "info.product", (const char *)v2cap.card);

		if ((v2cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) > 0) {
			libhal_device_add_capability (ctx, udi, "video4linux.video_capture", &error);
		} if ((v2cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) > 0) {
			LIBHAL_FREE_DBUS_ERROR (&error);
			libhal_device_add_capability (ctx, udi, "video4linux.video_output", &error);
		} if ((v2cap.capabilities & V4L2_CAP_VIDEO_OVERLAY) > 0) {
			LIBHAL_FREE_DBUS_ERROR (&error);
			libhal_device_add_capability (ctx, udi, "video4linux.video_overlay", &error);
		} if ((v2cap.capabilities & V4L2_CAP_AUDIO) > 0) {
			LIBHAL_FREE_DBUS_ERROR (&error);
			libhal_device_add_capability (ctx, udi, "video4linux.audio", &error);
		} if ((v2cap.capabilities & V4L2_CAP_TUNER) > 0) {
			LIBHAL_FREE_DBUS_ERROR (&error);
			libhal_device_add_capability (ctx, udi, "video4linux.tuner", &error);
		} if ((v2cap.capabilities & V4L2_CAP_RADIO) > 0) {
			LIBHAL_FREE_DBUS_ERROR (&error);
			libhal_device_add_capability (ctx, udi, "video4linux.radio", &error);
		}
	} else {
		HAL_DEBUG (("ioctl VIDIOC_QUERYCAP failed"));

		if (ioctl (fd, VIDIOCGCAP, &v1cap) == 0) {
			libhal_changeset_set_property_string (cset,
			                                      "video4linux.version", "1");

			libhal_changeset_set_property_string (cset,
			                                      "info.product", v1cap.name);

			if ((v1cap.type & VID_TYPE_CAPTURE) > 0) {
				LIBHAL_FREE_DBUS_ERROR (&error);
				libhal_device_add_capability (ctx, udi, "video4linux.video_capture", &error);
			} if ((v1cap.type & VID_TYPE_OVERLAY) > 0) {
				LIBHAL_FREE_DBUS_ERROR (&error);
				libhal_device_add_capability (ctx, udi, "video4linux.video_overlay", &error);
			} if (v1cap.audios > 0) {
				LIBHAL_FREE_DBUS_ERROR (&error);
				libhal_device_add_capability (ctx, udi, "video4linux.audio", &error);
			} if ((v1cap.type & VID_TYPE_TUNER) > 0) {
				LIBHAL_FREE_DBUS_ERROR (&error);
				libhal_device_add_capability (ctx, udi, "video4linux.tuner", &error);
			}
		} else {
			HAL_DEBUG (("ioctl VIDIOCGCAP failed"));
		}
	}

	LIBHAL_FREE_DBUS_ERROR (&error);
	libhal_device_commit_changeset (ctx, cset, &error);
	libhal_device_free_changeset (cset);

	close (fd);

	ret = 0;

out:
	if (fd >= 0)
		close (fd);

	LIBHAL_FREE_DBUS_ERROR (&error);
	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return ret;
}

