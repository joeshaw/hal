/***************************************************************************
 * CVSID: $Id$
 *
 * Multimedia device class
 *
 * Copyright (C) 2004 Kay Sievers, <kay.sievers@vrfy.org>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/videodev.h>

#include "../logger.h"
#include "../device_store.h"
#include "../hald.h"

#include "class_device.h"
#include "common.h"

/**
 * @defgroup HalDaemonLinuxVideo Video class
 * @ingroup HalDaemonLinux
 * @brief Video class
 * @{
 */

static dbus_bool_t
multimedia_class_device_accept (ClassDeviceHandler *self,
			     const char *path,
			     struct sysfs_class_device *class_device)
{
	int video_number;

	if (class_device->sysdevice == NULL)
		return FALSE;

	if (sscanf (class_device->name, "video%d", &video_number) == 1)
		return TRUE;

	return FALSE;
}

/** This method is called just before the device is either merged
 *  onto the sysdevice or added to the GDL (cf. merge_or_add). 
 *  This is useful for extracting more information about the device
 *  through e.g. ioctl's using the device file property and also
 *  for setting info.category|capability.
 *
 *  @param  self          Pointer to class members
 *  @param  d             The HalDevice object of the instance of
 *                        this device class
 *  @param  sysfs_path    The path in sysfs (including mount point) of
 *                        the class device in sysfs
 *  @param  class_device  Libsysfs object representing class device
 *                        instance
 */

/* Red Hat doesn't have v4l2 headers, just copy that in
   until we find a better solution */
#define VIDIOC_QUERYCAP			_IOR  ('V',  0, struct v4l2_capability)
#define V4L2_CAP_VIDEO_CAPTURE		0x00000001
#define V4L2_CAP_VIDEO_OUTPUT		0x00000002
#define V4L2_CAP_TUNER			0x00010000
#define V4L2_CAP_AUDIO			0x00020000

struct v4l2_capability {
	__u8	driver[16];
	__u8	card[32];
	__u8	bus_info[32];
	__u32	version;
	__u32	capabilities;
		__u32	reserved[4];
} __attribute__((__packed__)) v2cap;

static void
multimedia_class_pre_process (ClassDeviceHandler *self,
			   HalDevice *d,
			   const char *sysfs_path,
			   struct sysfs_class_device *class_device)
{
	struct video_capability v1cap;
	struct v4l2_capability v2cap;
	int fd;

	hal_device_property_set_string (d, "info.category", "multimedia");

	/* set defaults */
	hal_device_property_set_bool (d, "multimedia.video.can_capture", FALSE);
	hal_device_property_set_bool (d, "multimedia.audio.has_audio", FALSE);
	hal_device_property_set_bool (d, "multimedia.tuner.has_tuner", FALSE);
	hal_device_property_set_string (d, "multimedia.linux.version", "");

	fd = open (hal_device_property_get_string (d, "multimedia.device"),
		   O_RDWR | O_EXCL);

	if (fd < 0)
		return;

	/* v4l Version 2 */
	if (ioctl(fd, VIDIOC_QUERYCAP, &v2cap) == 0) {
		dbus_bool_t flag;

		flag = ((v2cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) > 0);
		hal_device_property_set_bool (d, "multimedia.video.can_capture", flag);

		if ((v2cap.capabilities & V4L2_CAP_AUDIO) > 0);
		hal_device_property_set_bool (d, "multimedia.audio.has_audio", flag);

		flag = ((v2cap.capabilities & V4L2_CAP_TUNER) > 0);
		hal_device_property_set_bool (d, "multimedia.tuner.has_tuner", flag);

		hal_device_property_set_string (d, "multimedia.linux.version", "v4l2");

		goto out;
	}

	/* v4l Version 1 */
	if (ioctl(fd, VIDIOCGCAP, &v1cap) == 0) {
		dbus_bool_t flag;

		flag = ((v1cap.type & VID_TYPE_CAPTURE) > 0);
		hal_device_property_set_bool (d, "multimedia.video.can_capture", flag);

		flag = v1cap.audios > 0;
		hal_device_property_set_bool (d, "multimedia.audio.has_audio", flag);

		flag = ((v1cap.type & VID_TYPE_TUNER) > 0);
		hal_device_property_set_bool (d, "multimedia.tuner.has_tuner", flag);

		hal_device_property_set_string (d, "multimedia.linux.version", "v4l");
		goto out;
	}

out:
	close(fd);
}

/** methods for device class */
ClassDeviceHandler multimedia_class_handler = {
	.init			= class_device_init,
	.shutdown		= class_device_shutdown,
	.tick			= class_device_tick,
	.accept			= multimedia_class_device_accept,
	.visit			= class_device_visit,
	.removed		= class_device_removed,
	.udev_event		= class_device_udev_event,
	.get_device_file_target = class_device_get_device_file_target,
	.pre_process		= multimedia_class_pre_process ,
	.post_merge		= class_device_post_merge,
	.got_udi		= class_device_got_udi,
	.compute_udi		= NULL,
	.in_gdl			= class_device_in_gdl,
	.sysfs_class_name	= "video4linux",
	.hal_class_name		= "multimedia",
	.require_device_file	= TRUE,
	.merge_or_add		= TRUE
};

/** @} */
