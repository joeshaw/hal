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

#ifndef CLASS_DEVICE_H
#define CLASS_DEVICE_H

#include "libsysfs/libsysfs.h"

/* fwd decl */
struct ClassDeviceHandler_s;
typedef struct ClassDeviceHandler_s ClassDeviceHandler;

/** Method and property table for ClassDeviceHandler */
struct ClassDeviceHandler_s {
	/** Called when the HAL daemon is starting up 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*init) (ClassDeviceHandler* self);

	/** Called when all device detection (on bootstrap) is done 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*detection_done) (ClassDeviceHandler* self);

	/** Called just before the HAL daemon is shutting down 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*shutdown) (ClassDeviceHandler* self);

	/** Called regulary (every two seconds) for polling etc. 
	 *
	 *  @param  self        Pointer to class members
	 */
	void (*tick) (ClassDeviceHandler* self);

	/** Called when a new instance of a class device is detected either
	 *  through hotplug or through initial detection.
	 *
	 *  This function should create a HalDevice object if we are interested
	 *  in the class device (e.g. if it maps to a particular hardware 
	 *  device). If a HalDevice object is created and we need to wait for
	 *  an udev event, then the string property 
	 *
	 *    .udev.sysfs_path 
	 *
	 *  must be set to the sysfs_path given and the string property
	 *
	 *    .udev.class_name
	 *
	 *  need to carry the appropriate class device name (e.g. from 
	 *  /sys/class such as input, net).
	 *  
	 *
	 *  @param  self          Pointer to class members
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the class device in sysfs
	 *  @param  class_device  Libsysfs object representing new class device
	 *                        instance
	 *  @param  is_probing    Set to TRUE only initial detection
	 */
	void (*visit) (ClassDeviceHandler* self,
		       const char *sysfs_path,
		       struct sysfs_class_device *class_device,
		       dbus_bool_t is_probing);

	/** Called when the class device instance have been removed
	 *
	 *  @param  self          Pointer to class members
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the class device in sysfs
	 *  @param  d             The HalDevice object of the instance of
	 *                        this device class
	 */
	void (*removed) (ClassDeviceHandler* self, 
			 const char *sysfs_path, 
			 HalDevice *d);

	/** Called when the device file (e.g. a file in /dev) have
	 *  been created for a particual instance of this class device
	 *
	 *  @param  self          Pointer to class members
	 *  @param  d             The HalDevice object of the instance of
	 *                        this device class
	 *  @param  dev_file      Device file, e.g. /udev/input/event4
	 */
	void (*udev_event) (ClassDeviceHandler* self,
			    HalDevice *d, char *dev_file);

	/** Get the name of that the property that the device file should
	 *  be put in
	 *
	 *  @param  self          Pointer to class members
	 *  @param  d             The HalDevice object of the instance of
	 *                        this device class
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the class device in sysfs
	 *  @param  class_device  Libsysfs object representing class device
	 *                        instance
	 *  @param  dev_file_prop Device file property name (out)
	 *  @param  dev_file_prop_len  Maximum length of string
	 */
	void (*get_device_file_target) (ClassDeviceHandler* self,
					HalDevice *d, 
					const char *sysfs_path,
					struct sysfs_class_device *class_device,
					char* dev_file_prop,
					int dev_file_prop_len);

	/** This method is called just before the device is either merged
	 *  onto the sysdevice or added to the GDL (cf. merge_or_add). 
	 *  This is useful for extracting more information about the device
	 *  through e.g. ioctl's using the device file property and also
	 *  for setting info.category|capability.
	 *
	 *  @param  self          Pointer to class members
	 *  @param  d             The HalDevice object of the instance of
	 *                        this device class
	 *  @param  sysfs_path    The path in sysfs (including mount point) of
	 *                        the class device in sysfs
	 *  @param  class_device  Libsysfs object representing class device
	 *                        instance
	 */
	void (*post_process) (ClassDeviceHandler* self,
			      HalDevice *d,
			      const char *sysfs_path,
			      struct sysfs_class_device *class_device);	

	/** This function will compute the device udi based on other properties
	 *  of the device. 
	 *
	 *  It only makes sense to implement this method if, and only if,
	 *  merge_or_add==FALSE
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


	/** name of device class the instance handles (name mentioned 
	 *  in /sys/class */
	const char *sysfs_class_name;

	/** hal class name - if merge_or_add==FALSE then info.bus will have
	 *  this name */
	const char* hal_class_name;

	/** TRUE if the class device should get the device file from
	 *  udev (using udevinfo on probing / waiting for dbus signal
	 *  on hotplug). FALSE if there is no special device file for
	 *  the device class (such as net devices).
	 *
	 *  If set to TRUE then class_device_target_property must be 
	 *  implemented.
	 */
	dbus_bool_t require_device_file;

	/** TRUE if the class device should be merged onto the sysdevice;
	 *  if FALSE the class device will be added as a child to the 
	 *  parent of the sysdevice */
	dbus_bool_t merge_or_add;
};

void class_device_visit (ClassDeviceHandler *self,
			 const char *path,
			 struct sysfs_class_device *class_device,
			 dbus_bool_t is_probing);

void class_device_removed (ClassDeviceHandler* self, 
			   const char *sysfs_path, 
			   HalDevice *d);

void class_device_udev_event (ClassDeviceHandler *self,
			      HalDevice *d, char *dev_file);

void class_device_init (ClassDeviceHandler *self);

void class_device_detection_done (ClassDeviceHandler *self);

void class_device_shutdown (ClassDeviceHandler *self);

void class_device_post_process (ClassDeviceHandler *self,
				HalDevice *d,
				const char *sysfs_path,
				struct sysfs_class_device *class_device);

void class_device_tick (ClassDeviceHandler *self);

void class_device_get_device_file_target (ClassDeviceHandler *self,
					  HalDevice *d,
					  const char *sysfs_path,
					  struct sysfs_class_device *class_device,
					  char* dev_file_prop,
					  int dev_file_prop_len);

void class_device_got_sysdevice (HalDeviceStore *store, 
				 HalDevice *sysdevice, 
				 gpointer user_data);

void class_device_got_parent_device (HalDeviceStore *store, 
				     HalDevice *parent, 
				     gpointer user_data);


#endif /* CLASS_DEVICE_H */
