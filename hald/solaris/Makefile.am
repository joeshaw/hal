
SUBDIRS = probing addons .

AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@ @POLKIT_CFLAGS@

if HALD_COMPILE_SOLARIS
noinst_LTLIBRARIES = libhald_solaris.la
endif

libhald_solaris_la_SOURCES = \
	devinfo.c devinfo_ieee1394.c devinfo_misc.c devinfo_pci.c devinfo_storage.c devinfo_usb.c \
	hotplug.c osspec.c sysevent.c

