## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = hal-usb-agent.hotplug

hal_usb_agent_hotplug_SOURCES =                                \
        usb_agent.c

hal_usb_agent_hotplug_LDADD = @PACKAGE_LIBS@ $(top_builddir)/libhal/libhal.la

clean-local :
	rm -f *~
