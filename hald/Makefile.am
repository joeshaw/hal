## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

sbin_PROGRAMS = hald

BUILT_SOURCES =		\
	hald_marshal.h	\
	hald_marshal.c

hald_SOURCES =                                                          \
	hald_marshal.h			hald_marshal.c			\
	callout.h			callout.c			\
	device.h			device.c			\
	device_info.h			device_info.c			\
	device_store.h			device_store.c			\
        hald.h				hald.c				\
	hald_dbus.h			hald_dbus.c			\
	logger.h			logger.c			\
	osspec.h							\
	property.h			property.c

hald_SOURCES +=	\
	linux/common.h			linux/common.c			\
					linux/osspec.c			\
	linux/bus_device.h		linux/bus_device.c		\
	linux/class_device.h		linux/class_device.c		\
					linux/pci_bus_device.c		\
					linux/usb_bus_device.c		\
					linux/usbif_bus_device.c	\
					linux/input_class_device.c	\
					linux/scsi_host_class_device.c	\
					linux/scsi_device_class_device.c \
					linux/block_class_device.c	\
					linux/ide_host_bus_device.c	\
					linux/ide_bus_device.c		\
	linux/linux_dvd_rw_utils.h	linux/linux_dvd_rw_utils.c


hald_SOURCES +=                                                         \
        linux/libsysfs/libsysfs.h                                       \
        linux/libsysfs/dlist.h          linux/libsysfs/dlist.c          \
        linux/libsysfs/sysfs.h                                          \
                                        linux/libsysfs/sysfs_bus.c      \
                                        linux/libsysfs/sysfs_class.c    \
                                        linux/libsysfs/sysfs_device.c   \
                                        linux/libsysfs/sysfs_dir.c      \
                                        linux/libsysfs/sysfs_driver.c   \
                                        linux/libsysfs/sysfs_utils.c

hald_LDADD = @PACKAGE_LIBS@

#### Init scripts fun
SCRIPT_IN_FILES=haldaemon.in

## Red Hat start
if INIT_SCRIPTS_RED_HAT

initddir=$(sysconfdir)/rc.d/init.d

initd_SCRIPTS= 	\
	haldaemon

endif
 ## Red Hat end

EXTRA_DIST =			\
	hald_marshal.list

hald_marshal.h: hald_marshal.list
	cd $(srcdir) \
	&& glib-genmarshal --prefix=hald_marshal hald_marshal.list --header >> xgen-gmh \
	&& (cmp -s xgen-gmh hald_marshal.h || cp xgen-gmh hald_marshal.h) \
	&& rm -f xgen-gmh xgen-gmh~

hald_marshal.c: hald_marshal.list
	cd $(srcdir) \
	&& glib-genmarshal --prefix=hald_marshal hald_marshal.list --body >> xgen-gmc \
	&& (cmp -s xgen-gmc hald_marshal.c || cp xgen-gmc hald_marshal.c) \
	&& rm -f xgen-gmc xgen-gmc~

clean-local:
	rm -f *~
	rm -f hald_marshal.c hald_marshal.h
