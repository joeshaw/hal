/***************************************************************************
 * CVSID: $Id$
 *
 * probe-net-bluetooth.c : Probe bluetooth network devices
 *
 * Copyright (C) 2007 Luiz Augusto von Dentz, <luiz.dentz@indt.org.br>
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

#include <string.h>

#include "../../logger.h"
#include "libhal/libhal.h"

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_PATH "/org/bluez"
#define BLUEZ_MANAGER_IFACE "org.bluez.Manager"
#define BLUEZ_NET_PATH "/org/bluez/network"
#define BLUEZ_NET_MANAGER_IFACE "org.bluez.network.Manager"
#define BLUEZ_NET_CONNECTION_IFACE "org.bluez.network.Connection"
#define BLUEZ_NET_SERVER_IFACE "org.bluez.network.Server"

static void
get_properties (DBusConnection *conn, LibHalChangeSet *cs,
                const char *id, const char *path)
{
	DBusMessage *msg;
	DBusMessage *reply = NULL;
	DBusMessageIter reply_iter;
	DBusMessageIter dict_iter;
	DBusError error;

	dbus_error_init (&error);

	msg = dbus_message_new_method_call (id, path, BLUEZ_NET_CONNECTION_IFACE, "GetInfo");

	if (msg == NULL)
		goto out;

	HAL_INFO (("%s.GetInfo()", BLUEZ_NET_CONNECTION_IFACE));
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &error);

	if (dbus_error_is_set (&error) || dbus_set_error_from_message (&error, reply)) {
		dbus_error_free (&error);
		goto out;
	}

	dbus_message_iter_init (reply, &reply_iter);

	if (dbus_message_iter_get_arg_type (&reply_iter) != DBUS_TYPE_ARRAY  &&
	    dbus_message_iter_get_element_type (&reply_iter) != DBUS_TYPE_DICT_ENTRY) {
		goto out;
	}

	dbus_message_iter_recurse (&reply_iter, &dict_iter);

	while (dbus_message_iter_get_arg_type (&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter dict_entry_iter, var_iter;
		const char *key;
		char prop[32];

		dbus_message_iter_recurse (&dict_iter, &dict_entry_iter);
		dbus_message_iter_get_basic (&dict_entry_iter, &key);

		dbus_message_iter_next (&dict_entry_iter);
		dbus_message_iter_recurse (&dict_entry_iter, &var_iter);

                snprintf(prop, sizeof (prop), "net.bluetooth.%s", key);

		/* Make any property found annouced by hal */
		switch (dbus_message_iter_get_arg_type (&var_iter)) {
		case DBUS_TYPE_STRING:
		{
			const char *value;

			dbus_message_iter_get_basic (&var_iter, &value);

			HAL_INFO (("reply: %s:%s", key, value));

			libhal_changeset_set_property_string (cs, prop, value);
			break;
		}
		case DBUS_TYPE_INT32:
		{
			dbus_int32_t value;

			dbus_message_iter_get_basic (&var_iter, &value);

			HAL_INFO (("reply: %s:%d", key, value));

			libhal_changeset_set_property_int (cs, prop, value);
			break;
		}
		default:
			break;
		}
		dbus_message_iter_next (&dict_iter);
	}

out:
	if (msg)
		dbus_message_unref (msg);
	if (reply)
		dbus_message_unref (reply);
	return;
}

int
main (int argc, char *argv[])
{
	char *udi;
	char *iface;
	char *id;
	const char *connection;
	char network[8] = "network";
	const char *pnetwork = network;
	LibHalContext *ctx = NULL;
	LibHalChangeSet *cs = NULL;
	DBusConnection *conn;
	DBusMessage *msg = NULL;
	DBusMessage *reply = NULL;
	DBusError error;

	udi = getenv ("UDI");
	if (udi == NULL)
		goto out;

	iface = getenv ("HAL_PROP_NET_INTERFACE");
	if (iface == NULL)
		goto out;

	HAL_INFO (("Investigating '%s'", iface));

	dbus_error_init (&error);

	if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
		goto out;

	msg = dbus_message_new_method_call (BLUEZ_SERVICE, BLUEZ_PATH, BLUEZ_MANAGER_IFACE, "ActivateService");

	if (msg == NULL)
		goto out;

	HAL_INFO (("%s.ActivateService('%s')", BLUEZ_MANAGER_IFACE, pnetwork));
	dbus_message_append_args (msg, DBUS_TYPE_STRING, &pnetwork, DBUS_TYPE_INVALID);
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &error);

	if (dbus_error_is_set (&error) || dbus_set_error_from_message (&error, reply)) {
		dbus_error_free (&error);
		goto out;
	}

	dbus_message_unref (msg);
	msg = NULL;

	dbus_message_get_args (reply, &error, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID);
	if (dbus_error_is_set (&error)) {
		dbus_error_free (&error);
		goto out;
	}

	dbus_message_unref (reply);
	reply = NULL;

	HAL_INFO (("Found Bluez Network service '%s'", id));

	msg = dbus_message_new_method_call (id, BLUEZ_NET_PATH, BLUEZ_NET_MANAGER_IFACE, "FindConnection");

	if (msg == NULL)
		goto out;

	HAL_INFO (("%s.FindConnection('%s')", BLUEZ_NET_MANAGER_IFACE, iface));
	dbus_message_append_args (msg, DBUS_TYPE_STRING, &iface,
							DBUS_TYPE_INVALID);
	reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &error);

	if (dbus_error_is_set (&error) || dbus_set_error_from_message (&error, reply)) {
		dbus_error_free (&error);
		goto out;
	}

	dbus_message_unref (msg);
	msg = NULL;

	dbus_message_get_args (reply, &error, DBUS_TYPE_STRING, &connection, DBUS_TYPE_INVALID);
	if (dbus_error_is_set (&error)) {
		dbus_error_free (&error);
		goto out;
	}

	ctx = libhal_ctx_init_direct (&error);
	if (ctx == NULL)
		goto out;

	cs = libhal_device_new_changeset (udi);
	if (cs == NULL) {
		HAL_ERROR(("Cannot initialize changeset"));
		goto out;
	}

	get_properties (conn, cs, id, connection);

out:
	if (msg)
		dbus_message_unref (msg);
	if (reply)
		dbus_message_unref (reply);

	LIBHAL_FREE_DBUS_ERROR (&error);

	if (cs != NULL) {
		libhal_device_commit_changeset (ctx, cs, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_device_free_changeset (cs);
	}

	if (ctx != NULL) {
		libhal_ctx_shutdown (ctx, &error);
		LIBHAL_FREE_DBUS_ERROR (&error);
		libhal_ctx_free (ctx);
	}

	return 0;
}
