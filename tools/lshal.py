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
        for p in properties:
            val = properties[p]
            ptype = type(val)
            if ptype==str:
                print "  %s = '%s'  (string)"%(p, val)
            elif ptype==int:
                print "  %s = %d = 0x%x  (int)"%(p, val, val)
            elif ptype==bool:
                if val:
                    print "  %s = true   (bool)"%p
                    print "  %s = false   (bool)"%p
            elif ptype==float:
                print "  %s = %g   (double)"%(p, val)
        print ""
    print ""
    print "Dumped %d devices from the GDL"%(len(device_names))
    print "==========================================="
    print ""

def gdl_changed(dbus_if, dbus_member, dbus_svc, dbus_obj_path, dbus_message):
    if dbus_member=="DeviceAdded" or dbus_member=="DeviceRemoved":
        [device_udi] = dbus_message.get_args_list()
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

    print_devices();
    gtk.main()


if __name__ == '__main__':
    main()

