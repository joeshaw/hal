## Process this file with automake to produce Makefile.in

SUBDIRS = volume_id libhal libhal-storage libhal-policy hald hald-runner tools \
          fdi doc examples po policy

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = hal.pc hal-storage.pc hal-policy.pc

dbusdir = $(DBUS_SYS_DIR)
dist_dbus_DATA = hal.conf


DISTCLEANFILES = hal.pc hal-storage.pc hal-policy.pc intltool-extract intltool-merge intltool-update

EXTRA_DIST = HACKING hal.pc.in hal-storage.pc.in hal-policy.pc.in hal.conf.in intltool-extract.in intltool-merge.in intltool-update.in mkinstalldirs

clean-local :
	rm -f *~
