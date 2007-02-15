## Process this file with automake to produce Makefile.in


SUBDIRS = device-manager

if HALD_COMPILE_LINUX
SUBDIRS += linux
endif
if HALD_COMPILE_FREEBSD
SUBDIRS += freebsd
endif

DIST_SUBDIRS = device-manager freebsd linux

INCLUDES = \
	-DPACKAGE_LIBEXEC_DIR=\""$(libexecdir)"\" \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-DPACKAGE_SCRIPT_DIR=\""$(libdir)/hal/scripts"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-I$(top_srcdir) -I$(top_srcdir)/libhal -I$(top_srcdir)/libhal-storage \
	@DBUS_CFLAGS@ @GLIB_CFLAGS@ @POLKIT_CFLAGS@

bin_PROGRAMS =                    \
	lshal                     \
	hal-get-property          \
	hal-set-property          \
	hal-find-by-capability    \
	hal-find-by-property      \
	hal-device

lshal_SOURCES = lshal.c
lshal_LDADD = @GLIB_LIBS@ $(top_builddir)/libhal/libhal.la

hal_get_property_SOURCES = hal_get_property.c
hal_get_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_set_property_SOURCES = hal_set_property.c
hal_set_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_find_by_capability_SOURCES = hal_find_by_capability.c
hal_find_by_capability_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_find_by_property_SOURCES = hal_find_by_property.c
hal_find_by_property_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_device_SOURCES = hal-device.c
hal_device_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

libexec_PROGRAMS =                          \
	hal-storage-mount	            \
	hal-storage-unmount 	            \
	hal-storage-eject	            \
	hal-storage-closetray	            \
	hal-storage-cleanup-mountpoint      \
	hal-storage-cleanup-all-mountpoints

if HAVE_PMU
libexec_PROGRAMS +=			    \
	hal-system-power-pmu
endif

if HAVE_ACLMGMT
libexec_PROGRAMS += hal-acl-add hal-acl-remove

hal_acl_add_SOURCES = hal-acl-add.c
hal_acl_remove_SOURCES = hal-acl-remove.c
endif



hal_storage_mount_SOURCES = hal-storage-mount.c hal-storage-shared.c hal-storage-shared.h
hal_storage_mount_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

hal_storage_unmount_SOURCES = hal-storage-unmount.c hal-storage-shared.c hal-storage-shared.h
hal_storage_unmount_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

hal_storage_eject_SOURCES = hal-storage-eject.c hal-storage-shared.c hal-storage-shared.h
hal_storage_eject_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

hal_storage_closetray_SOURCES = hal-storage-closetray.c hal-storage-shared.c hal-storage-shared.h
hal_storage_closetray_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

hal_storage_cleanup_mountpoint_SOURCES = hal-storage-cleanup-mountpoint.c hal-storage-shared.c hal-storage-shared.h
hal_storage_cleanup_mountpoint_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

hal_storage_cleanup_all_mountpoints_SOURCES = hal-storage-cleanup-all-mountpoints.c hal-storage-shared.c hal-storage-shared.h
hal_storage_cleanup_all_mountpoints_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la

if HAVE_PMU
hal_system_power_pmu_SOURCES = hal-system-power-pmu.c
hal_system_power_pmu_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la
endif

scriptdir = $(libdir)/hal/scripts

script_SCRIPTS =				\
	hal-luks-setup                          \
	hal-luks-teardown                       \
	hal-luks-remove		                \
	hal-system-power-suspend		\
	hal-system-power-hibernate		\
	hal-system-power-shutdown		\
	hal-system-power-reboot			\
	hal-system-lcd-get-brightness		\
	hal-system-lcd-set-brightness		\
	hal-system-power-set-power-save

EXTRA_DIST=$(man_MANS) $(MAN_IN_FILES) gen-libgphoto-hal-fdi $(script_SCRIPTS)

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
