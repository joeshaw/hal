## Process this file with automake to produce Makefile.in

SUBDIRS = libhal libhal-storage hald tools fdi doc examples po

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = hal.pc hal-storage.pc

dbusdir = $(DBUS_SYS_DIR)
dist_dbus_DATA = hal.conf


DISTCLEANFILES = hal.pc hal-storage.pc

EXTRA_DIST = HACKING hal.pc.in hal-storage.pc.in hal.conf.in

clean-local :
	rm -f *~
