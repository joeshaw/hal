## Process this file with automake to produce Makefile.in

INCLUDES = \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	@PACKAGE_CFLAGS@

bin_PROGRAMS = hald

hald_SOURCES =                                           \
        main.c                                           \
                                  logger.h               \
        device_store.c            device_store.h         \
        device_info.c             device_info.h         

hald_LDADD = @PACKAGE_LIBS@

clean-local :
	rm -f *~
