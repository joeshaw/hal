## Process this file with automake to produce Makefile.in

SUBDIRS = libhal hald agents

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = hal.pc

DISTCLEANFILES =                \
	hal.pc

EXTRA_DIST =                    \
	hal.pc.in               \
	hal.conf


all-local: Doxyfile

### install / uninstall of hal.conf into D-BUS system.d

install-data-local :
	$(INSTALL_DATA) hal.conf $(DBUS_SYS_DIR)

uninstall-local :
	rm -f $(DBUS_SYS_DIR)/hal.conf

clean-local :
	rm -f *~
