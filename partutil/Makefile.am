if HALD_COMPILE_SOLARIS 
# do nothing
else if HALD_COMPILE_FREEBSD
# do nothing
else 

INCLUDES = @GLIB_CFLAGS@

noinst_LTLIBRARIES = libpartutil.la

libpartutil_la_SOURCES = partutil.h partutil.c ../hald/logger.c

libpartutil_la_LIBADD = @GLIB_LIBS@ @PARTED_LIBS@

clean-local :
	rm -f *~
endif
