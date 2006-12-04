/***************************************************************************
 * CVSID: $Id$
 *
 * hf-pci.h : poll for ACPI properties
 *
 * Copyright (C) 2006 Joe Marcus Clarke <marcus@FreeBSD.org>
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

#ifndef _HF_ACPI_H
#define _HF_ACPI_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "hf-osspec.h"
#include "hf-devd.h"

extern HFHandler hf_acpi_handler;
extern HFDevdHandler hf_acpi_devd_handler;

void hf_acpi_button_set_properties (HalDevice *device);
void hf_acpi_acad_set_properties (HalDevice *device);
void hf_acpi_battery_set_properties (HalDevice *device);
void hf_acpi_tz_set_properties (HalDevice *device);

#endif /* _HF_ACPI_H */
