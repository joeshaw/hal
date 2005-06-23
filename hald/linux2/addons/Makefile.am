
INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

if HALD_COMPILE_LINUX2
libexec_PROGRAMS  = hald-addon-hid-ups hald-addon-acpi hald-addon-storage hald-addon-pmu

if HAVE_LIBUSB
libexec_PROGRAMS += hald-addon-usb-csr
endif
endif

hald_addon_hid_ups_SOURCES = addon-hid-ups.c
hald_addon_hid_ups_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_acpi_SOURCES = addon-acpi.c
hald_addon_acpi_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_pmu_SOURCES = addon-pmu.c
hald_addon_pmu_LDADD = $(top_builddir)/libhal/libhal.la

hald_addon_storage_SOURCES = addon-storage.c
hald_addon_storage_LDADD = $(top_builddir)/libhal/libhal.la

if HAVE_LIBUSB
hald_addon_usb_csr_SOURCES = addon-usb-csr.c
hald_addon_usb_csr_LDADD = $(top_builddir)/libhal/libhal.la -lusb @PACKAGE_LIBS@
endif
