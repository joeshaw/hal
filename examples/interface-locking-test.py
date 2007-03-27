#!/usr/bin/python


import dbus
import time

# this example show how to prevent automounters from mounting volumes

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)

manager = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                         "/org/freedesktop/Hal/Manager"),
                          "org.freedesktop.Hal.Manager")

manager.AcquireGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage", True)
time.sleep(10)
manager.ReleaseGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage")

