## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	-I$(top_srcdir) \
	@PACKAGE_CFLAGS@

sbin_PROGRAMS = hald

hald_SOURCES =                                                          \
        hald.h                                                          \
                                        main.c                          \
        osspec.h                                                        \
        logger.h                        logger.c                        \
        device_store.h                  device_store.c                  \
        device_info.h                   device_info.c         

hald_SOURCES +=                                                         \
                                        linux/linux_osspec.c            \
        linux/linux_common.h            linux/linux_common.c            \
        linux/linux_pci.h               linux/linux_pci.c               \
        linux/linux_i2c.h               linux/linux_i2c.c               \
        linux/linux_usb.h               linux/linux_usb.c               \
        linux/linux_ieee1394.h          linux/linux_ieee1394.c          \
        linux/linux_ide.h               linux/linux_ide.c               \
		linux/linux_dvd_rw_utils.h      linux/linux_dvd_rw_utils.c	    \
        linux/linux_class_block.h       linux/linux_class_block.c       \
        linux/linux_class_scsi.h        linux/linux_class_scsi.c        \
        linux/linux_class_net.h         linux/linux_class_net.c         \
        linux/linux_class_i2c_adapter.h linux/linux_class_i2c_adapter.c \
        linux/linux_class_v4l.h         linux/linux_class_v4l.c         \
        linux/linux_class_input.h       linux/linux_class_input.c

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


clean-local :
	rm -f *~
