AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@DBUS_CFLAGS@

if HALD_COMPILE_FREEBSD
libexec_PROGRAMS = hald-addon-storage \
		   hald-addon-mouse-sysmouse
endif

hald_addon_storage_SOURCES = addon-storage.c
hald_addon_storage_LDADD = \
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la

hald_addon_mouse_sysmouse_SOURCES = addon-mouse.c
hald_addon_mouse_sysmouse_CFLAGS = $(AM_CPPFLAGS) @GLIB_CFLAGS@
hald_addon_mouse_sysmouse_LDADD = \
	@GLIB_LIBS@		  \
	-lutil			  \
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la
