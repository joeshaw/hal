## Process this file with automake to produce Makefile.in

SUBDIRS = $(HALD_BACKEND) .

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

check_PROGRAMS = hald-test

hald_test_SOURCES =                                                     \
	hald_marshal.h			hald_marshal.c			\
	callout.h			callout.c			\
	device.h			device.c			\
	device_info.h			device_info.c			\
	device_store.h			device_store.c			\
	logger.h			logger.c			\
	property.h			property.c                      \
	hald_conf.h                     hald_conf.c			\
	hald_dbus.h			hald_dbus.c			\
	hald_test.c							\
	hald_test_libhal.c

hald_test_LDADD = @PACKAGE_LIBS@ -lm @EXPAT_LIB@ $(top_builddir)/libhal/libhal.la

TESTS = hald-test


sbin_PROGRAMS = hald

BUILT_SOURCES =		\
	hald_marshal.h	\
	hald_marshal.c

hald_SOURCES =                                                          \
	hald_marshal.h			hald_marshal.c			\
	util.h				util.c				\
	device.h			device.c			\
	device_info.h			device_info.c			\
	device_store.h			device_store.c			\
        hald.h				hald.c				\
	hald_dbus.h			hald_dbus.c			\
	logger.h			logger.c			\
	osspec.h							\
	property.h			property.c                      \
	hald_conf.h                     hald_conf.c

hald_LDADD = @PACKAGE_LIBS@ -lm -lcap @EXPAT_LIB@ $(top_builddir)/hald/$(HALD_BACKEND)/libhald_$(HALD_BACKEND).la

if HAVE_SELINUX
hald_LDADD += -lselinux
endif

#### Init scripts fun
SCRIPT_IN_FILES=haldaemon.in

## Red Hat start
if INIT_SCRIPTS_RED_HAT

initddir=$(sysconfdir)/rc.d/init.d

initd_SCRIPTS= 	\
	haldaemon

endif
 ## Red Hat end

EXTRA_DIST =			\
	hald_marshal.list

hald_marshal.h: hald_marshal.list
	cd $(srcdir) \
	&& glib-genmarshal --prefix=hald_marshal hald_marshal.list --header >> xgen-gmh \
	&& (cmp -s xgen-gmh hald_marshal.h || cp xgen-gmh hald_marshal.h) \
	&& rm -f xgen-gmh xgen-gmh~

hald_marshal.c: hald_marshal.list
	cd $(srcdir) \
	&& glib-genmarshal --prefix=hald_marshal hald_marshal.list --body >> xgen-gmc \
	&& (cmp -s xgen-gmc hald_marshal.c || cp xgen-gmc hald_marshal.c) \
	&& rm -f xgen-gmc xgen-gmc~

clean-local:
	rm -f *~
	rm -f hald_marshal.c hald_marshal.h

confdir = $(sysconfdir)/hal

dist_conf_DATA = hald.conf

install-data-local :
	$(mkinstalldirs) $(DESTDIR)$(SYSCONFDIR)/hal/device.d
	$(mkinstalldirs) $(DESTDIR)$(SYSCONFDIR)/hal/capability.d
	$(mkinstalldirs) $(DESTDIR)$(SYSCONFDIR)/hal/property.d
	$(mkinstalldirs) $(DESTDIR)$(LOCALSTATEDIR)/lib/hal

