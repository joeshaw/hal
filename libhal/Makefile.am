## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@DBUS_CFLAGS@

lib_LTLIBRARIES=libhal.la

libhalincludedir=$(includedir)/hal

libhalinclude_HEADERS =                                   \
	libhal.h

libhal_la_SOURCES =                                       \
	libhal.c                  libhal.h


libhal_la_LIBADD = @DBUS_LIBS@ 

clean-local :
	rm -f *~
