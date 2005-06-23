
SUBDIRS = probing addons .

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@PACKAGE_CFLAGS@

if HALD_COMPILE_LINUX2
noinst_LTLIBRARIES = libhald_linux2.la
endif

libhald_linux2_la_SOURCES =					\
				osspec.c			\
	osspec_linux.h						\
	hotplug.h		hotplug.c			\
	hotplug_helper.h					\
	coldplug.h		coldplug.c			\
	physdev.h		physdev.c			\
	classdev.h		classdev.c			\
	blockdev.h		blockdev.c			\
	acpi.h			acpi.c				\
	apm.h			apm.c				\
	pmu.h			pmu.c				\
	ids.h			ids.c				\
	pcmcia_utils.h		pcmcia_utils.c			\
	pcmcia_cs.h








