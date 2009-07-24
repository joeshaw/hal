/*
 * Macbook Backlight Control
 * Copyright © 2006 Ryan Lortie <desrt@desrt.ca>
 * 
 * HAL integration Copyright © 2007 Ryan Lortie <desrt@desrt.ca>
 * using code Copyright © 2006 David Zeuthen <david@fubar.dk>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02110 USA
 *
 * This program was written after I reverse engineered the
 * AppleIntelIntegratedFramebuffer.kext kernel extension in Mac OS X and
 * played with the register at the memory location I found therein.
 *
 * From my experiments, the register appears to have two halves.
 *
 * yyyyyyyyyyyyyyy0xxxxxxxxxxxxxxx0
 *
 * The top (y) bits appear to be the maximum brightness level and the
 * bottom (x) bits are the current brightness level.  0s are always 0.
 * The brightness level is, therefore, x/y.
 *
 * As my Macbook boots, y is set to 0x94 and x is set to 0x1f.  Going below
 * 0x1f produces odd results.  For example, if you come from above, the
 * backlight will completely turn off at 0x12 (18).  Coming from below,
 * however, you need to get to 0x15 (21) before the backlight comes back on.
 *
 * Since there is no clear cut boundry, I assume that this value specifies
 * a raw voltage.  Also, it appears that the bootup value of 0x1f corresponds
 * to the lowest level that Mac OS X will set the backlight I choose this
 * value as a minimum.
 *
 * For the maximum I do not let the value exceed the value in the upper 15
 * bits.
 *
 * Turning the backlight off entirely is not supported (as this is supported
 * by the kernel itself).  This utility is only for setting the brightness
 * of the backlight when it is enabled.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <pci/pci.h>

#include <glib.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DBUS_API_SUBJECT_TO_CHANGE

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include "libhal/libhal.h"
#include "../../logger.h"
#include "../../util_helper.h"
#include "../../util_helper_priv.h"

#define REGISTER_OFFSET       0x00061254
#define PAGE_SIZE             4096
#define PAGE_MASK             (PAGE_SIZE - 1)

#define ACCESS_OFFSET         (REGISTER_OFFSET & PAGE_MASK)
#define ACCESS_INDEX          (ACCESS_OFFSET >> 2)

static unsigned int *register_page;
static LibHalContext *halctx = NULL;

static unsigned long
determine_video_base_address (void)
{
	struct pci_access *pacc;
	struct pci_dev *pdev;
	unsigned long address;
	int i;

	address = 0;

	pacc = pci_alloc ();
	pci_init (pacc);
	pci_scan_bus (pacc);

	for (pdev = pacc->devices; pdev; pdev = pdev->next) {
		pci_fill_info (pdev, PCI_FILL_IDENT | PCI_FILL_BASES);

		if (pdev->vendor_id == 0x8086 && pdev->device_id == 0x27a2)
			for (i = 0; i < (int) G_N_ELEMENTS (pdev->base_addr); i++) {
				if (pdev->size[i] == 512 * 1024) {
					address = pdev->base_addr[i];
					goto end;
				}
			}
	}

end:
	pci_cleanup (pacc);

	return address;
}

static gboolean
map_register_page (void)
{
	unsigned long address;
	int fd;

	address = determine_video_base_address ();

	if (address == 0) {
		HAL_ERROR (("Unable to locate video base address"));
		return FALSE;
	}

	fd = open ("/dev/mem", O_RDWR);

	if (fd < 0) {
		HAL_ERROR (("failed to open /dev/mem"));
		return FALSE;
	}

	register_page = mmap (NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			      MAP_SHARED, fd, (address + REGISTER_OFFSET) & ~PAGE_MASK);

	close (fd);

	drop_privileges (FALSE);

	if (register_page == MAP_FAILED) {
		HAL_ERROR (("failed to mmap"));
		return FALSE;
	}

	return TRUE;
}

static unsigned long
register_get (void)
{
	return register_page[ACCESS_INDEX];
}

static void
register_set (unsigned long value)
{
	register_page[ACCESS_INDEX] = value;
}

static gboolean
backlight_set (long value)
{
	long max;

	max = register_get () >> 17;

	/* after resume the register might be set to zero; fix this */
	if (max == 0x00)
		max = 0x94;

	/* sanity check: this should always be 0x94 */
	if (max != 0x94)
		return FALSE;

	value = CLAMP (value, 0x1f, max);

	register_set ((max << 17) | (value << 1));

	return TRUE;
}

static long
backlight_get (void)
{
	return (register_get () >> 1) & 0x7fff;
}


#define BACKLIGHT_OBJECT \
  "/org/freedesktop/Hal/devices/macbook_backlight"
#define BACKLIGHT_IFACE \
  "org.freedesktop.Hal.Device.LaptopPanel"
#define INTERFACE_DESCRIPTION \
  "    <method name=\"SetBrightness\">\n" \
  "      <arg name=\"brightness_value\" direction=\"in\" type=\"i\"/>\n" \
  "      <arg name=\"return_code\" direction=\"out\" type=\"i\"/>\n" \
  "    </method>\n" \
  "    <method name=\"GetBrightness\">\n" \
  "      <arg name=\"brightness_value\" direction=\"out\" type=\"i\"/>\n" \
  "    </method>\n"

static DBusHandlerResult
filter_function (DBusConnection * connection, DBusMessage * message, void *userdata)
{
	DBusMessage *reply;
	DBusError err;
	int level;
	int ret;

        if (!check_priv (halctx, connection, message, dbus_message_get_path (message), "org.freedesktop.hal.power-management.lcd-panel")) {
                return DBUS_HANDLER_RESULT_HANDLED;
        }

	reply = NULL;
	ret = 0;

	dbus_error_init (&err);

	if (dbus_message_is_method_call (message, BACKLIGHT_IFACE, "SetBrightness")) {

		if (dbus_message_get_args (message, &err, DBUS_TYPE_INT32,
					   &level, DBUS_TYPE_INVALID)) {
			backlight_set (level + 0x1f);

			if ((reply = dbus_message_new_method_return (message)))
				dbus_message_append_args (reply, DBUS_TYPE_INT32,
							  &ret, DBUS_TYPE_INVALID);
		}
	} else if (dbus_message_is_method_call (message, BACKLIGHT_IFACE, "GetBrightness")) {
		if (dbus_message_get_args (message, &err, DBUS_TYPE_INVALID)) {
			level = backlight_get () - 0x1f;
			level = CLAMP (level, 0, 117);

			if ((reply = dbus_message_new_method_return (message)))
				dbus_message_append_args (reply, DBUS_TYPE_INT32,
							  &level, DBUS_TYPE_INVALID);
		}
	}

	if (reply) {
		dbus_connection_send (connection, reply, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	LIBHAL_FREE_DBUS_ERROR (&err);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int
main (int argc, char **argv)
{
	DBusConnection *conn;
	GMainLoop *main_loop;
	const char *udi;
	DBusError err;
	int retval = 0;

	setup_logger ();
	udi = getenv ("UDI");

	if (udi == NULL)
		HAL_ERROR (("no device specified"));

	dbus_error_init (&err);
	if ((halctx = libhal_ctx_init_direct (&err)) == NULL) {
		HAL_ERROR (("cannot connect to hald"));
		retval = -4;
		goto out;
	}

	if (!libhal_device_addon_is_ready (halctx, udi, &err)) {
		HAL_ERROR (("libhal_device_addon_is_ready returned false, exit now"));
		retval = -4;
		goto out;
	}

	if (!map_register_page ()) {
		HAL_ERROR (("failed to gain access to the video card"));
	}

	conn = libhal_ctx_get_dbus_connection (halctx);
	dbus_connection_setup_with_g_main (conn, NULL);
	dbus_connection_set_exit_on_disconnect (conn, 0);

	dbus_connection_add_filter (conn, filter_function, NULL, NULL);

	if (!libhal_device_claim_interface (halctx, BACKLIGHT_OBJECT,
					    BACKLIGHT_IFACE, INTERFACE_DESCRIPTION, &err)) {
		HAL_ERROR (("cannot claim interface"));
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

