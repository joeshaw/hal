
AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@ @BLKID_CFLAGS@

if HALD_COMPILE_LINUX
libexec_PROGRAMS = 			\
	hald-probe-input 		\
	hald-probe-hiddev 		\
	hald-probe-storage 		\
	hald-probe-volume 		\
	hald-probe-printer 		\
	hald-probe-pc-floppy 		\
	hald-probe-smbios 		\
	hald-probe-serial 		\
	hald-probe-ieee1394-unit 	\
	hald-probe-net-bluetooth 	\
	hald-probe-video4linux
endif

hald_probe_smbios_SOURCES = probe-smbios.c ../../logger.c
hald_probe_smbios_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

hald_probe_printer_SOURCES = probe-printer.c ../../logger.c
hald_probe_printer_LDADD = $(top_builddir)/libhal/libhal.la @GLIB_LIBS@
#TODO : get rid of glib in hald_probe_printer

hald_probe_input_SOURCES = probe-input.c ../../logger.c
hald_probe_input_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

hald_probe_hiddev_SOURCES = probe-hiddev.c ../../logger.c
hald_probe_hiddev_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

hald_probe_serial_SOURCES = probe-serial.c ../../logger.c
hald_probe_serial_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_storage_SOURCES = probe-storage.c linux_dvd_rw_utils.c linux_dvd_rw_utils.h ../../util_helper.c ../../logger.c  
hald_probe_storage_LDADD = @GLIB_LIBS@ @BLKID_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/partutil/libpartutil.la 

hald_probe_pc_floppy_SOURCES = probe-pc-floppy.c ../../logger.c

hald_probe_volume_SOURCES = probe-volume.c linux_dvd_rw_utils.c ../../logger.c 
hald_probe_volume_LDADD = $(top_builddir)/libhal/libhal.la $(top_builddir)/partutil/libpartutil.la @GLIB_LIBS@ @BLKID_LIBS@

hald_probe_ieee1394_unit_SOURCES = probe-ieee1394-unit.c ../../logger.c
hald_probe_ieee1394_unit_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

hald_probe_net_bluetooth_SOURCES = probe-net-bluetooth.c ../../logger.c
hald_probe_net_bluetooth_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

hald_probe_video4linux_SOURCES = probe-video4linux.c ../../logger.c
hald_probe_video4linux_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

