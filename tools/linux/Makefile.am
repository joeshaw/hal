## Process this file with automake to produce Makefile.in

udevrulesdir = $(sysconfdir)/udev/rules.d
udevrules_DATA = 90-hal.rules

scriptdir = $(libdir)/hal/scripts

script_SCRIPTS =					\
	hal-luks-setup-linux                          	\
	hal-luks-teardown-linux                       	\
	hal-luks-remove-linux		                \
	hal-system-power-suspend-linux			\
	hal-system-power-hibernate-linux		\
	hal-system-power-shutdown-linux			\
	hal-system-power-reboot-linux			\
	hal-system-lcd-get-brightness-linux		\
	hal-system-lcd-set-brightness-linux		\
	hal-system-power-set-power-save-linux		\
	hal-system-storage-unmount-linux		\
	hal-system-storage-eject-linux			\
	hal-system-storage-cleanup-mountpoint-linux   	\
	hal-system-storage-cleanup-mountpoints-linux

EXTRA_DIST = 90-hal.rules $(script_SCRIPTS)

