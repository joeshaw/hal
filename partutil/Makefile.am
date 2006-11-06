if !HALD_COMPILE_SOLARIS

INCLUDES = @GLIB_CFLAGS@

noinst_LTLIBRARIES = libpartutil.la

libpartutil_la_SOURCES = partutil.h partutil.c ../hald/logger.c

libpartutil_la_LIBADD = @GLIB_LIBS@ @PARTED_LIBS@

clean-local :
	rm -f *~

endif
