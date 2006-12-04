#!/usr/bin/env python

# Simple program to test locking; will acquire a lock on the hal device
# object representing the computer.
#
# usage: locking.py <numsecs>

import dbus
import sys
import time
import os

dev_udi = "/org/freedesktop/Hal/devices/computer"
duration = int(sys.argv[1])
pid = os.getpid()
reason = "locking.py pid %d"%pid

bus = dbus.Bus(dbus.Bus.TYPE_SYSTEM)

devobj = bus.get_object("org.freedesktop.Hal", dev_udi)
dev = dbus.Interface(devobj, "org.freedesktop.Hal.Device")
#hal_service = bus.get_service("org.freedesktop.Hal")
#dev = hal_service.get_object (dev_udi, "org.freedesktop.Hal.Device")


print "I am %s with pid %d"%(bus.get_connection().get_unique_name(), pid)
print


print "Attempting to lock %s for %d secs"%(dev_udi, duration)
print "with reason '%s'"%reason
print

try:
    dev.Lock(reason)
except Exception, e:
    print "Locking failed"
    reason =   dev.GetProperty("info.locked.reason")
    lock_svc = dev.GetProperty("info.locked.dbus_service")
    print "Reason:                   '%s'"%reason
    print "Locked by D-BUS basesvc:  '%s'"%lock_svc
    sys.exit(1)

print "Lock acquired; sleeping %d seconds"%duration    
time.sleep(duration)
print "Releasing lock"
dev.Unlock()
print "Lock released"
