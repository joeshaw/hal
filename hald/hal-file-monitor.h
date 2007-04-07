/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifndef HAL_FILE_MONITOR_H
#define HAL_FILE_MONITOR_H

#include <glib-object.h>

G_BEGIN_DECLS

#define HAL_TYPE_FILE_MONITOR         (hal_file_monitor_get_type ())
#define HAL_FILE_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), HAL_TYPE_FILE_MONITOR, HalFileMonitor))
#define HAL_FILE_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), HAL_TYPE_FILE_MONITOR, HalFileMonitorClass))
#define HAL_IS_FILE_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), HAL_TYPE_FILE_MONITOR))
#define HAL_IS_FILE_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), HAL_TYPE_FILE_MONITOR))
#define HAL_FILE_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), HAL_TYPE_FILE_MONITOR, HalFileMonitorClass))

typedef struct HalFileMonitorPrivate HalFileMonitorPrivate;

typedef struct
{
        GObject               parent;
        HalFileMonitorPrivate *priv;
} HalFileMonitor;

typedef struct
{
        GObjectClass parent_class;
} HalFileMonitorClass;

typedef enum
{
        HAL_FILE_MONITOR_EVENT_NONE    = 1 << 0,
        HAL_FILE_MONITOR_EVENT_ACCESS  = 1 << 1,
        HAL_FILE_MONITOR_EVENT_CREATE  = 1 << 2,
        HAL_FILE_MONITOR_EVENT_DELETE  = 1 << 3,
        HAL_FILE_MONITOR_EVENT_CHANGE  = 1 << 4,
} HalFileMonitorEvent;

typedef enum
{
        HAL_FILE_MONITOR_ERROR_GENERAL
} HalFileMonitorError;

#define HAL_FILE_MONITOR_ERROR hal_file_monitor_error_quark ()

typedef void (*HalFileMonitorNotifyFunc) (HalFileMonitor      *monitor,
                                          HalFileMonitorEvent  event,
                                          const char         *path,
                                          gpointer            user_data);

GQuark              hal_file_monitor_error_quark           (void);
GType               hal_file_monitor_get_type              (void);

HalFileMonitor     * hal_file_monitor_new                   (void);

guint               hal_file_monitor_add_notify            (HalFileMonitor          *monitor,
                                                           const char             *path,
                                                           int                     mask,
                                                           HalFileMonitorNotifyFunc notify_func,
                                                           gpointer                data);
void                hal_file_monitor_remove_notify         (HalFileMonitor          *monitor,
                                                           guint                   id);

G_END_DECLS

#endif /* HAL_FILE_MONITOR_H */
