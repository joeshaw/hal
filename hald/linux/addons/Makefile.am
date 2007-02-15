
INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
        -I$(top_srcdir) \
        @GLIB_CFLAGS@ @DBUS_CFLAGS@ @POLKIT_CFLAGS@

if HALD_COMPILE_LINUX
libexec_PROGRAMS  = 			\
	hald-addon-hid-ups 		\
	hald-addon-keyboard 		\
	hald-addon-storage

if HAVE_PMU
libexec_PROGRAMS  += 			\
	hald-addon-pmu
endif

if HAVE_ACPI
libexec_PROGRAMS  += 			\
	hald-addon-acpi 		\
	hald-addon-acpi-buttons-toshiba
endif

if BUILD_OMAP
libexec_PROGRAMS  +=			\
	hald-addon-omap-backlight
hald_addon_omap_backlight_SOURCES = addon-omap-backlight.c ../../logger.c ../../util_helper.c
hald_addon_omap_backlight_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@
endif

if BUILD_MACBOOKPRO
libexec_PROGRAMS += hald-addon-macbookpro-backlight
hald_addon_macbookpro_backlight_SOURCES = addon-macbookpro-backlight.c ../../logger.c
hald_addon_macbookpro_backlight_LDADD = $(top_builddir)/libhal/libhal.la -lpci @GLIB_LIBS@
endif

if BUILD_CPUFREQ
libexec_PROGRAMS += hald-addon-cpufreq
hald_addon_cpufreq_SOURCES = addon-cpufreq.c addon-cpufreq.h addon-cpufreq-userspace.h \
	                     addon-cpufreq-userspace.c ../../logger.c
hald_addon_cpufreq_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@ @POLKIT_LIBS@
endif

if BUILD_USBCSR
libexec_PROGRAMS += hald-addon-usb-csr
hald_addon_usb_csr_SOURCES = addon-usb-csr.c ../../logger.c ../../util_helper.c
hald_addon_usb_csr_LDADD = $(top_builddir)/libhal/libhal.la -lusb @GLIB_LIBS@
endif

if BUILD_DELL
libexec_PROGRAMS += hald-addon-dell-backlight
hald_addon_dell_backlight_SOURCES = addon-dell-backlight.cpp ../../logger.c 
hald_addon_dell_backlight_LDADD = $(top_builddir)/libhal/libhal.la -lsmbios @GLIB_LIBS@ 
endif
endif

hald_addon_acpi_SOURCES = addon-acpi.c ../../logger.c ../../util_helper.c
hald_addon_acpi_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_acpi_buttons_toshiba_SOURCES = addon-acpi-buttons-toshiba.c ../../logger.c ../../util_helper.c
hald_addon_acpi_buttons_toshiba_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@

hald_addon_hid_ups_SOURCES = addon-hid-ups.c ../../logger.c ../../util_helper.c ../../util_pm.c
hald_addon_hid_ups_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@

hald_addon_keyboard_SOURCES = addon-keyboard.c ../../logger.c ../../util_helper.c
hald_addon_keyboard_LDADD = $(top_builddir)/libhal/libhal.la 

hald_addon_pmu_SOURCES = addon-pmu.c ../../logger.c ../../util_helper.c
hald_addon_pmu_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_storage_SOURCES = addon-storage.c ../../logger.c ../../util_helper.c
hald_addon_storage_LDADD = $(top_builddir)/libhal/libhal.la
