
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include "libhal/libhal.h"

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE !TRUE
#endif

static int verbose = FALSE;

#define dbg(...) do {if (verbose) fprintf (stderr, __VA_ARGS__);} while (0)

/* @todo FIXME HACK: it's a hack to hardcode this */
static const char *usbmapfiles[] = {"/etc/hotplug/usb.usermap", "/etc/hotplug/usb/libsane.usermap", "/etc/hotplug/usb/libgphoto2.usermap",  NULL};

static int
handle_usb_found (const char *module)
{
	char *udi;
	LibHalContext *hal_context = NULL;
	DBusConnection *conn;
	DBusError error;

	udi = getenv ("UDI");
	if (udi == NULL)
		return FALSE;

	dbus_error_init (&error);	
	conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		fprintf (stderr, "error: dbus_bus_get: %s: %s\n", error.name, error.message);
		goto out;
	}		
	if ((hal_context = libhal_ctx_new ()) == NULL) {
		fprintf (stderr, "error: libhal_ctx_new\n");
		goto out;
	}
	if (!libhal_ctx_set_dbus_connection (hal_context, conn)) {
		fprintf (stderr, "error: libhal_ctx_set_dbus_connection: %s: %s\n", error.name, error.message);
		goto out;
	}
	if (!libhal_ctx_init (hal_context, &error)) {
		fprintf (stderr, "error: libhal_ctx_init: %s: %s\n", error.name, error.message);
		goto out;
	}


	if (strcmp (module, "usbcam") == 0 || strcmp(module, "libgphoto2") == 0) {
		libhal_device_add_capability (hal_context, udi, "camera", &error);
		libhal_device_set_property_string (hal_context, udi, "info.category", "camera", &error);
		libhal_device_set_property_string (hal_context, udi, "camera.access_method", "user", &error);
		libhal_device_set_property_bool (hal_context, udi, "camera.libgphoto2_support", TRUE, &error);
	} else if (strcmp (module, "libusbscanner") == 0) {
		libhal_device_add_capability (hal_context, udi, "scanner", &error);
		libhal_device_set_property_string (hal_context, udi, "info.category", "scanner", &error);
		libhal_device_set_property_string (hal_context, udi, "scanner.access_method", "user", &error);
		libhal_device_set_property_bool (hal_context, udi, "scanner.libsane_support", TRUE, &error);
	}

	libhal_ctx_shutdown (hal_context, &error);
	libhal_ctx_free (hal_context);
out:
	return TRUE;
}

static int 
handle_usb_mapfile (const char *mapfile, int target_vendor_id, int target_product_id)
{
	int found;
	FILE *f;
	char buf[256];
	char module[256];
	int match, vendor_id, product_id;

	found = FALSE;
	f = fopen (mapfile, "r");
	if (f == NULL)
		goto out;

	while (fgets (buf, sizeof (buf), f) != NULL) {
		if (buf[0] == '#')
			continue;
		if (sscanf (buf, "%s 0x%x 0x%x 0x%x", module, &match, &vendor_id, &product_id) != 4)
			continue;

		if (match != 0x03)
			continue;

		/*dbg ("entry: %s 0x%04x 0x%04x\n", module, vendor_id, product_id);*/

		if (target_vendor_id == vendor_id && target_product_id == product_id) {
			dbg ("FOUND! %s\n", module);
			handle_usb_found (module);
			found = TRUE;
			goto out;
		}
	}

out:
	if (f != NULL)
		fclose (f);
	return found;
}

int 
main (int argc, char *argv[])
{
	int i;
	char *bus;
	char *vendor_id_str;
	char *product_id_str;
	int vendor_id;
	int product_id;

	if (argc != 2)
		return 1;

	if (strcmp (argv[1], "add") != 0)
		return 0;

	if (getenv ("HALD_VERBOSE") != NULL)
		verbose = TRUE;

	bus = getenv ("HAL_PROP_INFO_BUS");
	if (bus == NULL || strcmp (bus, "usb_device") != 0)
		return 0;

	vendor_id_str = getenv ("HAL_PROP_USB_DEVICE_VENDOR_ID");
	if (vendor_id_str == NULL)
		return 1;
	vendor_id = atoi (vendor_id_str);

	product_id_str = getenv ("HAL_PROP_USB_DEVICE_PRODUCT_ID");
	if (product_id_str == NULL)
		return 1;
	product_id = atoi (product_id_str);

	/* root hubs */
	if (vendor_id == 0 && product_id == 0)
		return 0;

	dbg ("hal_hotplug_map: Checking usermaps for USB device vid=0x%04x pid=0x%04x\n", vendor_id, product_id);

	for (i = 0; usbmapfiles[i] != NULL; i++) {
		if (handle_usb_mapfile (usbmapfiles[i], vendor_id, product_id))
			return 0;
	}

	return 0;
}
