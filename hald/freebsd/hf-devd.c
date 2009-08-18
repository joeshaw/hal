/***************************************************************************
 * CVSID: $Id$
 *
 * hf-devd.c : process devd events
 *
 * Copyright (C) 2006, 2007 Joe Marcus Clarke <marcus@FreeBSD.org>
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
#include <errno.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../logger.h"
#include "../osspec.h"

#include "hf-devd.h"
#include "hf-devtree.h"
#include "hf-acpi.h"
#include "hf-net.h"
#include "hf-pcmcia.h"
#include "hf-storage.h"
#include "hf-usb.h"
#ifdef HAVE_LIBUSB20
#include "hf-usb2.h"
#endif
#include "hf-util.h"

#define HF_DEVD_SOCK_PATH		"/var/run/devd.pipe"

#define HF_DEVD_EVENT_NOTIFY		'!'
#define HF_DEVD_EVENT_ADD		'+'
#define HF_DEVD_EVENT_REMOVE		'-'
#define HF_DEVD_EVENT_NOMATCH		'?'

static HFDevdHandler *handlers[] = {
#ifdef HAVE_LIBUSB20
  &hf_usb2_devd_handler,
#endif
#if __FreeBSD_version < 800092
  &hf_usb_devd_handler,
#endif
  &hf_net_devd_handler,
  &hf_acpi_devd_handler,
  &hf_pcmcia_devd_handler,
  &hf_storage_devd_handler
};

static gboolean hf_devd_inited = FALSE;

static void hf_devd_init (void);

static GHashTable *
hf_devd_parse_params (const char *str)
{
  GHashTable *params;
  char **pairs;
  int i;

  g_return_val_if_fail(str != NULL, FALSE);

  params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  pairs = g_strsplit(str, " ", 0);
  for (i = 0; pairs[i]; i++)
    {
      char *equal;

      equal = strchr(pairs[i], '=');
      g_hash_table_insert(params,
			  equal ? g_strndup(pairs[i], equal - pairs[i]) : g_strdup(pairs[i]),
			  equal ? g_strdup(equal + 1) : NULL);
    }
  g_strfreev(pairs);

  return params;
}

static gboolean
hf_devd_parse_add_remove (const char *event,
			  char **name,
			  GHashTable **params,
			  GHashTable **at,
			  char **parent)
{
  char *params_ptr;
  char *at_ptr;
  char *parent_ptr;

  /*
+ugen0 vendor=0x0a12 product=0x0001 devclass=0xe0 devsubclass=0x01 release=0x0525 sernum="" at port=0 vendor=0x0a12 product=0x0001 devclass=0xe0 devsubclass=0x01 release=0x0525 sernum="" on uhub3

-ugen0 vendor=0x0a12 product=0x0001 devclass=0xe0 devsubclass=0x01 release=0x0525 sernum="" at port=0 vendor=0x0a12 product=0x0001 devclass=0xe0 devsubclass=0x01 release=0x0525 sernum="" on uhub3
  */

  g_return_val_if_fail(event != NULL, FALSE);
  g_return_val_if_fail(name != NULL, FALSE);
  g_return_val_if_fail(params != NULL, FALSE);
  g_return_val_if_fail(at != NULL, FALSE);
  g_return_val_if_fail(parent != NULL, FALSE);

  if ((params_ptr = strchr(event, ' '))
      && (at_ptr = strstr(params_ptr + 1, " at "))
      && (parent_ptr = strstr(at_ptr + 4, " on ")))
    {
      char *params_str;
      char *at_str;

      *name = g_strndup(event, params_ptr - event);
      params_str = g_strndup(params_ptr + 1, at_ptr - params_ptr - 1);
      at_str = g_strndup(at_ptr + 4, parent_ptr - at_ptr - 4);
      *parent = g_strdup(parent_ptr + 4);

      if (! strcmp(*parent, ".")) /* sys/kern/subr_bus.c */
	{
	  g_free(*parent);
	  *parent = NULL;
	}

      *params = hf_devd_parse_params(params_str);
      g_free(params_str);

      *at = hf_devd_parse_params(at_str);
      g_free(at_str);

      return TRUE;
    }
  else
    return FALSE;
}

static gboolean
hf_devd_parse_nomatch (const char *event,
                       GHashTable **at,
		       char **parent)
{
  char *at_ptr;
  char *parent_ptr;

  /*
? at port=0 vendor=0x0a12 product=0x0001 devclass=0xe0 devsubclass=0x01 release=0x0525 sernum="" on uhub3
  */

  g_return_val_if_fail(event != NULL, FALSE);
  g_return_val_if_fail(at != NULL, FALSE);
  g_return_val_if_fail(parent != NULL, FALSE);

  if ((at_ptr = strstr(event, " at "))
     && (parent_ptr = strstr(at_ptr + 4, " on ")))
    {
      char *at_str;

      at_str = g_strndup(at_ptr + 4, parent_ptr - at_ptr - 4);
      *parent = g_strdup(parent_ptr + 4);

      if (! strcmp(*parent, ".")) /* sys/kern/subr_bus.c */
        {
          g_free(*parent);
	  *parent = NULL;
	}

      *at = hf_devd_parse_params(at_str);
      g_free(at_str);

      return TRUE;
    }
  else
    return FALSE;
}

static gboolean
hf_devd_parse_notify (const char *event,
		      char **system,
		      char **subsystem,
		      char **type,
		      char **data)
{
  char **items;
  gboolean status = FALSE;

  /*
!system=IFNET subsystem=bge0 type=LINK_DOWN
  */

  g_return_val_if_fail(event != NULL, FALSE);
  g_return_val_if_fail(system != NULL, FALSE);
  g_return_val_if_fail(subsystem != NULL, FALSE);
  g_return_val_if_fail(type != NULL, FALSE);
  g_return_val_if_fail(data != NULL, FALSE);

  items = g_strsplit(event, " ", 0);
  if (g_strv_length(items) < 3)
    goto end;

  if (! g_str_has_prefix(items[0], "system=") ||
      ! g_str_has_prefix(items[1], "subsystem=") ||
      ! g_str_has_prefix(items[2], "type="))
    goto end;

  *system = g_strdup(items[0] + 7);
  *subsystem = g_strdup(items[1] + 10);
  *type = g_strdup(items[2] + 5);
  *data = g_strdup(items[3]);	/* may be NULL */

  status = TRUE;

 end:
  g_strfreev(items);
  return status;
}

static void
hf_devd_process_add_event (const char *name,
			   GHashTable *params,
			   GHashTable *at,
			   const char *parent)
{
  int i;

  g_return_if_fail(name != NULL);
  g_return_if_fail(params != NULL);
  g_return_if_fail(at != NULL);

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->add && handlers[i]->add(name, params, at, parent))
      return;

  /* no match, default action: probe to catch the new device */

  osspec_probe();
}

static void
hf_devd_process_remove_event (const char *name,
			      GHashTable *params,
			      GHashTable *at,
			      const char *parent)
{
  int i;
  HalDevice *device;

  g_return_if_fail(name != NULL);
  g_return_if_fail(params != NULL);
  g_return_if_fail(at != NULL);

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->remove && handlers[i]->remove(name, params, at, parent))
      return;

  /* no match, default action: remove the device and its children */

  device = hf_devtree_find_from_name(hald_get_gdl(), name);
  if (device)
    hf_device_remove_tree(device);
}

static void
hf_devd_process_notify_event (const char *system,
			      const char *subsystem,
			      const char *type,
			      const char *data)
{
  int i;

  g_return_if_fail(system != NULL);
  g_return_if_fail(subsystem != NULL);
  g_return_if_fail(type != NULL);
  g_return_if_fail(data != NULL);

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->notify && handlers[i]->notify(system, subsystem, type, data))
      return;

  /* no default action */
}

static void
hf_devd_process_nomatch_event (GHashTable *at, const char *parent)
{
  int i;

  g_return_if_fail(at != NULL);

  for (i = 0; i < (int) G_N_ELEMENTS(handlers); i++)
    if (handlers[i]->nomatch && handlers[i]->nomatch(at, parent))
      return;
}

static void
hf_devd_process_event (const char *event)
{
  g_return_if_fail(event != NULL);

  HAL_INFO(("received devd event: %s", event));

  switch (event[0])
    {
    case HF_DEVD_EVENT_ADD:
    case HF_DEVD_EVENT_REMOVE:
      {
	char *name;
	GHashTable *params;
	GHashTable *at;
	char *parent;

	if (! hf_devd_parse_add_remove(event + 1, &name, &params, &at, &parent))
	  goto malformed;

	if (event[0] == HF_DEVD_EVENT_ADD)
	  hf_devd_process_add_event(name, params, at, parent);
	else
	  hf_devd_process_remove_event(name, params, at, parent);

	g_free(name);
	g_hash_table_destroy(params);
	g_hash_table_destroy(at);
	g_free(parent);
      }
      return;

    case HF_DEVD_EVENT_NOTIFY:
      {
	char *system;
	char *subsystem;
	char *type;
	char *data;

	if (! hf_devd_parse_notify(event + 1, &system, &subsystem, &type, &data))
	  goto malformed;

	hf_devd_process_notify_event(system, subsystem, type, data);

	g_free(system);
	g_free(subsystem);
	g_free(type);
	g_free(data);
      }
      return;

    case HF_DEVD_EVENT_NOMATCH:
      {
        GHashTable *at;
        char *parent;

        if (! hf_devd_parse_nomatch(event + 1, &at, &parent))
          goto malformed;

	hf_devd_process_nomatch_event(at, parent);

        g_hash_table_destroy(at);
        g_free(parent);
      }
      return;
    }

 malformed:
  HAL_WARNING(("malformed devd event: %s", event));
}

static gboolean
hf_devd_event_cb (GIOChannel *source, GIOCondition condition,
                      gpointer user_data)
{
  char *event;
  gsize terminator;
  GIOStatus status;

  if (hf_is_waiting)
    return TRUE;

  status = g_io_channel_read_line(source, &event, NULL, &terminator, NULL);

  if (status == G_IO_STATUS_NORMAL)
    {
      event[terminator] = 0;
      hf_devd_process_event(event);
      g_free(event);
    }
  else if (status == G_IO_STATUS_AGAIN)
    {
      hf_devd_init();
      if (hf_devd_inited)
        {
          int fd;

	  fd = g_io_channel_unix_get_fd(source);
	  g_io_channel_shutdown(source, FALSE, NULL);
	  close(fd);

	  return FALSE;
	}
    }

  return TRUE;
}

static void
hf_devd_init (void)
{
  int event_fd;
  struct sockaddr_un addr;

  event_fd = socket(PF_UNIX, SOCK_STREAM, 0);
  if (event_fd < 0)
    {
      HAL_WARNING(("failed to create event socket: %s", g_strerror(errno)));
      hf_devd_inited = FALSE;
      return;
    }

  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, HF_DEVD_SOCK_PATH, sizeof(addr.sun_path));
  if (connect(event_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    {
      GIOChannel *channel;

      channel = g_io_channel_unix_new(event_fd);
      g_io_add_watch(channel, G_IO_IN, hf_devd_event_cb, NULL);
      g_io_channel_unref(channel);
      hf_devd_inited = TRUE;
    }
  else
    {
      HAL_WARNING(("failed to connect to %s: %s", HF_DEVD_SOCK_PATH,
                   g_strerror(errno)));
      close(event_fd);
      hf_devd_inited = FALSE;
    }
}

HFHandler hf_devd_handler = {
  .init = hf_devd_init
};
