if HALD_COMPILE_LINUX
noinst_LTLIBRARIES = libpartutil.la
endif

AM_CPPFLAGS = @GLIB_CFLAGS@

libpartutil_la_SOURCES = partutil.h partutil.c ../hald/logger.c

libpartutil_la_LIBADD = @GLIB_LIBS@ @PARTED_LIBS@

clean-local :
	rm -f *~
