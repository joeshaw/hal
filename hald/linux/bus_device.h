/***************************************************************************
 * CVSID: $Id$
 *
 * linux_class_generic.h : Generic methods for class devices
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
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

#ifndef BUS_DEVICE_H
#define BUS_DEVICE_H

#include "libsysfs/libsysfs.h"

/* fwd decl */
struct BusDeviceHandler_s;
typedef struct BusDeviceHandler_s BusDeviceHandler;

/** Method and property table for BusDeviceHandler */
struct BusDeviceHandler_s {
	/** Called when the HAL daemon is starting up 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*init) (BusDeviceHandler *self);

	/** Called when all device detection (on bootstrap) is done 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*detection_done) (BusDeviceHandler *self);

	/** Called just before the HAL daemon is shutting down 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*shutdown) (BusDeviceHandler *self);

	/** Called regulary (every two seconds) for polling etc. 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*tick) (BusDeviceHandler *self);

	/** Called when processing a new device instance to determine 
	 *  whether this class accepts this kind of device.
	 *
	 *  @param  self          Pointer to class members
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the device in sysfs
	 *  @param  device        Libsysfs object representing new device
	 *                        instance
	 *  @return               Must return TRUE if this class should
	 *                        process this device
	 */
	dbus_bool_t (*accept) (BusDeviceHandler *self,
			       const char *sysfs_path,
			       struct sysfs_device *device);

	/** Called to process the new device instance has passed accept().
	 *
	 *  This function should create a HalDevice object - the following
	 *  properties need to be set
	 *
	 *    linux.sysfs_path - must be set to the fully qualified path
	 *                       to the sysfs object (e.g. /sys/devices/..)
	 *
	 *  @param  self          Pointer to class members
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the device in sysfs
	 *  @param  device        Libsysfs object representing new device
	 *                        instance
	 */
	void (*visit) (BusDeviceHandler *self,
		       const char *sysfs_path,
		       struct sysfs_device *device);

	/** Called when the class device instance have been removed
	 *
	 *  @param  self          Pointer to class members
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the class device in sysfs
	 *  @param  d             The HalDevice object of the instance of
	 *                        this device class
	 */
	void (*removed) (BusDeviceHandler *self, 
			 const char *sysfs_path, 
			 HalDevice *d);

	/** This function will compute the device udi based on other properties
	 *  of the device. 
	 *
	 *  Requirements for udi:
	 *   - do not rely on bus, port etc.; we want this id to be as 
	 *     unique for the device as we can
	 *   - make sure it doesn't rely on properties that cannot be obtained
	 *     from the minimal information we can obtain on an unplug event
	 *
	 *  @param  d                   HalDevice object
	 *  @param  append_num          Number to append to name if not -1
	 *  @return                     New unique device id; only good until
	 *                              the next invocation of this function
	 */
	char* (*compute_udi) (HalDevice *d, int append_num);

	/** This method is called just before the device is added to the 
	 *  GDL.
	 *
	 *  This is useful for adding more information about the device.
	 *
	 *  @param  self          Pointer to class members
	 *  @param  d             The HalDevice object of the instance of
	 *                        this device class
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the class device in sysfs
	 *  @param  device        Libsysfs object representing device instance
	 */
	void (*pre_process) (BusDeviceHandler *self,
			     HalDevice *d,
			     const char *sysfs_path,
			     struct sysfs_device *device);

	/** Called when the UDI has been determined, but before the device
	 *  is added to the GDL.
	 *
	 *  @param  self          Pointer to class members
	 *  @param  d             The HalDevice object
	 *  @param  udi           UDI of device
	 */
	void (*got_udi) (BusDeviceHandler *self, 
			 HalDevice *d, 
			 const char *udi);


	/** name of bus the instance handles (name mentioned in /sys/bus) */
	const char *sysfs_bus_name;

	/** hal bus name - property info.bus will be set to this name */
	const char *hal_bus_name;
};

dbus_bool_t bus_device_accept (BusDeviceHandler *self, const char *path, 
			       struct sysfs_device *device);

void bus_device_visit (BusDeviceHandler *self, const char *path, 
		       struct sysfs_device *device);

void bus_device_detection_done (BusDeviceHandler *self);

void bus_device_init (BusDeviceHandler *self);

void bus_device_shutdown (BusDeviceHandler *self);

void bus_device_tick (BusDeviceHandler *self);

void bus_device_removed (BusDeviceHandler *self, 
			 const char *sysfs_path, 
			 HalDevice *d);

void bus_device_pre_process (BusDeviceHandler *self,
			     HalDevice *d,
			     const char *sysfs_path,
			     struct sysfs_device *device);

void bus_device_got_udi (BusDeviceHandler *self,
			 HalDevice *d,
			 const char *udi);

#endif /* BUS_DEVICE_H */
