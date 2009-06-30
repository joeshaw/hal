## Process this file with automake to produce Makefile.in
AM_CPPFLAGS = \
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\" \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-DPACKAGE_SCRIPT_DIR=\""$(libexecdir)/scripts"\" \
	-DHALD_SOCKET_DIR=\""$(HALD_SOCKET_DIR)"\" \
	-I$(top_srcdir) \
	@DBUS_CFLAGS@ @GLIB_CFLAGS@

libexec_PROGRAMS = hald-runner

hald_runner_SOURCES = main.c runner.c runner.h utils.h utils.c
hald_runner_LDADD = @GLIB_LIBS@ @DBUS_LIBS@
