## Process this file with automake to produce Makefile.in

SUBDIRS = libhal hald tools

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = hal.pc

dbusdir = $(DBUS_SYS_DIR)
dist_dbus_DATA = hal.conf


DISTCLEANFILES = hal.pc

EXTRA_DIST = hal.pc.in

all-local: Doxyfile

clean-local :
	rm -f *~
