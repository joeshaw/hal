#!/usr/bin/python

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
hal_service = bus.get_service("org.freedesktop.Hal")

print "I am %s with pid %d"%(bus.get_connection().get_base_service(), pid)
print

dev = hal_service.get_object (dev_udi, "org.freedesktop.Hal.Device")

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
