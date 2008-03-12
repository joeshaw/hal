/*
 * Licensed under the GNU General Public License Version 2
 *
 * Copyright (C) 2005-2008 Danny Kukawka <danny.kukawka@web.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <syslog.h>

#define MAX_CMD_LENGTH 256

static int debug = 0;


int main (int argc, const char *argv[])
{
	int ret_val = EXIT_FAILURE;
    	char *udi = NULL;
	char *irq = NULL;
	char *port = NULL;
	char *baud_base = NULL;
	char *input_dev = NULL;
	char cmd[MAX_CMD_LENGTH+1];
	
	if (getenv ("HALD_VERBOSE") != NULL )
        	debug = 1;
	if (debug)
        	syslog (LOG_INFO, "hal-system-setserial started in debug mode." );

	udi = getenv("UDI");
	irq = getenv("HAL_PROP_PNP_SERIAL_IRQ");
	port = getenv("HAL_PROP_PNP_SERIAL_PORT");
	baud_base = getenv("HAL_PROP_PNP_SERIAL_BAUD_BASE");
	input_dev = getenv("HAL_PROP_INPUT_DEVICE_SET");

	if (udi == NULL || irq == NULL || port == NULL || input_dev == NULL) {
		syslog (LOG_INFO, "Missing env variable, exit NOW." );
		return ret_val;
	}

	if (baud_base != NULL)
		snprintf( cmd, MAX_CMD_LENGTH, "/bin/setserial %s port %s irq %s baud_base %s autoconfig", input_dev, port, irq, baud_base);
	else
		snprintf( cmd, MAX_CMD_LENGTH, "/bin/setserial %s port %s irq %s autoconfig", input_dev, port, irq);

	syslog (LOG_INFO, "Collected setserial options and called(%d): %s ", system(cmd), cmd);

	ret_val = EXIT_SUCCESS;

	return ret_val;
}

