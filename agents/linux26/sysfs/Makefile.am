## Process this file with automake to produce Makefile.in

SUBDIRS = libsysfs

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
        -Ilibsysfs \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = hal-sysfs-agent.hotplug

hal_sysfs_agent_hotplug_SOURCES =                            \
        main.c                    main.h                     \
        hal_usb.c                 hal_usb.h                  \
        hal_pci.c                 hal_pci.h                  \
        hal_ide.c                 hal_ide.h                  \
        hal_scsi.c                hal_scsi.h                 \
        hal_block.c               hal_block.h                \
        hal_net.c                 hal_net.h                  \
        hal_input.c               hal_input.h                \
	hal_monitor.c             hal_monitor.h


#hal_sysfs_agent_hotplug_LDADD = @PACKAGE_LIBS@

hal_sysfs_agent_hotplug_LDADD = libsysfs/libsysfs.la $(top_builddir)/libhal/libhal.la -ldbus-1 -ldbus-glib-1

clean-local :
	rm -f *~
