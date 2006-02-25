## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\" \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-DPACKAGE_SCRIPT_DIR=\""$(datadir)/hal/scripts"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@GLIB_CFLAGS@

lib_LTLIBRARIES=libhal-policy.la

libhal_policyincludedir=$(includedir)/hal

libhal_policyinclude_HEADERS =                            \
	libhal-policy.h

libhal_policy_la_SOURCES =                                \
	libhal-policy.c           libhal-policy.h

libhal_policy_la_LIBADD = @GLIB_LIBS@

libhal_policy_la_LDFLAGS = -version-info $(LT_CURRENT):$(LT_REVISION):$(LT_AGE)


check_PROGRAMS = libhal-policy-test

libhal_policy_test_SOURCES =                             \
	libhal-policy-test.c

libhal_policy_test_LDADD = @GLIB_LIBS@ libhal-policy.la

TESTS = libhal-policy-test


clean-local :
	rm -f *~

