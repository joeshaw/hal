#!/usr/bin/env python

import dbus
import gtk

def print_devices():
    device_names = hal_manager.GetAllDevices()
    print ""
    print "==========================================="
    print "Dumping %d devices from the GDL"%(len(device_names))
    print ""
    for name in device_names:
        device = hal_service.get_object(name, "org.freedesktop.Hal.Device")
        print "device_unique_id = %s"%name
        properties = device.GetAllProperties()
        keys = properties.keys()
        keys.sort()
        for p in keys:
            val = properties[p]
            ptype = type(val)
            if ptype==str:
                print "  %s = '%s'  (string)"%(p, val)
            elif ptype==int:
                print "  %s = %d = 0x%x  (int)"%(p, val, val)
            elif ptype==bool:
                if val:
                    print "  %s = true   (bool)"%p
                else:
                    print "  %s = false   (bool)"%p
            elif ptype==float:
                print "  %s = %g   (double)"%(p, val)
        print ""
    print ""
    print "Dumped %d devices from the GDL"%(len(device_names))
    print "==========================================="
    print ""

def device_changed(dbus_if,dbus_member, dbus_svc, dbus_obj_path, dbus_message):
    [property_name] = dbus_message.get_args_list()
    print "%s %s property %s"%(dbus_member, dbus_obj_path, property_name)

def gdl_changed(dbus_if, dbus_member, dbus_svc, dbus_obj_path, dbus_message):
    if dbus_member=="DeviceAdded":
        [device_udi] = dbus_message.get_args_list()
        bus.add_signal_receiver(device_changed,
                                "org.freedesktop.Hal.Device",
                                "org.freedesktop.Hal",
                                device_udi)
        print_devices()
    elif dbus_member=="DeviceRemoved":
        [device_udi] = dbus_message.get_args_list()
        bus.remove_signal_receiver(device_changed,
                                   "org.freedesktop.Hal.Device",
                                   "org.freedesktop.Hal",
                                   device_udi)
        print_devices()
    else:
        print "*** Unknown signal %s"%dbus_member

def main():
    global bus, hal_service, hal_manager

    bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)

    hal_service = bus.get_service("org.freedesktop.Hal")
    hal_manager = hal_service.get_object("/org/freedesktop/Hal/Manager",
                                         "org.freedesktop.Hal.Manager")

    # gdl_changed will be invoked when the Global Device List is changed
    # per the hal spec
    bus.add_signal_receiver(gdl_changed,
                            "org.freedesktop.Hal.Manager",
                            "org.freedesktop.Hal",
                            "/org/freedesktop/Hal/Manager")

    # Add listeners for all devices
    device_names = hal_manager.GetAllDevices()
    for name in device_names:
        bus.add_signal_receiver(device_changed,
                                "org.freedesktop.Hal.Device",
                                "org.freedesktop.Hal",
                                name)


    print_devices();
    gtk.main()


if __name__ == '__main__':
    main()

