/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-
 ***************************************************************************
 * CVSID: $Id$
 *
 * probe-ieee1394-unit.c : Probe for Firewire unit types
 *
 * Copyright (C) 2007 David Zeuthen, <david@redhat.com>
 * Copyright (C) 2007 Kristian Hogsberg, <krh@redhat.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <asm/types.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include "../../logger.h"
#include "libhal/libhal.h"

/* Defines and structs copied from fw-device-cdev.h */

#define TCODE_WRITE_BLOCK_REQUEST	1
#define RCODE_COMPLETE			0x0
#define RCODE_TYPE_ERROR		0x6

#define FW_CDEV_EVENT_BUS_RESET		0x00
#define FW_CDEV_EVENT_RESPONSE		0x01
#define FW_CDEV_EVENT_REQUEST		0x02
#define FW_CDEV_EVENT_ISO_INTERRUPT	0x03

struct fw_cdev_event_bus_reset {
	__u64 closure;
	__u32 type;
	__u32 node_id;
	__u32 local_node_id;
	__u32 bm_node_id;
	__u32 irm_node_id;
	__u32 root_node_id;
	__u32 generation;
};

struct fw_cdev_event_response {
	__u64 closure;
	__u32 type;
	__u32 rcode;
	__u32 length;
	__u32 data[0];
};

struct fw_cdev_event_request {
	__u64 closure;
	__u32 type;
	__u32 tcode;
	__u64 offset;
	__u32 handle;
	__u32 length;
	__u32 data[0];
};

#define FW_CDEV_IOC_GET_INFO		_IOWR('#', 0x00, struct fw_cdev_get_info)
#define FW_CDEV_IOC_SEND_REQUEST	_IOW('#', 0x01, struct fw_cdev_send_request)
#define FW_CDEV_IOC_ALLOCATE		_IOWR('#', 0x02, struct fw_cdev_allocate)
#define FW_CDEV_IOC_DEALLOCATE		_IOW('#', 0x03, struct fw_cdev_deallocate)
#define FW_CDEV_IOC_SEND_RESPONSE	_IOW('#', 0x04, struct fw_cdev_send_response)

/* FW_CDEV_VERSION History
 *
 * 1	Feb 18, 2007:  Initial version.
 */
#define FW_CDEV_VERSION		1

struct fw_cdev_get_info {
	__u32 version;
	__u32 rom_length;
	__u64 rom;
	__u64 bus_reset;
	__u64 bus_reset_closure;
	__u32 card;
};

struct fw_cdev_send_request {
	__u32 tcode;
	__u32 length;
	__u64 offset;
	__u64 closure;
	__u64 data;
	__u32 generation;
};

struct fw_cdev_send_response {
	__u32 rcode;
	__u32 length;
	__u64 data;
	__u32 handle;
};

struct fw_cdev_allocate {
	__u64 offset;
	__u64 closure;
	__u32 length;
	__u32 handle;
};

struct fw_cdev_deallocate {
	__u32 handle;
};

/* end of code from fw-device-cdev.h */


#define ARRAY_LENGTH(a) (sizeof(a)/sizeof((a)[0]))

#define ptr_to_u64(p) ((__u64)(unsigned long)(p))
#define u64_to_ptr(p) ((void *)(unsigned long)(p))

#define CSR_FCP_COMMAND		0xfffff0000b00ull
#define CSR_FCP_RESPONSE	0xfffff0000d00ull

struct avc_frame {
	unsigned int ctype : 4;
	unsigned int cts : 4;
	unsigned int subunit_id : 3;
	unsigned int subunit_type : 5;
	unsigned int opcode : 8;
	unsigned int operand0 : 8;
};

enum {
	CTS_AVC
	/* Nevermind the rest. */
};

enum {
	AVC_COMMAND_CONTROL,
	AVC_COMMAND_STATUS,
	AVC_COMMAND_SPECIFIC_INQUIRY,
	AVC_COMMAND_NOTIFY,
	AVC_COMMAND_GENERAL_INQUIRY,
	AVC_RESPONSE_NOT_IMPLEMENTED = 0x08,
	AVC_RESPONSE_ACCEPTED,
	AVC_RESPONSE_REJECTED,
	AVC_RESPONSE_IN_TRANSITION,
	AVC_RESPONSE_STABLE,
	AVC_RESPONSE_CHANGED,
	AVC_RESPONSE_INTERIM = 0x0f
};

enum {
	AVC_SUBUNIT_MONITOR,
	AVC_SUBUNIT_AUDIO,
	AVC_SUBUNIT_PRINTER,
	AVC_SUBUNIT_DISC,
	AVC_SUBUNIT_TAPE_RECORDER_PLAYER,
	AVC_SUBUNIT_TUNER,
	AVC_SUBUNIT_CA,
	AVC_SUBUNIT_CAMERA,
	AVC_SUBUNIT_PANEL = 0x09,
	AVC_SUBUNIT_BULLETIN_BOARD,
	AVC_SUBUNIT_CAMERA_STORAGE,
	AVC_SUBUNIT_VENDOR_UNIQUE = 0x1c,
	AVC_SUBUNIT_ALL,
	AVC_SUBUNIT_EXTENDED,
	AVC_SUBUNIT_UNIT
};

static const char * const unit_names[] = {
	"monitor",
	"audio",
	"printer",
	"disc",
	"tape_recorder_player",
	"tuner",
	"ca",
	"camera",
	NULL, /* unused */
	"panel",
	"bulletin_board",
	"camera_storage",
};

enum {
	AVC_OPCODE_UNIT_INFO = 0x30,
	AVC_OPCODE_SUBUNIT_INFO = 0x31,
};

enum {
	AVC_SUBUNIT_ID_UNIT = 0x07,
};

static const char *udi = NULL;
static LibHalContext *ctx = NULL;


static void
send_response (int fd, __u32 handle, int rcode, void *data, size_t length)
{
	struct fw_cdev_send_response response;
	
	response.length = length;
	response.handle = handle;
	response.rcode  = rcode;
	response.data   = ptr_to_u64(data);
	
	if (ioctl (fd, FW_CDEV_IOC_SEND_RESPONSE, &response) < 0) {
		HAL_ERROR (("failed to send response: %s", strerror (errno)));
		return;
	}
}

static int
handle_request (int fd, struct fw_cdev_event_request *request)
{
	struct  {
		struct avc_frame frame;
		unsigned char operands[8];
	} *response;
	unsigned int unit_type;
	char capname[256];
	int i;

	if (request->tcode != TCODE_WRITE_BLOCK_REQUEST ||
	    request->offset != CSR_FCP_RESPONSE) {
		send_response (fd, request->handle, RCODE_TYPE_ERROR, NULL, 0);
		HAL_ERROR (("AVC response to wrong address"));
		return -1;
	}
	
	send_response (fd, request->handle, RCODE_COMPLETE, NULL, 0);
	
	response = (void *) request->data;
	if (response->frame.cts != CTS_AVC) {
		HAL_ERROR (("not an fcp response"));
		return -1;
	}

	if (response->frame.ctype == AVC_RESPONSE_INTERIM) {
		HAL_ERROR (("got interim"));
		/* Returning -1 here will make get_subunit_info() go back into
		 * poll() and wait for up to 200ms. */
		return -1;
	}
	
	for (i = 0; i < 4; i++) {
		if (response->operands[i] == 0xff)
			break;

		unit_type = response->operands[i] >> 3;
		if (unit_type > ARRAY_LENGTH(unit_names) ||
		    unit_names[unit_type] == NULL) {
			HAL_ERROR (("unknown unit type"));
			return -1;
		}

		snprintf (capname, sizeof (capname),
			  "ieee1394_unit.avc.%s", unit_names[unit_type]);
		libhal_device_add_capability (ctx, udi, capname, NULL);
	}

	return 0;
}

/* We wait 200ms for the AV/C response and the IEEE1394 response. */
#define AVC_TIMEOUT 200

static int
get_subunit_info (int fd, int generation)
{
	struct {
		struct avc_frame frame;
		unsigned char operands[8];
	} payload;
	struct fw_cdev_send_request request;
	union {
		struct fw_cdev_event_response response;
		struct fw_cdev_event_request request;
		__u8 buffer[64];
	} u;
	struct pollfd fds[1];

	request.tcode = TCODE_WRITE_BLOCK_REQUEST;
	request.offset = CSR_FCP_COMMAND;
	request.length = 8;
	request.generation = generation;
	request.data = ptr_to_u64(&payload);
	
	payload.frame.cts = CTS_AVC;
	payload.frame.ctype = AVC_COMMAND_STATUS;
	payload.frame.subunit_type = AVC_SUBUNIT_UNIT;
	payload.frame.subunit_id = AVC_SUBUNIT_ID_UNIT;
	payload.frame.opcode = AVC_OPCODE_SUBUNIT_INFO;
	payload.frame.operand0 = 0xff;
	payload.operands[0] = 0xff;
	payload.operands[1] = 0xff;
	payload.operands[2] = 0xff;
	payload.operands[3] = 0xff;
	
	if (ioctl (fd, FW_CDEV_IOC_SEND_REQUEST, &request) < 0) {
		HAL_ERROR (("failed to write request: %s", strerror (errno)));
		return -1;
	}
	
	fds[0].fd = fd;
	fds[0].events = POLLIN;
	
	while (TRUE) {
		if (poll (fds, 1, AVC_TIMEOUT) < 0) {
			HAL_ERROR (("poll error: %s", strerror (errno)));
			return -1;
		}
		
		if (fds[0].revents == 0) {
			HAL_ERROR (("timeout"));
			return -1;
		}
		
		if (read (fd, &u, sizeof u) < 0) {
			HAL_ERROR (("read failed: %s", strerror (errno)));
			return -1;
		}
		
		switch (u.response.type) {
		case FW_CDEV_EVENT_RESPONSE:
			if (u.response.rcode != RCODE_COMPLETE) {
				/* The device didn't appreciate us sending an AV/C
				 * command, maybe it doesn't speak AV/C afterall...*/
				HAL_ERROR (("not AVC device?"));
				return -1;
			}
			break;
		case FW_CDEV_EVENT_REQUEST:
			if (handle_request(fd, &u.request) == 0)
				return 0;
			
			/* Maybe we got AVC_RESPONSE_INTERIM, so wait a little longer. */
			break;
			
		case FW_CDEV_EVENT_BUS_RESET:
			HAL_ERROR (("bus reset"));
			return -1;
			
		default:
			HAL_ERROR (("unexpected event, shouldn't happen"));
			return -1;
		}
	}
}

int main (int argc, char *argv[])
{
	int i;
	int fd;
	int ret;
	struct fw_cdev_get_info get_info;
	struct fw_cdev_event_bus_reset bus_reset;
	struct fw_cdev_allocate request;
	const char *device_file;
	const char *ieee1394_udi;
	DBusError error;

	/* assume failure */
	ret = 1;

	setup_logger ();

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	ieee1394_udi = getenv ("HAL_PROP_IEEE1394_UNIT_ORIGINATING_DEVICE");
	if (ieee1394_udi == NULL)
		goto out;

	dbus_error_init (&error);
	if ((ctx = libhal_ctx_init_direct (&error)) == NULL)
		goto out;

	device_file = libhal_device_get_property_string (ctx, ieee1394_udi, "ieee1394.device", &error);
	if (device_file == NULL)
		goto out;

	HAL_INFO (("Investigating '%s'", device_file));

	fd = open (device_file, O_RDWR);
	if (fd < 0) {
		HAL_ERROR (("failed to open %s: %s", device_file, strerror (errno)));
		goto out;
	}

	get_info.version = FW_CDEV_VERSION;
	get_info.rom = 0;
	get_info.rom_length = 0;
	get_info.bus_reset = ptr_to_u64(&bus_reset);
	if (ioctl(fd, FW_CDEV_IOC_GET_INFO, &get_info) < 0) {
		HAL_ERROR (("get config rom ioctl: %s", strerror(errno)));
		goto out;
	}

	request.offset = CSR_FCP_RESPONSE;
	request.length = 0x200;
	request.closure = 0;
	if (ioctl (fd, FW_CDEV_IOC_ALLOCATE, &request) < 0) {
		HAL_ERROR (("failed to allocate fcp response area: %s",
			    strerror (errno)));
		goto out;
	}

	/* Retry the command three times. */
	for (i = 0; i < 3; i++) {
		if (get_subunit_info(fd, bus_reset.generation) == 0)
			break;
		poll (NULL, 0, 500); /* take a 500ms nap */
	}
	
	close(fd);

	ret = 0;

out:
	LIBHAL_FREE_DBUS_ERROR (&error);

        if (ctx != NULL) {
                libhal_ctx_shutdown (ctx, &error);
                LIBHAL_FREE_DBUS_ERROR (&error);
                libhal_ctx_free (ctx);
        }

	return ret;
}
