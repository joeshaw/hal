#!/usr/bin/python

import dbus
import gtk
import time

# This is just a very very crude example of what a volume daemon could
# look like... It actually doesn't mount anything; it only prints out
# messages when it should mount/unmount stuff.
#
# A volume daemon should also support optical and floppy disks, it should
# handle multi-session cdroms and much more. Maybe someone will write
# this one day... Maybe the disc change stuff should even be in HAL?
#
# This requires udev with D-BUS enabled and HAL to work correctly
#

def get_mount_location(udi, device_name):
    """Given a the UDI for a device and the name of the device file,
    determine a name for the mount point"""
    return '/mnt/somewhere/unique%f'%(time.time())

def attempt_mount(udi):
    """See if a block device has enough information so we can mount it"""
    dobj = hal_service.get_object(udi, 'org.freedesktop.Hal.Device')
    if (not mount_dict.has_key(udi)) and dobj.PropertyExists('block.device'):
        device = dobj.GetProperty('block.device')
        mount_location = get_mount_location(udi, device)
        print "mounting device=%s at %s udi=%s"%(device, mount_location, udi)
        mount_dict[udi] = mount_location

def unmount(udi):
    """Unmount a device"""
    mount_location = mount_dict[udi]
    print "unmounting %s"%mount_location
    del mount_dict[udi]

def device_changed(dbus_if, member, svc, obj_path, message):
    """Called when properties on a HAL device changes"""
    #print member
    udi = obj_path
    if udi in vol_list:
        attempt_mount(udi)

def gdl_changed(dbus_if, member, svc, obj_path, message):
    """Called when a HAL device is added, removed or it got a
    new capability"""
    #print member
    if member=='NewCapability':
        [udi, cap] = message.get_args_list()
        if cap=='volume':
            if not udi in vol_list:
                vol_list.append(udi)
                bus.add_signal_receiver(device_changed,
                                        'org.freedesktop.Hal.Device',
                                        'org.freedesktop.Hal',
                                        udi)
                attempt_mount(udi)
                
        #print "  %s %s"%(cap,udi)

    elif member=='DeviceRemoved':
        [udi] = message.get_args_list()
        if udi in vol_list:
            vol_list.remove(udi)
            bus.remove_signal_receiver(device_changed,
                                       'org.freedesktop.Hal.Device',
                                       'org.freedesktop.Hal',
                                       udi)
            unmount(udi)
            
    elif member=='DeviceAdded':
        [udi] = message.get_args_list()
        dobj = hal_service.get_object(udi, 'org.freedesktop.Hal.Device')
        #if dobj.PropertyExists('Capabilities'):
        #    print '  caps=%s'%(dobj.GetProperty('Capabilities'))
        if dobj.QueryCapability('volume'):
            vol_list.append(udi)
            bus.add_signal_receiver(device_changed,
                                    'org.freedesktop.Hal.Device',
                                    'org.freedesktop.Hal',
                                    udi)
            attempt_mount(udi)

def main():
    """Entry point"""
    global bus, hal_service, vol_list, mount_dict

    vol_list = []
    mount_dict = {}
    
    bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)
    hal_service = bus.get_service('org.freedesktop.Hal')

    bus.add_signal_receiver(gdl_changed,
                            'org.freedesktop.Hal.Manager',
                            'org.freedesktop.Hal',
                            '/org/freedesktop/Hal/Manager')

    gtk.mainloop()

if __name__=='__main__':
    main()
