/***************************************************************************
 * CVSID: $Id$
 *
 * hal_addon_macbookpro-backlight.c : Set backlight for Macbook Pro
 * laptops that uses the ATI X1600 chipset. Based on code from Nicolas
 * Boichat found on the mactel-linux mailing list.
 *
 * Copyright (C) 2006 David Zeuthen <david@fubar.dk>
 * Copyright (C) 2006 Nicolas Boichat <nicolas@boichat.ch>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pci/pci.h>
#include <unistd.h> 

#include <glib/gmain.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "libhal/libhal.h"
#include "../../logger.h"
#include "../../util_helper_priv.h"

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

static unsigned char 
read_backlight (void)
{
 	return INREG(0x7af8) >> 8;
}

static void 
write_backlight (unsigned char value)
{
 	OUTREG(0x7af8, 0x00000001 | ((unsigned int)value << 8));
}


#define LIGHT_SENSOR_LEFT_KEY	"ALV0" //0x414c5630, r-o length 6
#define LIGHT_SENSOR_RIGHT_KEY	"ALV1" //0x414c5631, r-o length 6
#define BACKLIGHT_KEY 		"LKSB" //0x4c4b5342, w-o

static int debug = 0;

static struct timeval lasttv;
static struct timeval newtv;


static void
ssleep (const int usec)
{
	gettimeofday(&lasttv, NULL);
	while (1) {
		gettimeofday(&newtv, NULL);
		if (((newtv.tv_usec - lasttv.tv_usec) + ((newtv.tv_sec - lasttv.tv_sec)*1000000)) > usec) {
			break;
		}
	}
}

static unsigned char 
get_status (void)
{
	return inb(0x304);
}

static int 
waitfree (char num)
{
	char c, pc = -1;
	int retry = 100;
	while (((c = get_status())&0x0F) != num && retry) {
		ssleep(10);
		retry--;
		if (pc != c) {
			//printf("%x-%d:", c, retry);
			pc = c;
		}
	}
	if (retry == 0) {
		printf("Waitfree failed %x != %x.\n", c, num);
		return 0;
	}
	/*else
		printf("Waitfree ok %x.\n", c);*/

	return 1;
}


static int
writekey (char* key, char len, unsigned char* buffer)
{
	int i;

	outb(0x11, 0x304);
	if (!waitfree(0x0c)) return 0;
	
	for (i = 0; i < 4; i++) {
		outb(key[i], 0x300);
		if (!waitfree(0x04)) return 0;
	}
	if (debug) printf(">%s", key);

	outb(len, 0x300);
	if (debug) printf(">%x", len);

	for (i = 0; i < len; i++) {
		if (!waitfree(0x04)) return 0;
		outb(buffer[i], 0x300);
		if (debug) printf(">%x", buffer[i]);
	}
	if (debug) printf("\n");
	return 1;
}

static int 
readkey (char* key, char len, unsigned char* buffer)
{
	int i; unsigned char c;

	outb(0x10, 0x304);
	if (!waitfree(0x0c)) return 0;
	
	for (i = 0; i < 4; i++) {
		outb(key[i], 0x300);
		if (!waitfree(0x04)) return 0;
	}
	if (debug) printf("<%s", key);

	outb(len, 0x300);
	if (debug) printf(">%x", len);

	for (i = 0; i < len; i++) {
		if (!waitfree(0x05)) return 0;
		c = inb(0x300);
		buffer[i] = c;
		if (debug) printf("<%x", c);
	}
	if (debug) printf("\n");
	return 1;
}

static int 
read_light_sensor (gboolean left)
{
	unsigned char buffer[6];

	if (readkey (left ? LIGHT_SENSOR_LEFT_KEY : LIGHT_SENSOR_RIGHT_KEY, 6, buffer))
		return buffer[2];
	else
		return -1;
}

static int
set_keyboard_backlight (char value)
{
	unsigned char buffer[2];
	buffer[0] = value;
	buffer[1] = 0x00;
	return writekey (BACKLIGHT_KEY, 2, buffer);	
}


#if 0
static int
read_keyboard_backlight (void)
{
	unsigned char buffer[6];

	if (readkey (BACKLIGHT_KEY, 6, buffer))
		return buffer[2];
	else
		return -1;
}
#endif


static int last_keyboard_brightness = -1;

static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *userdata)
{
	DBusError err;
	DBusMessage *reply;
        const char *udi;

        udi = dbus_message_get_path (message);

	/*dbg ("filter_function: sender=%s destination=%s obj_path=%s interface=%s method=%s", 
	     dbus_message_get_sender (message), 
	     dbus_message_get_destination (message), 
	     dbus_message_get_path (message), 
	     dbus_message_get_interface (message),
	     dbus_message_get_member (message));*/

	reply = NULL;
	dbus_error_init (&err);

	if (dbus_message_is_method_call (message, 
					 "org.freedesktop.Hal.Device.LaptopPanel", 
					 "SetBrightness")) {
		int brightness;

                if (!check_priv (halctx, connection, message, udi, "org.freedesktop.hal.power-management.lcd-panel"))
                        goto error;

		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			/* dbg ("setting brightness %d", brightness); */
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

                if (!check_priv (halctx, connection, message, udi, "org.freedesktop.hal.power-management.lcd-panel"))
                        goto error;

		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INVALID)) {

			brightness = read_backlight () - 27;
			if (brightness < 0)
				brightness = 0;
			if (brightness > 228)
				brightness = 228;

			/* dbg ("getting brightness, it's %d", brightness); */

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;

			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
		
	} else if (dbus_message_is_method_call (message, 
						"org.freedesktop.Hal.Device.LightSensor", 
						"GetBrightness")) {
		int brightness[2];

                if (!check_priv (halctx, connection, message, udi, "org.freedesktop.hal.power-management.light-sensor"))
                        goto error;

		brightness[0] = read_light_sensor (FALSE); /* right */
		brightness[1] = read_light_sensor (TRUE); /* left */

		if (brightness[0] == -1 || brightness[1] == -1) {
			reply = dbus_message_new_error (message,
							"org.freedesktop.Hal.Device.LightSensors.Error",
							"Error poking hardware");
			dbus_connection_send (connection, reply, NULL);
		} else {
			int (*pb)[] = &brightness;

			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;
			
			dbus_message_append_args (reply,
						  DBUS_TYPE_ARRAY, DBUS_TYPE_INT32, &pb, 2,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
	} else if (dbus_message_is_method_call (message, 
						"org.freedesktop.Hal.Device.KeyboardBacklight", 
						"GetBrightness")) {

                if (!check_priv (halctx, connection, message, udi, "org.freedesktop.hal.power-management.keyboard-backlight"))
                        goto error;

		/* I can't get this working so just cache last SetBrightness value :-/ */
		if (last_keyboard_brightness == -1 ) {
			reply = dbus_message_new_error (message,
							"org.freedesktop.Hal.Device.KeyboardBacklight.Error",
							"Error poking hardware");
			dbus_connection_send (connection, reply, NULL);
		} else {
			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;
			
			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &last_keyboard_brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
#if 0
		int brightness;

		brightness = read_keyboard_backlight ();

		if (brightness == -1) {
			reply = dbus_message_new_error (message,
							"org.freedesktop.Hal.Device.KeyboardBacklight.Error",
							"Error poking hardware");
			dbus_connection_send (connection, reply, NULL);
		} else {
			reply = dbus_message_new_method_return (message);
			if (reply == NULL)
				goto error;
			
			dbus_message_append_args (reply,
						  DBUS_TYPE_INT32, &brightness,
						  DBUS_TYPE_INVALID);
			dbus_connection_send (connection, reply, NULL);
		}
#endif
	} else if (dbus_message_is_method_call (message, 
						"org.freedesktop.Hal.Device.KeyboardBacklight", 
						"SetBrightness")) {
		int brightness;

                if (!check_priv (halctx, connection, message, udi, "org.freedesktop.hal.power-management.keyboard-backlight"))
                        goto error;

		if (dbus_message_get_args (message, 
					   &err,
					   DBUS_TYPE_INT32, &brightness,
					   DBUS_TYPE_INVALID)) {
			/*dbg ("setting keyboard brightness %d", brightness);*/
			if (brightness < 0 || brightness > 255) {
				reply = dbus_message_new_error (message,
								"org.freedesktop.Hal.Device.KeyboardBacklight.Invalid",
								"Brightness has to be between 0 and 255!");

			} else {
				set_keyboard_backlight (brightness);
				last_keyboard_brightness = brightness;

				reply = dbus_message_new_method_return (message);
				if (reply == NULL)
					goto error;
			}

			dbus_connection_send (connection, reply, NULL);
		}

	}
	
error:
	if (reply != NULL)
		dbus_message_unref (reply);

	LIBHAL_FREE_DBUS_ERROR (&err);

	return DBUS_HANDLER_RESULT_HANDLED;
}

int
main (int argc, char *argv[])
{
 	off_t address = 0;
 	size_t length = 0;
 	int fd;
 	int state;
	int retval = 0;
	struct pci_access *pacc;
 	struct pci_dev *dev;

	DBusError err;

	setup_logger ();
	udi = getenv ("UDI");

	HAL_DEBUG (("udi=%s", udi));
	if (udi == NULL) {
		HAL_ERROR (("No device specified"));
		return -2;
	}

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		HAL_ERROR (("Cannot connect to hald"));
		retval = -3;
		goto out;
	}

	if (!libhal_device_addon_is_ready (halctx, udi, &err)) {
		retval = -4;
		goto out;
	}


	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);
	dbus_connection_set_exit_on_disconnect (conn, 0);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

 	/* Search for the graphics card. */
 	/* Default values: */
 	/* address = 0x90300000; */
 	/* length = 0x20000; */

 	pacc = pci_alloc();
 	pci_init(pacc);
 	pci_scan_bus(pacc);
 	for(dev=pacc->devices; dev; dev=dev->next) {	/* Iterate over all devices */
 		pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);
 		if ((dev->vendor_id == 0x1002) && (dev->device_id == 0x71c5)) { // ATI X1600
 			address = dev->base_addr[2];
 			length = dev->size[2];
 		}
 	}
 	pci_cleanup(pacc);

	HAL_DEBUG (("addr 0x%x len=%d", address, length));
 
 	if (!address) {
 		HAL_DEBUG (("Failed to detect ATI X1600, aborting..."));
		retval = 1;
		goto out;
 	}
 
 	fd = open ("/dev/mem", O_RDWR);
 	
 	if (fd < 0) {
 		HAL_DEBUG (("cannot open /dev/mem"));
		retval = 1;
		goto out;
 	}
 
 	memory = mmap (NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, address);
 
 	if (memory == MAP_FAILED) {
 		HAL_ERROR (("mmap failed"));
		retval = 2;
		goto out;
 	}
 
 	/* Is it really necessary ? */
 	OUTREG(0x4dc, 0x00000005);
 	state = INREG(0x7ae4);
 	OUTREG(0x7ae4, state);

	if (ioperm (0x300, 0x304, 1) < 0) {
		HAL_ERROR (("ioperm failed (you should be root)."));
		exit(1);
	}

	/* this works because we hardcoded the udi's in the <spawn> in the fdi files */
	if (!libhal_device_claim_interface (halctx, 
					    "/org/freedesktop/Hal/devices/macbook_pro_lcd_panel", 
					    "org.freedesktop.Hal.Device.LaptopPanel", 
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.LaptopPanel'"));
		retval = -4;
		goto out;
	}
	if (!libhal_device_claim_interface (halctx, 
					    "/org/freedesktop/Hal/devices/macbook_pro_light_sensor",
					    "org.freedesktop.Hal.Device.LightSensor", 
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"ai\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.LightSensor'"));
		retval = -4;
		goto out;
	}
	if (!libhal_device_claim_interface (halctx, 
					    "/org/freedesktop/Hal/devices/macbook_pro_keyboard_backlight",
					    "org.freedesktop.Hal.Device.KeyboardBacklight", 
					    "    <method name=\"GetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n"
					    "    </method>\n"
					    "    <method name=\"SetBrightness\">\n"
					    "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n"
					    "    </method>\n",
					    &err)) {
		HAL_ERROR (("Cannot claim interface 'org.freedesktop.Hal.Device.KeyboardBacklight'"));
		retval = -4;
		goto out;
	}

	main_loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (main_loop);
	return 0;

out:
        HAL_DEBUG (("An error occured, exiting cleanly"));

        LIBHAL_FREE_DBUS_ERROR (&err);

        if (halctx != NULL) {
                libhal_ctx_shutdown (halctx, &err);
                LIBHAL_FREE_DBUS_ERROR (&err);
                libhal_ctx_free (halctx);
        }

        return retval;
}
