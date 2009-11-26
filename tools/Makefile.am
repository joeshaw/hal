## Process this file with automake to produce Makefile.in

SUBDIRS =

if HALD_COMPILE_LINUX
SUBDIRS += linux
endif
if HALD_COMPILE_FREEBSD
SUBDIRS += freebsd
endif

DIST_SUBDIRS = freebsd linux

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

bin_PROGRAMS =                    \
	lshal                     \
	hal-get-property          \
	hal-set-property          \
	hal-find-by-capability    \
	hal-find-by-property      \
	hal-device		  \
	hal-is-caller-locked-out  \
	hal-lock		  \
	hal-disable-polling

BUILT_SOURCES = 
CLEANFILES = 

if HAVE_POLKIT
bin_PROGRAMS += hal-is-caller-privileged
endif

if BUILD_KEYMAPS
bin_PROGRAMS += hal-setup-keymap
endif

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

hal_is_caller_locked_out_SOURCES = hal-is-caller-locked-out.c
hal_is_caller_locked_out_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_lock_SOURCES = hal-lock.c
hal_lock_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

hal_disable_polling_SOURCES = hal-disable-polling.c
hal_disable_polling_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

if HAVE_POLKIT
hal_is_caller_privileged_SOURCES = hal-is-caller-privileged.c
hal_is_caller_privileged_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la
endif

if BUILD_KEYMAPS

if HAVE_GPERF
hal-setup-keymap-keys.txt: @LINUX_INPUT_H@
	awk '/^#define.*KEY_/ { if ($$2 != "KEY_MAX") { print substr($$2, 5) } }' < $< > $@

hal-setup-keymap-hash-name.gperf: hal-setup-keymap-keys.txt
	awk 'BEGIN{ print "struct key { const char* name; unsigned short id; };"; print "%null-strings"; print "%%";} { print $$1 ", KEY_" $$1 }' < $< > $@

hal-setup-keymap-hash-name.h: hal-setup-keymap-hash-name.gperf
	gperf -t --ignore-case -N lookup_key -H hash_input_names -p -C < $< > $@
endif #HAVE_GPERF

hal_setup_keymap_SOURCES = hal-setup-keymap.c
nodist_hal_setup_keymap_SOURCES = hal-setup-keymap-hash-name.h
hal_setup_keymap_LDADD = $(top_builddir)/libhal/libhal.la @DBUS_LIBS@

endif #BUILD_KEYMAPS

libexec_PROGRAMS =                          \
	hal-storage-mount	            \
	hal-storage-unmount 	            \
	hal-storage-eject	            \
	hal-storage-closetray	            \
	hal-storage-cleanup-mountpoint      \
	hal-storage-cleanup-all-mountpoints \
	hal-system-setserial		    \
	hal-system-power-pm-is-supported

if HAVE_PMU
libexec_PROGRAMS +=			    \
	hal-system-power-pmu
endif

if HAVE_SONYPIC
libexec_PROGRAMS +=		            \
	hal-system-sonypic
endif

if HAVE_ACLMGMT
libexec_PROGRAMS += hal-acl-tool

hal_acl_tool_SOURCES = hal-acl-tool.c
hal_acl_tool_LDADD = @GLIB_LIBS@ @POLKIT_LIBS@ $(top_builddir)/libhal/libhal.la
endif

hal_system_power_pm_is_supported_SOURCES = hal-system-power-pm-is-supported.c
hal_system_power_pm_is_supported_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la

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

hal_system_setserial_SOURCES = hal-system-setserial.c
hal_system_setserial_LDADD = 

if HAVE_PMU
hal_system_power_pmu_SOURCES = hal-system-power-pmu.c
hal_system_power_pmu_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la
endif

if HAVE_SONYPIC
hal_system_sonypic_SOURCES = hal-system-sonypic.c
hal_system_sonypic_LDADD = @GLIB_LIBS@ @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la
endif

scriptdir = $(libexecdir)/scripts

script_SCRIPTS =				\
	hal-luks-setup                          \
	hal-luks-teardown                       \
	hal-luks-remove		                \
	hal-system-power-suspend		\
	hal-system-power-suspend-hybrid		\
	hal-system-power-hibernate		\
	hal-system-power-shutdown		\
	hal-system-power-reboot			\
	hal-system-lcd-get-brightness		\
	hal-system-lcd-set-brightness		\
	hal-system-power-set-power-save		\
	hal-system-killswitch-get-power		\
	hal-system-killswitch-set-power		\
	hal-system-wol-supported		\
	hal-system-wol-enabled			\
	hal-system-wol-enable			\
	hal-dockstation-undock			\
	hal-functions

EXTRA_DIST=$(man_MANS) $(MAN_IN_FILES) gen-libgphoto-hal-fdi $(script_SCRIPTS)

if HAVE_UMOUNT_HAL
slashsbindir = /sbin
slashsbin_PROGRAMS = umount.hal

umount_hal_SOURCES = umount-hal.c
umount_hal_LDADD = @DBUS_LIBS@ $(top_builddir)/libhal/libhal.la $(top_builddir)/libhal-storage/libhal-storage.la
endif

if BUILD_KEYMAPS

if HAVE_GPERF
CLEANFILES += 				\
	hal-setup-keymap-hash-name.h		\
	hal-setup-keymap-hash-name.gperf	\
	hal-setup-keymap-keys.txt


BUILT_SOURCES += 				\
	hal-setup-keymap-hash-name.h		\
	hal-setup-keymap-hash-name.gperf	\
	hal-setup-keymap-keys.txt
endif

endif

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
