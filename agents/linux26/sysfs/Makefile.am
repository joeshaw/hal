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
        hal-sysfs-agent.c 

#hal_sysfs_agent_hotplug_LDADD = @PACKAGE_LIBS@

hal_sysfs_agent_hotplug_LDADD = libsysfs/libsysfs.la $(top_builddir)/libhal/libhal.la -ldbus-1 -ldbus-glib-1

clean-local :
	rm -f *~
