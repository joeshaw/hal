#!/usr/bin/python
 
import gtk
import dbus
import dbus.glib

def device_added(udi):
    print 'Device %s was added'%udi

def device_removed(udi):
    print 'Device %s was removed'%udi

 
bus = dbus.Bus (dbus.Bus.TYPE_SYSTEM)
hal_manager = bus.get_object ('org.freedesktop.Hal',
                              '/org/freedesktop/Hal/Manager')

devices = hal_manager.GetAllDevices ()
for d in devices:
    print 'Found device %s'%d

bus.add_signal_receiver (device_added,
			 'DeviceAdded',
			 'org.freedesktop.Hal.Manager',
			 'org.freedesktop.Hal',
			 '/org/freedesktop/Hal/Manager')
bus.add_signal_receiver (device_removed,
			 'DeviceRemoved',
			 'org.freedesktop.Hal.Manager',
			 'org.freedesktop.Hal',
			 '/org/freedesktop/Hal/Manager')
gtk.main()
