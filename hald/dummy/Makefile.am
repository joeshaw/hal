
AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@ @POLKIT_CFLAGS@

noinst_LTLIBRARIES = libhald_dummy.la
libhald_dummy_la_SOURCES =                                      \
				osspec.c

