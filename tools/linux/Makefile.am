## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

libexec_PROGRAMS = hal.hotplug hal.dev hal-hotplug-map

hal_hotplug_SOURCES = hal_hotplug.c

hal_dev_SOURCES = hal_dev.c

hal_hotplug_map_SOURCES = hal_hotplug_map.c
hal_hotplug_map_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

clean-local :
	rm -f *~

install-data-local :
	$(mkinstalldirs) $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default
	- cd $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default && $(LN_S) $(libexecdir)/hal.hotplug hal.hotplug
	$(mkinstalldirs) $(DESTDIR)/etc/dev.d/default
	- cd $(DESTDIR)/etc/dev.d/default && $(LN_S) $(libexecdir)/hal.dev hal.dev
if HOTPLUG_MAP_ENABLED
	$(mkinstalldirs) $(DESTDIR)$(SYSCONFDIR)/hal/device.d
	- cd $(DESTDIR)$(SYSCONFDIR)/hal/device.d && $(LN_S) $(libexecdir)/hal-hotplug-map 40-hal-hotplug-map.hal
endif

uninstall-local :
	- rm -f $(DESTDIR)$(LINUX_HOTPLUG_DIR)/default/hal.hotplug
	- rm -f $(DESTDIR)/etc/dev.d/default/hal.dev
if HOTPLUG_MAP_ENABLED
	- rm -f $(DESTDIR)$(SYSCONFDIR)/hal/device.d/40-hal-hotplug-map.hal
endif


