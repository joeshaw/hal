## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@DBUS_CFLAGS@

libexec_PROGRAMS = hal.hotplug hal.dev

hal_hotplug_SOURCES = hal_hotplug.c

hal_hotplug_LDADD = @DBUS_LIBS@

hal_dev_SOURCES = hal_dev.c

hal_dev_LDADD = @DBUS_LIBS@

clean-local :
	rm -f *~

install-data-local :
	$(mkinstalldirs) $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default
	- cd $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default && $(LN_S) $(libexecdir)/hal.hotplug hal.hotplug
	$(mkinstalldirs) $(DESTDIR)/etc/dev.d/default
	- cd $(DESTDIR)/etc/dev.d/default && $(LN_S) $(libexecdir)/hal.dev hal.dev

uninstall-local :
	- rm -f $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default/hal.hotplug
	- rm -f $(DESTDIR)/etc/dev.d/default/hal.dev
