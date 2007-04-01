#!/usr/bin/python


import dbus
import time

# this example show how to prevent automounters from mounting volumes

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)

manager = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                         "/org/freedesktop/Hal/Manager"),
                          "org.freedesktop.Hal.Manager")

# replace this with a volume on your system
device = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                         "/org/freedesktop/Hal/devices/volume_uuid_2232_1F11"),
                          "org.freedesktop.Hal.Device")

device_volume = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                              "/org/freedesktop/Hal/devices/volume_uuid_2232_1F11"),
                               "org.freedesktop.Hal.Device.Volume")

#manager.AcquireGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage", True)
#time.sleep(10)
#manager.ReleaseGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage")

device.AcquireInterfaceLock("org.freedesktop.Hal.Device.Volume", False)
#device_volume.Mount("", "", [])
if device.IsLockedByOthers("org.freedesktop.Hal.Device.Volume"):
    print "device is locked by another process too!"
else:
    print "we are the only process locking the device"    
time.sleep(10)
device.ReleaseInterfaceLock("org.freedesktop.Hal.Device.Volume")

