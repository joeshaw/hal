
INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

libexec_PROGRAMS = hald-probe-input hald-probe-hiddev hald-probe-storage hald-probe-volume

hald_probe_input_SOURCES = probe-input.c
hald_probe_input_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_hiddev_SOURCES = probe-hiddev.c
hald_probe_hiddev_LDADD = $(top_builddir)/libhal/libhal.la

hald_probe_storage_SOURCES = probe-storage.c linux_dvd_rw_utils.c linux_dvd_rw_utils.h
hald_probe_storage_LDADD = $(top_builddir)/libhal/libhal.la $(top_builddir)/drive_id/libdrive_id.la $(top_builddir)/volume_id/libvolume_id.la

hald_probe_volume_SOURCES = probe-volume.c linux_dvd_rw_utils.c linux_dvd_rw_utils.h
hald_probe_volume_LDADD = $(top_builddir)/libhal/libhal.la $(top_builddir)/drive_id/libdrive_id.la $(top_builddir)/volume_id/libvolume_id.la





