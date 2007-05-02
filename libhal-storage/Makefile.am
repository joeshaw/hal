## Process this file with automake to produce Makefile.in

AM_CPPFLAGS = -I$(top_srcdir)/libhal \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	@DBUS_CFLAGS@

lib_LTLIBRARIES=libhal-storage.la

libhal_storageincludedir=$(includedir)/hal

libhal_storageinclude_HEADERS =                                   \
	libhal-storage.h

libhal_storage_la_SOURCES =                                       \
	libhal-storage.c                  libhal-storage.h


libhal_storage_la_LIBADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

libhal_storage_la_LDFLAGS = -version-info $(LT_CURRENT):$(LT_REVISION):$(LT_AGE)


clean-local :
	rm -f *~
