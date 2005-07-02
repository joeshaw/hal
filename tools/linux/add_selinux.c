/***************************************************************************
 * CVSID: $Id$
 *
 * add_selinux.c : Add selinux mount option if selinux is enabled
 *
 * Copyright (C) 2005 David Zeuthen, <david@fubar.dk>
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

#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <asm/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kdev_t.h>
#include <linux/cdrom.h>
#include <linux/fs.h>
#include <mntent.h>
#include <selinux/selinux.h>

#include "libhal/libhal.h"

static int 
get_selinux_removable_context (security_context_t *newcon)
{
	FILE *fp;
	char buf[255], *ptr;
	size_t plen;
	
	fp = fopen (selinux_removable_context_path(), "r");
	if (!fp)
		return -1;
	
	ptr = fgets_unlocked (buf, sizeof buf, fp);
	fclose (fp);
	
	if (!ptr)
		return -1;
	plen = strlen (ptr);
	if (buf[plen-1] == '\n') 
		buf[plen-1] = 0;
	
	*newcon = strdup (buf);
	/* If possible, check the context to catch
	   errors early rather than waiting until the
	   caller tries to use setexeccon on the context.
	   But this may not always be possible, e.g. if
	   selinuxfs isn't mounted. */
	if (security_check_context(*newcon) && errno != ENOENT) {
		free(*newcon);
		*newcon = 0;
		return -1;
	}
	
	return 0;
}

int 
main (int argc, char *argv[])
{
	char buf[256];
	char *udi;
	char *device_file;
	char *cat;
	LibHalContext *ctx = NULL;
	DBusError error;
	DBusConnection *conn;
	security_context_t scontext;
	dbus_bool_t is_volume;

	if ((udi = getenv ("UDI")) == NULL)
		goto out;
	if ((device_file = getenv ("HAL_PROP_BLOCK_DEVICE")) == NULL)
		goto out;
 	if ((cat = getenv ("HAL_PROP_INFO_CATEGORY")) == NULL)
		goto out;

 	if (strcmp (cat, "volume") == 0) {
		is_volume = TRUE;
 	} else if (strcmp (cat, "storage") == 0) {
		is_volume = FALSE;
	} else {
		goto out;
	}

	if (!is_selinux_enabled ())
		goto out;

	if (get_selinux_removable_context (&scontext) == 0) {

		if (is_volume)
			snprintf (buf, sizeof (buf), "volume.policy.mount_option.fscontext=%s", scontext);
		else
			snprintf (buf, sizeof (buf), "storage.policy.mount_option.fscontext=%s", scontext);
		freecon(scontext);

		dbus_error_init (&error);
		if ((conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error)) == NULL)
			goto out;

		if ((ctx = libhal_ctx_new ()) == NULL)
			goto out;
		if (!libhal_ctx_set_dbus_connection (ctx, conn))
			goto out;
		if (!libhal_ctx_init (ctx, &error))
			goto out;

		libhal_device_set_property_bool (ctx, udi, buf, TRUE, &error);
	}

out:

	if (ctx != NULL) {
		dbus_error_init (&error);
		libhal_ctx_shutdown (ctx, &error);
		libhal_ctx_free (ctx);
	}

	return 0;
}
