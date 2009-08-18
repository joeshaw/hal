SUBDIRS = libprobe probing addons .

AM_CPPFLAGS = \
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(localedir)"\" \
	-DPACKAGE_LOCALSTATEDIR=\""$(localstatedir)"\" \
	-I$(top_srcdir) -I.. \
	@GLIB_CFLAGS@ @DBUS_CFLAGS@ @POLKIT_CFLAGS@

if HALD_COMPILE_FREEBSD
noinst_LTLIBRARIES = libhald_freebsd.la
endif

libhald_freebsd_la_SOURCES = 	\
	hf-acpi.c		\
	hf-acpi.h		\
	hf-ata.c		\
	hf-ata.h		\
	hf-block.c		\
	hf-block.h		\
	hf-computer.c		\
	hf-computer.h		\
	hf-devd.c		\
	hf-devd.h		\
	hf-devtree.c		\
	hf-devtree.h		\
	hf-drm.c		\
	hf-drm.h		\
	hf-net.c		\
	hf-net.h		\
	hf-osspec.h		\
	hf-pci.c		\
	hf-pci.h		\
	hf-pcmcia.c		\
	hf-pcmcia.h		\
	hf-scsi.c		\
	hf-scsi.h		\
	hf-serial.c		\
	hf-serial.h		\
	hf-sound.c		\
	hf-sound.h		\
	hf-storage.c		\
	hf-storage.h		\
	hf-usb.c		\
	hf-usb.h		\
	hf-util.c		\
	hf-util.h		\
	hf-volume.c		\
	hf-volume.h		\
	osspec.c		\
	hal-file-monitor.c

libhald_freebsd_la_LDFLAGS = -lcam

if HAVE_LIBUSB20
libhald_freebsd_la_SOURCES += 	\
	hf-usb2.c		\
	hf-usb2.h
libhald_freebsd_la_LDFLAGS += @LIBUSB20_LIBS@
endif

EXTRA_DIST = README TODO
