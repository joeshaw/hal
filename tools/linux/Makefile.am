## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

libexec_PROGRAMS = hal.hotplug

hal_hotplug_SOURCES = hal_hotplug.c

clean-local :
	rm -f *~

install-data-local :
	$(mkinstalldirs) $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default
	- cd $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default && $(LN_S) $(libexecdir)/hal.hotplug 20-hal.hotplug

uninstall-local :
	- rm -f $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default/20-hal.hotplug


