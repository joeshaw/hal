
SUBDIRS = probing addons .

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@

if HALD_COMPILE_LINUX
noinst_LTLIBRARIES = libhald_linux.la
endif

libhald_linux_la_SOURCES =					\
				osspec.c			\
	osspec_linux.h						\
	hotplug.h		hotplug.c			\
	hotplug_helper.h					\
	coldplug.h		coldplug.c			\
	device.h		device.c			\
	blockdev.h		blockdev.c			\
	acpi.h			acpi.c				\
	apm.h			apm.c				\
	pmu.h			pmu.c

