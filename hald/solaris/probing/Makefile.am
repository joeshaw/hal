
AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I$(top_srcdir)/hald -I$(top_srcdir)/libhal -I$(top_srcdir)/libhal-storage \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@

if HALD_COMPILE_SOLARIS
libexec_PROGRAMS = hald-probe-storage hald-probe-volume
endif

hald_probe_storage_SOURCES = probe-storage.c cdutils.c cdutils.h fsutils.c fsutils.h ../../logger.c
hald_probe_storage_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@ -ladm -lefi
hald_probe_storage_CFLAGS = -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

hald_probe_volume_SOURCES = probe-volume.c cdutils.c cdutils.h fsutils.c fsutils.h ../../logger.c 
hald_probe_volume_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@ -lfstyp -lnvpair -ladm -lefi
hald_probe_volume_CFLAGS = -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

