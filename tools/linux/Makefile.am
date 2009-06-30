## Process this file with automake to produce Makefile.in

udevrulesdir = $(udev_prefix)/udev/rules.d
udevrules_DATA = 90-hal.rules


AM_CPPFLAGS = \
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\" \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-DPACKAGE_SCRIPT_DIR=\""$(libexecdir)/scripts"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-I$(top_srcdir) -I$(top_srcdir)/libhal -I$(top_srcdir)/libhal-storage \
	@DBUS_CFLAGS@ @GLIB_CFLAGS@ @POLKIT_CFLAGS@

libexec_PROGRAMS =  

scriptdir = $(libexecdir)/scripts/linux

script_SCRIPTS =					\
	hal-luks-setup-linux                          	\
	hal-luks-teardown-linux                       	\
	hal-luks-remove-linux		                \
	hal-system-power-suspend-linux			\
	hal-system-power-suspend-hybrid-linux		\
	hal-system-power-hibernate-linux		\
	hal-system-power-shutdown-linux			\
	hal-system-power-reboot-linux			\
	hal-system-lcd-get-brightness-linux		\
	hal-system-lcd-set-brightness-linux		\
	hal-system-power-set-power-save-linux		\
	hal-system-killswitch-get-power-linux		\
	hal-system-killswitch-set-power-linux		\
	hal-dockstation-undock-linux			\
	hal-system-wol-linux

EXTRA_DIST = 90-hal.rules $(script_SCRIPTS)

check:
	for f in $(script_SCRIPTS); do \
            echo -n "Validate bash syntax in $$f : "; \
            bash -n $(srcdir)/$$f 2> bash.error;\
            if test -s bash.error; then \
                echo failed; \
                cat bash.error; \
                rm -f bash.error; \
                exit 1; \
            else \
                echo ok; \
                rm -f bash.error; \
            fi; \
        done;

clean-local :
	rm -f *~

install-data-hook:
	ln -sf hal-system-wol-linux $(DESTDIR)$(scriptdir)/hal-system-wol-enable-linux
	ln -sf hal-system-wol-linux $(DESTDIR)$(scriptdir)/hal-system-wol-enabled-linux
	ln -sf hal-system-wol-linux $(DESTDIR)$(scriptdir)/hal-system-wol-supported-linux

uninstall-hook:
	rm -f $(DESTDIR)$(scriptdir)/hal-system-wol-*
