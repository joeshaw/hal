## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = hal-sysfs-agent.hotplug

hal_sysfs_agent_hotplug_SOURCES =                            \
        hal-sysfs-agent.c 

hal_sysfs_agent_hotplug_LDADD = @PACKAGE_LIBS@ $(top_builddir)/libhal/libhal.la -lsysfs

clean-local :
	rm -f *~
