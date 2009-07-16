AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@DBUS_CFLAGS@

if HALD_COMPILE_FREEBSD
noinst_LTLIBRARIES = libhald_freebsd_probe.la
endif

libhald_freebsd_probe_la_SOURCES = \
	hfp.c		\
	hfp.h		\
	hfp-cdrom.c	\
	hfp-cdrom.h
libhald_freebsd_probe_la_LDFLAGS = $(top_builddir)/libhal/libhal.la -lcam
