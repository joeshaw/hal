
INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
        -I$(top_srcdir) \
        @GLIB_CFLAGS@ @DBUS_CFLAGS@

if HALD_COMPILE_LINUX
libexec_PROGRAMS  = 			\
	hald-addon-acpi 		\
	hald-addon-acpi-buttons-toshiba \
	hald-addon-hid-ups 		\
	hald-addon-keyboard 		\
	hald-addon-pmu			\
	hald-addon-storage		\
	hald-addon-cpufreq

if HAVE_LIBPCI
libexec_PROGRAMS += hald-addon-macbookpro-backlight
endif
if HAVE_LIBUSB
libexec_PROGRAMS += hald-addon-usb-csr
endif
endif

hald_addon_cpufreq_SOURCES = addon-cpufreq.c addon-cpufreq.h addon-cpufreq-userspace.h \
	                     addon-cpufreq-userspace.c ../../logger.c
hald_addon_cpufreq_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@ @POLKIT_LIBS@

hald_addon_acpi_SOURCES = addon-acpi.c ../../logger.c ../../util_helper.c
hald_addon_acpi_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_acpi_buttons_toshiba_SOURCES = addon-acpi-buttons-toshiba.c ../../logger.c ../../util_helper.c
hald_addon_acpi_buttons_toshiba_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@

hald_addon_hid_ups_SOURCES = addon-hid-ups.c ../../logger.c ../../util_helper.c
hald_addon_hid_ups_LDADD = $(top_builddir)/libhal/libhal.la 

hald_addon_keyboard_SOURCES = addon-keyboard.c ../../logger.c ../../util_helper.c
hald_addon_keyboard_LDADD = $(top_builddir)/libhal/libhal.la 

if HAVE_LIBPCI
hald_addon_macbookpro_backlight_SOURCES = addon-macbookpro-backlight.c ../../logger.c
hald_addon_macbookpro_backlight_LDADD = $(top_builddir)/libhal/libhal.la -lpci @GLIB_LIBS@
endif

hald_addon_pmu_SOURCES = addon-pmu.c ../../logger.c ../../util_helper.c
hald_addon_pmu_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_storage_SOURCES = addon-storage.c ../../logger.c ../../util_helper.c
hald_addon_storage_LDADD = $(top_builddir)/libhal/libhal.la

if HAVE_LIBUSB
hald_addon_usb_csr_SOURCES = addon-usb-csr.c ../../logger.c ../../util_helper.c
hald_addon_usb_csr_LDADD = $(top_builddir)/libhal/libhal.la -lusb @GLIB_LIBS@
endif
