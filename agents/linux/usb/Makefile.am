## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = hal-agent.usb

hal_agent_usb_SOURCES =                                           \
        usb_agent.c

hal_agent_usb_LDADD = @PACKAGE_LIBS@ $(top_builddir)/libhal/libhal.la

clean-local :
	rm -f *~
