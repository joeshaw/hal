
devicemanagerbindir = $(bindir)

dist_devicemanagerbin_SCRIPTS = hal-device-manager

devicemanagerdir = $(datadir)/hal/device-manager

nodist_devicemanager_PYTHON =                      \
                       Const.py

devicemanager_PYTHON =                             \
                       DeviceManager.py            \
                       Device.py                   \
                       GtkAttributesFacade.py      \
                       LibGladeApplication.py      \
                       Representation.py

dist_devicemanager_DATA =                                          \
                       fdo-logo.png          hal-cat-mouse.png     \
                       hal-abstract.png      hal-cdrom.png         \
                       hal-audio.png         hal-flash.png         \
                       hal-bus-pci.png       hal-floppy.png        \
                       hal-bus-usb.png       hal-harddisk.png      \
                       hal-camera.png        hal-network.png       \
                       hal-cat-cardbus.png   hal-unknown.png       \
                       hal-cat-keyboard.png  hal-video.png         \
                       hal-computer.png      hal-serial-port.png   \
                       hal-device-manager.glade

# symlink into $(bindir) for convenience
#install-data-local :
#	$(mkinstalldirs) $(DESTDIR)$(bindir)
#	- cd $(DESTDIR)$(bindir) && $(CP) $(pkgdatadir)/device-manager/hal-device-manager hal-device-manager
#
#uninstall-local :
#	- rm -f $(DESTDIR)$(bindir)/hal-device-manager

clean-local :
	rm -f *~
	rm -f hal-device-manager.glade.h
