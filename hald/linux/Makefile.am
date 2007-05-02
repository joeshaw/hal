
SUBDIRS = probing addons .

AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@ @POLKIT_CFLAGS@

if HALD_COMPILE_LINUX
noinst_LTLIBRARIES = libhald_linux.la
endif

libhald_linux_la_SOURCES =				\
	acpi.h						\
	apm.h						\
	pmu.h						\
				osspec.c		\
	osspec_linux.h					\
	hotplug.h		hotplug.c		\
	hotplug_helper.h				\
	coldplug.h		coldplug.c		\
	device.h		device.c		\
	blockdev.h		blockdev.c		\
	inotify_local.h					\
				hal-file-monitor.c

if HAVE_ACPI
libhald_linux_la_SOURCES +=				\
	acpi.c
endif

if HAVE_APM
libhald_linux_la_SOURCES +=				\
	apm.c
endif

if HAVE_PMU
libhald_linux_la_SOURCES +=				\
	pmu.c
endif
