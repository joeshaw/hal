## Process this file with automake to produce Makefile.in
INCLUDES = \
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\" \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-DPACKAGE_SCRIPT_DIR=\""$(datadir)/hal/scripts"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

libexec_PROGRAMS = hald-runner

hald_runner_SOURCES = main.c runner.c runner.h utils.h utils.c
hald_runner_LDADD = @PACKAGE_LIBS@ @DBUS_LIBS@
