
INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@PACKAGE_CFLAGS@

noinst_LTLIBRARIES = libhald_linux.la
libhald_linux_la_SOURCES =                                                 \
	common.h			common.c			\
					osspec.c			\
	bus_device.h		bus_device.c		\
	class_device.h		class_device.c		\
					pci_bus_device.c		\
					usb_bus_device.c		\
					usbif_bus_device.c	\
					input_class_device.c	\
					net_class_device.c	\
					printer_class_device.c	\
					scsi_bus_device.c           \
					scsi_generic_class_device.c \
					scsi_host_class_device.c	\
					block_class_device.c	\
					ide_host_bus_device.c	\
					ide_bus_device.c		\
					pcmcia_socket_class_device.c \
					macio_bus_device.c	\
					ieee1394_class_device.c	\
					ieee1394_node_class_device.c \
					ieee1394_host_class_device.c \
					serial_class_device.c \
					usb_serial_bus_device.c \
					multimedia_class_device.c \
					platform_bus_device.c	\
	linux_dvd_rw_utils.h	linux_dvd_rw_utils.c \
	hald_helper.h

#					ieee1394_host_class_device.c

if PCMCIA_SUPPORT_ENABLE
libhald_linux_la_SOURCES += pcmcia_utils.c pcmcia_utils.h pcmcia_cs.h
endif

libhald_linux_la_SOURCES +=                                                         \
        libsysfs/libsysfs.h                                       \
        libsysfs/dlist.h          libsysfs/dlist.c          \
        libsysfs/sysfs.h                                          \
                                        libsysfs/sysfs_bus.c      \
                                        libsysfs/sysfs_class.c    \
                                        libsysfs/sysfs_device.c   \
                                        libsysfs/sysfs_dir.c      \
                                        libsysfs/sysfs_driver.c   \
                                        libsysfs/sysfs_utils.c

libhald_linux_la_SOURCES +=                                                         \
	volume_id/volume_id_logging.h				\
	volume_id/volume_id.h	volume_id/volume_id.c	\
	drive_id/drive_id_logging.h				\
	drive_id/drive_id.h	drive_id/drive_id.c
