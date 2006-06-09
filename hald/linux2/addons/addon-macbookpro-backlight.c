/***************************************************************************
 * CVSID: $Id$
 *
 * hal_addon_macbookpro-backlight.c : Set backlight for Macbook Pro
 * laptops that uses the ATI X1600 chipset. Based on code from Nicolas
 * Boichat found on the mactel-linux mailing list.
 *
 * Copyright (C) 2006 David Zeuthen <david@fubar.dk>
 * Copyright (C) 2006 Nicolas Boichat <nicolas@bo...>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#include <config.h>
#include <stdio.h>

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <sys/io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h> 
#include <pci/pci.h>

#include "libhal/libhal.h"
#include "../probing/shared.h"

static LibHalContext *halctx = NULL;
static GMainLoop *main_loop;
static char *udi;
static DBusConnection *conn;

static char* memory;
 
static inline unsigned int readl(const volatile void *addr)
{
 	return *(volatile unsigned int*) addr;
}

static inline void writel(unsigned int b, volatile void *addr)
{
 	*(volatile unsigned int*) addr = b;
}

#define INREG(addr)		readl(memory+addr)
#define OUTREG(addr,val)	writel(val, memory+addr)

unsigned char read_backlight() {
 	return INREG(0x7af8) >> 8;
}

void write_backlight(unsigned char value) {
 	OUTREG(0x7af8, 0x00000001 | ((unsigned int)value << 8));
}

static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply;

	/*dbg ("filter_function: sender=%s destination=%s obj_path=%s interface=%s method=%s", 
	     dbus_message_get_sender (message), 
	     dbus_message_get_destination (message), 
	     dbus_message_get_path (message), 
	     dbus_message_get_interface (message),
	     dbus_message_get_member (message));*/

	reply = NULL;

	if (dbus_message_is_method_call (message, 
					 "org.freedesktop.Hal.Device.LaptopPanel", 
					 "SetBrightness")) {
		int brightness;

		dbus_error_init (&err);
		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			dbg ("setting brightness %d", brightness);
			if (brightness < 0 || brightness > 228) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.LaptopPanel.Invalid",
								"Brightness has to be between 0 and 228!");

			} else {
				int return_code;

				write_backlight (brightness + 27);

				reply = dbus_message_new_method_return (message);
				if (reply == NULL)
					goto error;

				return_code = 0;
				dbus_message_append_args (reply,
							  DBUS_TYPE_INT32, &return_code,
							  DBUS_TYPE_INVALID);
			}

			dbus_connection_send (connection, reply, NULL);
		}
		
	} else if (dbus_message_is_method_call (message, 
						"org.freedesktop.Hal.Device.LaptopPanel", 
						"GetBrightness")) {
		int brightness;

		dbus_error_init (&err);
		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INVALID)) {

			brightness = read_backlight () - 27;
			if (brightness < 0)
				brightness = 0;
			if (brightness > 228)
				brightness = 228;

			dbg ("getting brightness, it's %d", brightness);

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
		
	}
	
error:
	if (reply != NULL)
		dbus_message_unref (reply);

	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
 	char* endptr;
 	int ret = 0;
 	off_t address = 0;
 	size_t length = 0;
 	int fd;
 	int state;
	DBusError err;

	_set_debug ();	
	udi = getenv ("UDI");

	dbg ("udi=%s", udi);
	if (udi == NULL) {
		fprintf (stderr, "No device specified");
		return -2;
	}

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		fprintf (stderr, "Cannot connect to hald");
		return -3;
	}

	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

 	/* Search for the graphics card. */
 	/* Default values: */
 	/* address = 0x90300000; */
 	/* length = 0x20000; */

 	struct pci_access *pacc = pci_alloc();
 	pci_init(pacc);
 	pci_scan_bus(pacc);
 	struct pci_dev *dev;
 	for(dev=pacc->devices; dev; dev=dev->next) {	/* Iterate over all devices */
 		pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);
 		if ((dev->vendor_id == 0x1002) && (dev->device_id == 0x71c5)) { // ATI X1600
 			address = dev->base_addr[2];
 			length = dev->size[2];
 		}
 	}
 	pci_cleanup(pacc);

	dbg ("addr 0x%x len=%d", address, length);
 
 	if (!address) {
 		dbg ("Failed to detect ATI X1600, aborting...");
 		return 1;
 	}
 
 	fd = open ("/dev/mem", O_RDWR);
 	
 	if (fd < 0) {
 		dbg ("cannot open /dev/mem");
 		return 1;
 	}
 
 	memory = mmap (NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, address);
 
 	if (memory == MAP_FAILED) {
 		perror ("mmap failed");
 		return 2;
 	}
 
 	/* Is it really necessary ? */
 	OUTREG(0x4dc, 0x00000005);
 	state = INREG(0x7ae4);
 	OUTREG(0x7ae4, state);


	if (!libhal_device_claim_interface (halctx, 
					    udi, 
					    "org.freedesktop.Hal.Device.LaptopPanel", 
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		fprintf (stderr, "Cannot claim interface");
		return -4;
	}

	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	return 0;
}
