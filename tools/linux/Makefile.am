## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@PACKAGE_CFLAGS@

libexec_PROGRAMS = hal.hotplug

hal_hotplug_SOURCES = hal_hotplug.c

hal_hotplug_LDADD = -ldbus-1

clean-local :
	rm -f *~

install-data-local :
	$(mkinstalldirs) $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default
	- /bin/ln -s $(DESTDIR)$(libexecdir)/hal.hotplug $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default

uninstall-local :
	- rm -f $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default/hal.hotplug
