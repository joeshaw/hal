#!/usr/bin/python
 
import gtk
import dbus

def device_added(interface, signal_name, service, path, message):
    [udi] = message.get_args_list ()
    print 'Device %s was added'%udi

def device_removed(interface, signal_name, service, path, message):
    [udi] = message.get_args_list ()
    print 'Device %s was removed'%udi

 
bus = dbus.Bus (dbus.Bus.TYPE_SYSTEM)
hal_service = bus.get_service ('org.freedesktop.Hal')
hal_manager = hal_service.get_object ('/org/freedesktop/Hal/Manager',
				      'org.freedesktop.Hal.Manager')

devices = hal_manager.GetAllDevices ()
for d in devices:
    print 'Found device %s'%d

bus.add_signal_receiver(device_added,
			 'DeviceAdded',
			 'org.freedesktop.Hal.Manager',
			 'org.freedesktop.Hal',
			 '/org/freedesktop/Hal/Manager')
bus.add_signal_receiver(device_removed,
			 'DeviceRemoved',
			 'org.freedesktop.Hal.Manager',
			 'org.freedesktop.Hal',
			 '/org/freedesktop/Hal/Manager')
gtk.main()





