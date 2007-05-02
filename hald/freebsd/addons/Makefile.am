AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@DBUS_CFLAGS@

if HALD_COMPILE_FREEBSD
libexec_PROGRAMS = hald-addon-storage
endif

hald_addon_storage_SOURCES = addon-storage.c
hald_addon_storage_LDADD = \
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la
