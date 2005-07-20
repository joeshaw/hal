## Process this file with automake to produce Makefile.in

SUBDIRS =  linux device-manager

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-I$(top_srcdir) -I$(top_srcdir)/libhal \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = lshal hal-get-property hal-set-property hal-find-by-capability hal-find-by-property hal-device

if FSTAB_SYNC_ENABLED
sbin_PROGRAMS = fstab-sync
endif

lshal_SOURCES = lshal.c
lshal_LDADD = @PACKAGE_LIBS@ $(top_builddir)/libhal/libhal.la

hal_get_property_SOURCES = hal_get_property.c
hal_get_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_set_property_SOURCES = hal_set_property.c
hal_set_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_find_by_capability_SOURCES = hal_find_by_capability.c
hal_find_by_capability_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_find_by_property_SOURCES = hal_find_by_property.c
hal_find_by_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_device_SOURCES = hal-device.c
hal_device_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

if FSTAB_SYNC_ENABLED
fstab_sync_SOURCES = fstab-sync.c
fstab_sync_LDADD = -lpopt $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

MAN_IN_FILES = fstab-sync.8.in
man_MANS = fstab-sync.8

endif
## FSTAB_SYNC_ENABLED end

sbin_SCRIPTS = hal-luks-setup

EXTRA_DIST=$(man_MANS) $(MAN_IN_FILES) gen-libgphoto-hal-fdi

clean-local :
	rm -f *~
