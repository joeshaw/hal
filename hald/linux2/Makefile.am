
SUBDIRS = probing .

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@PACKAGE_CFLAGS@

noinst_LTLIBRARIES = libhald_linux2.la

libhald_linux2_la_SOURCES =					\
				osspec.c			\
	hotplug.h		hotplug.c			\
	coldplug.h		coldplug.c			\
	physdev.h		physdev.c			\
	classdev.h		classdev.c			\
	blockdev.h		blockdev.c			\
	util.h			util.c				\
	acpi.h			acpi.c				\
	apm.h			apm.c				\
	pmu.h			pmu.c				\
	ids.h			ids.c				\
	pcmcia_utils.h		pcmcia_utils.c







