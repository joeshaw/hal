#!/usr/bin/python
#
# Simple program to set the keyboard backlight according to how much
# light is around. This is a very rough example, real production code
# needs to poll much less often and probably fade the backlight from
# 3-4 predefined values instead of jumping around like crazy.
#
# Copyright (C) 2006 David Zeuthen <david@fubar.dk>.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#

import dbus
import sys
import time
import os

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)
manager = dbus.Interface(bus.get_object('org.freedesktop.Hal', \
                                        '/org/freedesktop/Hal/Manager'), \
                         'org.freedesktop.Hal.Manager')
light_sensor_udis = manager.FindDeviceByCapability('light_sensor')
keyboard_backlight_udis = manager.FindDeviceByCapability('keyboard_backlight')

if len(light_sensor_udis) < 1 or len(keyboard_backlight_udis) < 1:
    print 'Light sensors:       ', light_sensor_udis
    print 'Keyboard backlights: ', keyboard_backlight_udis
    print ''
    print 'This program needs at least one light_sensor and one keyboard_backlight.'
    sys.exit(1)

light_sensor_num_sensors = dbus.Interface(bus.get_object('org.freedesktop.Hal', \
                                                         light_sensor_udis[0]), \
                                          'org.freedesktop.Hal.Device').GetProperty('light_sensor.num_sensors')

keyboard_backlight_num_levels = dbus.Interface(bus.get_object('org.freedesktop.Hal',       \
                                                              keyboard_backlight_udis[0]), \
                                             'org.freedesktop.Hal.Device').GetProperty('keyboard_backlight.num_levels')

light_sensor = dbus.Interface(bus.get_object('org.freedesktop.Hal', \
                                             light_sensor_udis[0]), \
                              'org.freedesktop.Hal.Device.LightSensor')

keyboard_backlight = dbus.Interface(bus.get_object('org.freedesktop.Hal',       \
                                                   keyboard_backlight_udis[0]), \
                                    'org.freedesktop.Hal.Device.KeyboardBacklight')


clamp = 0.35

last_val_set = -1;

while 1:
    try:
        levels = light_sensor.GetBrightness()
        sum = 0
        for l in levels:
            sum += l
        val = float(sum / light_sensor_num_sensors) / keyboard_backlight_num_levels
        if (val > clamp):
            val_to_set = 0
        else:
            val_to_set = (clamp - val) / clamp * (keyboard_backlight_num_levels - 1)
            if (val_to_set < 0):
                val_to_set = 0
            elif val_to_set >= keyboard_backlight_num_levels:
                val_to_set = keyboard_backlight_num_levels - 1
            
        if val_to_set != last_val_set:
            last_val_set = val_to_set
            keyboard_backlight.SetBrightness(val_to_set)
            print 'val_to_set is ', val_to_set

        time.sleep(0.1)
    except Exception, exception:
        print exception
