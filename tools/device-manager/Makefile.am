
if USE_MAINTAINER_MODE
    DATADIR = $(BUILD_DIR)/tools/device-manager
else
    DATADIR = $(datadir)/hal/device-manager
endif

all : Const.py

Const.py : Const.py.in
	sed -e 's#DATADIR_IN#$(DATADIR)#; s/VERSION_IN/@PACKAGE_VERSION@/' Const.py.in > Const.py

devicemanagerdir = $(datadir)/hal/device-manager

devicemanager_SCRIPTS = hal-device-manager

devicemanager_PYTHON =                             \
                       Const.py                    \
                       DeviceManager.py            \
                       Device.py                   \
                       GtkAttributesFacade.py      \
                       LibGladeApplication.py      \
                       Representation.py

devicemanager_DATA =                                               \
                       fdo-logo.png          hal-cat-mouse.png     \
                       hal-abstract.png      hal-cdrom.png         \
                       hal-audio.png         hal-flash.png         \
                       hal-bus-pci.png       hal-floppy.png        \
                       hal-bus-usb.png       hal-harddisk.png      \
                       hal-camera.png        hal-network.png       \
                       hal-cat-cardbus.png   hal-unknown.png       \
                       hal-cat-keyboard.png  hal-video.png         \
                       hal-device-manager.glade

EXTRA_DIST = $(devicemanager_DATA) $(devicemanager_SCRIPTS) Const.py.in

dist-hook : 
	- rm -f $(distdir)/Const.py

install-data-local :
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	- /bin/ln -s $(DESTDIR)$(datadir)/hal/device-manager/hal-device-manager $(DESTDIR)$(bindir)

uninstall-local :
	- rm -f $(DESTDIR)$(bindir)/hal-device-manager
