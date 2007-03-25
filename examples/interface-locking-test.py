#!/usr/bin/python


import dbus
import sys
import time
import os

# this is just various test code

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)

manager = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                         "/org/freedesktop/Hal/Manager"),
                          "org.freedesktop.Hal.Manager")

computer = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                         "/org/freedesktop/Hal/devices/computer"),
                          "org.freedesktop.Hal.Device")

#                                       "/org/freedesktop/Hal/devices/volume_uuid_456C_AAA8"),

device = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                       "/org/freedesktop/Hal/devices/macbook_pro_keyboard_backlight"),
                        "org.freedesktop.Hal.Device")
device2 = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                        "/org/freedesktop/Hal/devices/macbook_pro_keyboard_backlight"),
                         "org.freedesktop.Hal.Device.KeyboardBacklight")

device3 = dbus.Interface(bus.get_object("org.freedesktop.Hal",
                                       "/org/freedesktop/Hal/devices/storage_serial_Kingston_DataTraveler_2_0_07F0E4611101494D"),
                        "org.freedesktop.Hal.Device")

manager.AcquireGlobalInterfaceLock("org.freedesktop.Hal.Device.Storage", True)
time.sleep(10000)

#device3.AcquireInterfaceLock("org.freedesktop.Hal.Device.Storage", True)
#time.sleep(100000)

#manager.AcquireGlobalInterfaceLock("org.freedesktop.Hal.Device.KeyboardBacklight")
#device.AcquireInterfaceLock("org.freedesktop.Hal.Device.KeyboardBacklight")
#n = 0
#while True:
#    time.sleep(1)
#    device2.SetBrightness (n)
#    n = n + 10
#    if (n > 200):
#        n = 0
#manager.ReleaseGlobalInterfaceLock("org.freedesktop.Hal.Device.KeyboardBacklight")
#device.ReleaseInterfaceLock("org.freedesktop.Hal.Device.KeyboardBacklight")

