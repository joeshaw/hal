## Process this file with automake to produce Makefile.in

SUBDIRS =  linux device-manager

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = lshal hal-get-property hal-set-property
sbin_PROGRAMS = fstab-sync

lshal_SOURCES = lshal.c
lshal_LDADD = @PACKAGE_LIBS@ \
              $(top_builddir)/libhal/libhal.la

hal_get_property_SOURCES = hal_get_property.c
hal_get_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_set_property_SOURCES = hal_set_property.c
hal_set_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

fstab_sync_SOURCES = fstab-sync.c
fstab_sync_LDADD = -lpopt $(top_builddir)/libhal/libhal.la

clean-local :
	rm -f *~
