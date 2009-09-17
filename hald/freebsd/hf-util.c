/***************************************************************************
 * CVSID: $Id$
 *
 * hf-util.c : utilities
 *
 * Copyright (C) 2006, 2007 Jean-Yves Lefort <jylefort@FreeBSD.org>
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

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <glib.h>

#include "../hald_runner.h"
#include "../osspec.h"
#include "../util.h"
#include "../device_info.h"

#include "hf-ata.h"
#include "hf-util.h"

typedef struct
{
  gboolean done;
} AsyncInfo;

typedef struct
{
  int exit_type;
  int return_code;
} RunnerInfo;

typedef struct
{
  char *key;
  char *strval;
  int type;
  int intval;
} PropertyBag;

gboolean hf_is_waiting = FALSE;

static void
hf_async_init (AsyncInfo *info)
{
  g_return_if_fail(info != NULL);

  info->done = FALSE;
}

static void
hf_async_wait (AsyncInfo *info)
{
  g_return_if_fail(info != NULL);
  g_return_if_fail(hf_is_waiting == FALSE);

  while (! info->done)
    {
      hf_is_waiting = TRUE;
      g_main_context_iteration(NULL, TRUE);
      hf_is_waiting = FALSE;
    }

  info->done = FALSE;		/* reset in case they want to call us again */
}

HalDevice *
hf_device_new (HalDevice *parent)
{
  HalDevice *device;

  device = hal_device_new();

  hal_device_property_set_string(device, "info.parent", parent ? hal_device_get_udi(parent) : HF_COMPUTER);

  return device;
}

static void
hf_device_callout_done_cb (HalDevice *device, gpointer data1, gpointer data2)
{
  AsyncInfo *async = data1;

  async->done = TRUE;
}

gboolean
hf_device_preprobe (HalDevice *device)
{
  AsyncInfo async;
  gboolean ignore;

  g_return_val_if_fail(HAL_IS_DEVICE(device), FALSE);

  /* add to temporary device store */
  hal_device_store_add(hald_get_tdl(), device);

  /* process preprobe fdi files */
  di_search_and_merge(device, DEVICE_INFO_TYPE_PREPROBE);

  /* run preprobe callouts */
  hf_async_init(&async);
  hal_util_callout_device_preprobe(device, hf_device_callout_done_cb, &async, NULL);
  hf_async_wait(&async);

  ignore = hal_device_property_get_bool(device, "info.ignore");
  if (ignore)
    {
      hal_device_property_remove(device, "info.category");
      hal_device_property_remove(device, "info.capabilities");
      hal_device_property_set_string(device, "info.udi", "/org/freedesktop/Hal/devices/ignored-device");
      hal_device_property_set_string(device, "info.product", "Ignored Device");

      /* move from temporary to global device store */
      hal_device_store_remove(hald_get_tdl(), device);
      hal_device_store_add(hald_get_gdl(), device);
    }

  return ! ignore;
}

void
hf_device_add (HalDevice *device)
{
  AsyncInfo async;

  g_return_if_fail(HAL_IS_DEVICE(device));

  /* process information and policy fdi files */
  di_search_and_merge(device, DEVICE_INFO_TYPE_INFORMATION);
  di_search_and_merge(device, DEVICE_INFO_TYPE_POLICY);

  /* run add callouts */
  hf_async_init(&async);
  hal_util_callout_device_add(device, hf_device_callout_done_cb, &async, NULL);
  hf_async_wait(&async);

  /* move from temporary to global device store */
  hal_device_store_remove(hald_get_tdl(), device);
  hal_device_store_add(hald_get_gdl(), device);
}

gboolean
hf_device_preprobe_and_add (HalDevice *device)
{
  g_return_val_if_fail(HAL_IS_DEVICE(device), FALSE);

  if (hf_device_preprobe(device))
    {
      hf_device_add(device);
      return TRUE;
    }
  else
    return FALSE;
}

void
hf_device_remove (HalDevice *device)
{
  AsyncInfo async;

  g_return_if_fail(HAL_IS_DEVICE(device));

  hf_async_init(&async);
  hal_util_callout_device_remove(device, hf_device_callout_done_cb, &async, NULL);
  hf_async_wait(&async);

  hal_device_store_remove(hald_get_gdl(), device);
  g_object_unref(device);
}

void
hf_device_remove_children (HalDevice *device)
{
  GSList *children;

  g_return_if_fail(HAL_IS_DEVICE(device));

  children = hf_device_store_get_children(hald_get_gdl(), device);
  children = g_slist_reverse(children);	/* remove leaves first */
  g_slist_foreach(children, (GFunc) hf_device_remove, NULL);
  g_slist_free(children);
}

/* remove the device and all its children */
void
hf_device_remove_tree (HalDevice *device)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hf_device_remove_children(device);
  hf_device_remove(device);
}

/*
 * Creates a device store containing the concatenation of the GDL and
 * the ATA pending devices list. This device store differs from the
 * GDL only between the ATA probe and the SCSI probe. It is used to
 * compute an unique UDI in hf_device_set_full_udi().
 */
static HalDeviceStore *
hf_pending_gdl_new (void)
{
  HalDeviceStore *store;
  GList *l;
  GSList *sl;

  store = hal_device_store_new();

  HF_LIST_FOREACH(sl, hald_get_gdl()->devices)
    hal_device_store_add(store, sl->data);

  HF_LIST_FOREACH(l, hf_ata_pending_devices)
    hal_device_store_add(store, l->data);

  return store;
}

static void
hf_pending_gdl_free (HalDeviceStore *store)
{
  g_return_if_fail(HAL_IS_DEVICE_STORE(store));

  /* the device store code fails to do this when a store is finalized */
  while (store->devices)
    hal_device_store_remove(store, store->devices->data);

  g_object_unref(store);
}

void
hf_device_set_udi (HalDevice *device, const char *format, ...)
{
  va_list args;
  char *udi;
  char *safe_str;

  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(format != NULL);

  va_start(args, format);
  udi = g_strdup_vprintf(format, args);
  va_end(args);

  safe_str = hf_str_escape(udi);
  g_free(udi);

  hf_device_set_full_udi(device, "/org/freedesktop/Hal/devices/%s", safe_str);
  g_free(safe_str);
}

void
hf_device_set_full_udi (HalDevice *device, const char *format, ...)
{
  va_list args;
  HalDeviceStore *pending_gdl;
  char *requested_udi;
  char actual_udi[256];

  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(format != NULL);

  /*
   * To ensure an unique UDI, we must work against the GDL as well as
   * the ATA pending devices list (since these devices will be added
   * to the GDL at the end of the probe).
   */
  pending_gdl = hf_pending_gdl_new();

  va_start(args, format);
  requested_udi = g_strdup_vprintf(format, args);
  va_end(args);

  hal_util_make_udi_unique (pending_gdl, actual_udi, sizeof(actual_udi), requested_udi);

  hf_pending_gdl_free(pending_gdl);
  g_free(requested_udi);

  hal_device_set_udi(device, actual_udi);
}

void
hf_device_property_set_string_printf (HalDevice *device,
				      const char *key,
				      const char *format,
				      ...)
{
  char *value = NULL;

  g_return_if_fail(HAL_IS_DEVICE(device));
  g_return_if_fail(key != NULL);

  if (format)
    {
      va_list args;

      va_start(args, format);
      value = g_strdup_vprintf(format, args);
      va_end(args);
    }

  hal_device_property_set_string(device, key, value);
  g_free(value);
}

void
hf_device_set_input (HalDevice *device,
		     const char *capability1,
		     const char *capability2,
		     const char *devname)
{
  g_return_if_fail(HAL_IS_DEVICE(device));

  hal_device_add_capability(device, "input");
  if (capability1)
    {
      char *capability;

      capability = g_strdup_printf("input.%s", capability1);
      hal_device_add_capability(device, capability);
      g_free(capability);
    }
  if (capability2)
    {
      char *capability;

      capability = g_strdup_printf("input.%s", capability2);
      hal_device_add_capability(device, capability);
      g_free(capability);
    }

  hal_device_property_set_string(device, "info.category", "input");

  if (devname)
    hf_device_property_set_string_printf(device, "input.device", "/dev/%s", devname);
  else
    hal_device_property_set_string(device, "input.device", NULL);
}

HalDevice *
hf_device_store_get_parent (HalDeviceStore *store, HalDevice *device)
{
  const char *parent_udi;

  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);
  g_return_val_if_fail(HAL_IS_DEVICE(device), NULL);

  parent_udi = hal_device_property_get_string(device, "info.parent");
  if (parent_udi)
    return hal_device_store_find(store, parent_udi);
  else
    return NULL;
}

GSList *
hf_device_store_get_children (HalDeviceStore *store, HalDevice *device)
{
  GSList *children;
  GSList *tmp;
  GSList *l;

  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);
  g_return_val_if_fail(HAL_IS_DEVICE(device), NULL);

  children = hal_device_store_match_multiple_key_value_string(store, "info.parent", hal_device_get_udi(device));

  /* recurse down the tree */
  tmp = g_slist_copy(children);
  HF_LIST_FOREACH(l, tmp)
    children = g_slist_concat(children, hf_device_store_get_children(store, l->data));
  g_slist_free(tmp);

  return children;
}

gboolean
hf_has_sysctl (const char *format, ...)
{
  va_list args;
  char *name;
  size_t value_len;
  gboolean status;

  g_return_val_if_fail(format != NULL, FALSE);

  va_start(args, format);
  name = g_strdup_vprintf(format, args);
  va_end(args);

  status = sysctlbyname(name, NULL, &value_len, NULL, 0) == 0;

  g_free(name);
  return status;
}

gboolean
hf_get_int_sysctl (int *value, GError **err, const char *format, ...)
{
  va_list args;
  char *name;
  size_t value_len = sizeof(int);
  gboolean status;

  g_return_val_if_fail(value != NULL, FALSE);
  g_return_val_if_fail(format != NULL, FALSE);

  va_start(args, format);
  name = g_strdup_vprintf(format, args);
  va_end(args);

  status = sysctlbyname(name, value, &value_len, NULL, 0) == 0;
  if (! status)
    g_set_error(err, 0, 0, "%s", g_strerror(errno));

  g_free(name);
  return status;
}

char *
hf_get_string_sysctl (GError **err, const char *format, ...)
{
  va_list args;
  char *name;
  size_t value_len;
  char *str = NULL;

  g_return_val_if_fail(format != NULL, FALSE);

  va_start(args, format);
  name = g_strdup_vprintf(format, args);
  va_end(args);

  if (sysctlbyname(name, NULL, &value_len, NULL, 0) == 0)
    {
      str = g_new(char, value_len + 1);
      if (sysctlbyname(name, str, &value_len, NULL, 0) == 0)
	str[value_len] = 0;
      else
	{
	  g_free(str);
	  str = NULL;
	}
    }

  if (! str)
    g_set_error(err, 0, 0, "%s", g_strerror(errno));

  g_free(name);
  return str;
}

char *
hf_run (GError **err, const char *format, ...)
{
  va_list args;
  char *command;
  int exit_status;
  char *output = NULL;

  g_return_val_if_fail(format != NULL, NULL);

  va_start(args, format);
  command = g_strdup_vprintf(format, args);
  va_end(args);

  if (g_spawn_command_line_sync(command, &output, NULL, &exit_status, err))
    {
      if (exit_status != 0)
	{
	  g_set_error(err, 0, 0, "command returned status %i", exit_status);
	  g_free(output);
	  output = NULL;
	}
    }

  g_free(command);
  return output;
}

static void
hf_runner_terminated_cb (HalDevice *device,
			 guint32 exit_type,
			 int return_code,
			 char **error,
			 gpointer data1,
			 gpointer data2)
{
  AsyncInfo *async = data1;
  RunnerInfo *info = data2;

  info->exit_type = exit_type;
  info->return_code = return_code;

  async->done = TRUE;
}

int
hf_runner_run_sync (HalDevice *device,
                    int timeout,
                    const char *command_line, ...)
{
  AsyncInfo async;
  RunnerInfo info;
  GPtrArray *extra_env;
  const char *variable;
  va_list args;

  g_return_val_if_fail(HAL_IS_DEVICE(device), 0);
  g_return_val_if_fail(command_line != NULL, 0);

  if (timeout <= 0)
    timeout = HAL_HELPER_TIMEOUT;

  extra_env = g_ptr_array_new();

  va_start(args, command_line);
  while ((variable = va_arg(args, const char *)))
    {
      const char *value;

      value = va_arg(args, const char *);
      g_assert(value != NULL);

      g_ptr_array_add(extra_env, g_strdup_printf("%s=%s", variable, value));
    }
  va_end(args);

  g_ptr_array_add(extra_env, NULL);

  hf_async_init(&async);
  hald_runner_run(device, command_line, (char **) extra_env->pdata, timeout, hf_runner_terminated_cb, &async, &info);
  hf_async_wait(&async);

  g_ptr_array_foreach(extra_env, (GFunc) g_free, NULL);
  g_ptr_array_free(extra_env, TRUE);

  return info.exit_type == HALD_RUN_SUCCESS ? info.return_code : -1;
}

int
hf_strv_find (char **strv, const char *elem)
{
  int i;

  g_return_val_if_fail(strv != NULL, -1);
  g_return_val_if_fail(elem != NULL, -1);

  for (i = 0; strv[i]; i++)
    if (! strcmp(strv[i], elem))
      return i;

  return -1;
}

static void
hf_property_bag_free (PropertyBag *bag)
{
  g_free(bag->key);
  g_free(bag->strval);

  g_free(bag);
}

HalDevice *
hf_device_store_match (HalDeviceStore *store, ...)
{
  GSList *props = NULL;
  va_list args;
  GSList *a;
  HalDevice *device = NULL;

  g_return_val_if_fail(HAL_IS_DEVICE_STORE(store), NULL);

  va_start(args, store);

  while (TRUE)
    {
      char *key;
      PropertyBag *bag;

      key = va_arg(args, char *);
      if (! key)
        break;

      bag = g_new0(PropertyBag, 1);
      bag->key = g_strdup(key);

      bag->type = va_arg(args, int);
      switch (bag->type)
        {
          case HAL_PROPERTY_TYPE_STRING:
            {
              char *val;

	      val = va_arg(args, char *);
              bag->strval = g_strdup(val ? val : "");
	      break;
	    }
	  case HAL_PROPERTY_TYPE_INT32:
            bag->intval = va_arg(args, int);
	    break;
	  default:
            g_slist_foreach(props, (GFunc) hf_property_bag_free, NULL);
            g_slist_free(props);
	    hf_property_bag_free(bag);

	    va_end(args);

	    g_return_val_if_reached(NULL);
	}

      props = g_slist_prepend(props, bag);
    }

  va_end(args);

  HF_LIST_FOREACH(a, store->devices)
    {
      GSList *b;

      device = (HalDevice *) a->data;

      HF_LIST_FOREACH(b, props)
        {
          PropertyBag *bag;

          bag = (PropertyBag *) b->data;

          switch (bag->type)
            {
              case HAL_PROPERTY_TYPE_STRING:
	        {
		  const char *pstr;

		  pstr = hal_device_property_get_string(device, bag->key);
                  if (! pstr || strcmp(pstr, bag->strval))
                    device = NULL;
                  break;
		}
              case HAL_PROPERTY_TYPE_INT32:
                if (hal_device_property_get_int(device, bag->key) !=
                    bag->intval)
                  device = NULL;
                break;
              default:
                g_assert_not_reached();
            }

	  if (! device)
	    break;
        }

      if (device)
        break;
    }

  g_slist_foreach(props, (GFunc) hf_property_bag_free, NULL);
  g_slist_free(props);

  return device;
}

char *
hf_str_escape (const char *str)
{
  char *safe_str;

  g_return_val_if_fail(str != NULL, NULL);

  safe_str = g_strdup(str);
  g_strcanon(safe_str,
             "_"
	     "abcdefghijklmnopqrstuvwxyz"
	     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	     "1234567890", '_');

  return safe_str;
}
