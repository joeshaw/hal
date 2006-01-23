
INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

if HALD_COMPILE_LINUX2
libexec_PROGRAMS = hald-probe-input hald-probe-hiddev hald-probe-storage hald-probe-volume hald-probe-printer \
	           hald-probe-pc-floppy hald-probe-smbios hald-probe-serial
endif

hald_probe_smbios_SOURCES = probe-smbios.c shared.h
hald_probe_smbios_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_printer_SOURCES = probe-printer.c shared.h
hald_probe_printer_LDADD = $(top_builddir)/libhal/libhal.la @PACKAGE_LIBS@
#TODO : get rid of glib in hald_probe_printer

hald_probe_input_SOURCES = probe-input.c shared.h
hald_probe_input_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_hiddev_SOURCES = probe-hiddev.c shared.h
hald_probe_hiddev_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_serial_SOURCES = probe-serial.c shared.h
hald_probe_serial_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_storage_SOURCES = probe-storage.c linux_dvd_rw_utils.c linux_dvd_rw_utils.h shared.h
hald_probe_storage_LDADD = $(top_builddir)/libhal/libhal.la $(top_builddir)/volume_id/libvolume_id.la @PACKAGE_LIBS@

hald_probe_pc_floppy_SOURCES = probe-pc-floppy.c

hald_probe_volume_SOURCES = probe-volume.c linux_dvd_rw_utils.c linux_dvd_rw_utils.h shared.h 
hald_probe_volume_LDADD = $(top_builddir)/libhal/libhal.la $(top_builddir)/volume_id/libvolume_id.la @PACKAGE_LIBS@

