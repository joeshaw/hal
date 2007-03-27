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

manager.AcquireGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage", True)
#device.AcquireInterfaceLock("org.freedesktop.Hal.Device.Volume", True)
time.sleep(2)
manager.ReleaseGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage")
#device.ReleaseInterfaceLock("org.freedesktop.Hal.Device.Volume")

