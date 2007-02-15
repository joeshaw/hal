## Process this file with automake to produce Makefile.in

SUBDIRS = dummy freebsd linux solaris .

INCLUDES = \
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\" \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-DPACKAGE_SCRIPT_DIR=\""$(libdir)/hal/scripts"\" \
	-I$(top_srcdir) \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@

## check_PROGRAMS = hald-test

check_PROGRAMS = hald-cache-test

#hald_test_SOURCES =                                                     \
#	hald_marshal.h			hald_marshal.c			\
#	device.h			device.c			\
#	device_info.h			device_info.c			\
#	device_store.h			device_store.c			\
#	logger.h			logger.c			\
#	hald_dbus.h			hald_dbus.c			\
#	hald_test.c							\
#	hald_test_libhal.c

# hald_test_LDADD = @PACKAGE_LIBS@ -lm @EXPAT_LIB@ $(top_builddir)/libhal/libhal.la

# TESTS = hald-test
TESTS = hald-cache-test.sh

sbin_PROGRAMS = hald
libexec_PROGRAMS = hald-generate-fdi-cache

BUILT_SOURCES =		\
	hald_marshal.h	\
	hald_marshal.c

hald_generate_fdi_cache_SOURCES = create_cache.c logger.h logger.c rule.h
hald_generate_fdi_cache_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ -lm @EXPAT_LIB@ @HALD_OS_LIBS@ $(top_builddir)/hald/$(HALD_BACKEND)/libhald_$(HALD_BACKEND).la

hald_cache_test_SOURCES = cache_test.c logger.h logger.c rule.h
hald_cache_test_LDADD = @GLIB_LIBS@ -lm @HALD_OS_LIBS@ $(top_builddir)/hald/$(HALD_BACKEND)/libhald_$(HALD_BACKEND).la

hald_SOURCES =                                                          \
	hald_marshal.h			hald_marshal.c			\
	util.h				util.c				\
	util_helper.h			util_helper.c			\
	util_pm.h			util_pm.c			\
	hald_runner.h			hald_runner.c			\
	device.h			device.c			\
	device_info.h			device_info.c			\
	device_store.h			device_store.c			\
        hald.h				hald.c				\
	hald_dbus.h			hald_dbus.c			\
	logger.h			logger.c			\
	osspec.h							\
	ids.h				ids.c				\
	rule.h				mmap_cache.c			\
	mmap_cache.h

if HAVE_CONKIT
hald_SOURCES += ck-tracker.h ck-tracker.c
endif


hald_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ -lm @HALD_OS_LIBS@ $(top_builddir)/hald/$(HALD_BACKEND)/libhald_$(HALD_BACKEND).la

#### Init scripts fun
SCRIPT_IN_FILES=haldaemon.in

## Red Hat start
if OS_TYPE_RED_HAT

initddir=$(sysconfdir)/rc.d/init.d

initd_SCRIPTS= 	\
	haldaemon

endif
 ## Red Hat end

EXTRA_DIST =			\
	hald_marshal.list hald-cache-test.sh

hald_marshal.h: hald_marshal.list
	glib-genmarshal $< --prefix=hald_marshal --header > $@

hald_marshal.c: hald_marshal.list
	echo "#include \"hald_marshal.h\"" > $@ && \
	glib-genmarshal $< --prefix=hald_marshal --body >> $@


install-data-local:
	-$(mkdir_p) $(DESTDIR)$(HALD_SOCKET_DIR)
	-chown $(HAL_USER):$(HAL_GROUP) $(DESTDIR)$(HALD_SOCKET_DIR)
	-chmod 0755 $(DESTDIR)$(HALD_SOCKET_DIR)
	-$(mkdir_p) $(DESTDIR)$(HALD_SOCKET_DIR)/hald-local
	-$(mkdir_p) $(DESTDIR)$(HALD_SOCKET_DIR)/hald-runner
	-$(mkdir_p) $(DESTDIR)$(localstatedir)/cache/hald
	-chown $(HAL_USER):$(HAL_GROUP) $(DESTDIR)$(localstatedir)/cache/hald

clean-local:
	rm -f *~
	rm -f hald_marshal.c hald_marshal.h

