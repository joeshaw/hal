## Process this file with automake to produce Makefile.in

SUBDIRS =  linux device-manager

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
    -I$(top_builddir) \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = lshal hal_get_property hal_set_property

lshal_SOURCES = lshal.c
lshal_LDADD = -ldbus-1 -ldbus-glib-1 -lglib-2.0 \
              -L$(top_builddir)/libhal -lhal

hal_get_property_SOURCES = hal_get_property.c
hal_get_property_LDADD = -ldbus-1 -L$(top_builddir)/libhal -lhal

hal_set_property_SOURCES = hal_set_property.c
hal_set_property_LDADD = -ldbus-1 -L$(top_builddir)/libhal -lhal

clean-local :
	rm -f *~
