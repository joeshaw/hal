#!/usr/bin/env python

import dbus

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)

hal_service = bus.get_service("org.freedesktop.Hal")
hal_manager = hal_service.get_object("/org/freedesktop/Hal/Manager",
                                     "org.freedesktop.Hal.Manager")

device_names = hal_manager.GetAllDevices()
print device_names

for name in device_names:
    device = hal_service.get_object(name, "org.freedesktop.Hal.Device")
    print "device=%s"%name
    properties = device.GetAllProperties()
    for p in properties:
        print "  %s=%s"%(p,properties[p])
    print ""

