AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) \
	@DBUS_CFLAGS@

if HALD_COMPILE_FREEBSD
libexec_PROGRAMS = \
	hald-probe-hiddev	\
	hald-probe-scsi		\
	hald-probe-smbios	\
	hald-probe-storage	\
	hald-probe-volume
endif

hald_probe_hiddev_SOURCES = probe-hiddev.c
hald_probe_hiddev_LDADD = \
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la	\
	-lusbhid

hald_probe_smbios_SOURCES = probe-smbios.c
hald_probe_smbios_LDADD = \
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la

hald_probe_scsi_SOURCES = probe-scsi.c
hald_probe_scsi_LDADD = \
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la

hald_probe_storage_SOURCES = freebsd_dvd_rw_utils.c freebsd_dvd_rw_utils.h probe-storage.c
hald_probe_storage_CPPFLAGS = $(AM_CPPFLAGS) @GLIB_CFLAGS@ @VOLUME_ID_CFLAGS@
hald_probe_storage_LDADD = \
	@GLIB_LIBS@							\
	@VOLUME_ID_LIBS@						\
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la

hald_probe_volume_SOURCES = freebsd_dvd_rw_utils.c freebsd_dvd_rw_utils.h probe-volume.c
hald_probe_volume_CPPFLAGS = $(AM_CPPFLAGS) @GLIB_CFLAGS@ @VOLUME_ID_CFLAGS@
hald_probe_volume_LDADD = \
	@GLIB_LIBS@							\
	@VOLUME_ID_LIBS@						\
	$(top_builddir)/hald/freebsd/libprobe/libhald_freebsd_probe.la
