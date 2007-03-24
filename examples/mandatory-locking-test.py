#!/usr/bin/python


import dbus
import sys
import time
import os

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)
device = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                       "/org/freedesktop/Hal/devices/computer"),
#                                       "/org/freedesktop/Hal/devices/volume_label_EOS_DIGITAL"),
                        "org.freedesktop.Hal.Device")

device.AcquireMandatoryLock("foo")
device.AcquireMandatoryLock("foo2")
time.sleep(2)
device.ReleaseMandatoryLock("foo2")
#device.ReleaseMandatoryLock("foo")
time.sleep(2)
