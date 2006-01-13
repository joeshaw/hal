"""This file contains the Representation class."""

import gtk

import Const

class Representation:
    """This class maps a device to presentation texts and icons used in
    the GUI"""

    def load_and_scale_icon(self, path):
        """Helper function for loading an icon and scaling it to 16x16"""
        orig = gtk.gdk.pixbuf_new_from_file(path)
        icon = gtk.gdk.Pixbuf(gtk.gdk.COLORSPACE_RGB, True, 8, 16, 16)
        orig.scale(icon, 0, 0, 16, 16,
                   0, 0,
                   16.0/orig.get_width(),
                   16.0/orig.get_height(),
                   gtk.gdk.INTERP_HYPER)
        return icon

    def __init__(self):
        """Init the class and load neccessary resources."""
        self.icons = {}
        self.icons["computer"] = self.load_and_scale_icon(Const.DATADIR + "/hal-computer.png")
        self.icons["bus_pci"] = self.load_and_scale_icon(Const.DATADIR + "/hal-bus-pci.png")
        self.icons["bus_usb"] = self.load_and_scale_icon(Const.DATADIR + "/hal-bus-usb.png")
        self.icons["abstract"] = self.load_and_scale_icon(Const.DATADIR + "/hal-abstract.png");
        self.icons["harddisk"] = self.load_and_scale_icon(Const.DATADIR + "/hal-harddisk.png");
        self.icons["cdrom"] = self.load_and_scale_icon(Const.DATADIR + "/hal-cdrom.png");
        self.icons["floppy"] = self.load_and_scale_icon(Const.DATADIR + "/hal-floppy.png");
        self.icons["unknown"] = self.load_and_scale_icon(Const.DATADIR + "/hal-unknown.png")
        self.icons["mouse"] = self.load_and_scale_icon(Const.DATADIR + "/hal-cat-mouse.png")
        self.icons["keyboard"] = self.load_and_scale_icon(Const.DATADIR + "/hal-cat-keyboard.png")
        self.icons["cardbus"] = self.load_and_scale_icon(Const.DATADIR + "/hal-cat-cardbus.png")
        self.icons["video"] = self.load_and_scale_icon(Const.DATADIR + "/hal-video.png")
        self.icons["flash"] = self.load_and_scale_icon(Const.DATADIR + "/hal-flash.png")
        self.icons["network"] = self.load_and_scale_icon(Const.DATADIR + "/hal-network.png")
        self.icons["audio"] = self.load_and_scale_icon(Const.DATADIR + "/hal-audio.png")
        self.icons["camera"] = self.load_and_scale_icon(Const.DATADIR + "/hal-camera.png")
        self.icons["serial"] = self.load_and_scale_icon(Const.DATADIR + "/hal-serial-port.png")


    def get_icon(self, device):
        """Given a Device object return an icon to display"""

        # Default to abstract icon
        icon = self.icons["abstract"]

        try:
            product = device.properties["info.product"]
        except KeyError:
            product = "Unknown"
        except TypeError:
            return icon
            
        if product=="Computer":
            return self.icons["computer"]

        # First look at bus type, every device got Bus property
        if device.properties.has_key("info.bus"):
	    bus = device.properties["info.bus"]
	    if bus=="usb_device":
		icon = self.icons["bus_usb"]
	    elif bus=="pci":
		icon = self.icons["bus_pci"]
	else:
	    bus = "unknown"

        # Then look at Category, if available
        if not device.properties.has_key("info.category"):
            return icon
        cat = device.properties["info.category"]
        if cat=="input.mouse":
            icon = self.icons["mouse"]
        elif cat=="input.keyboard":
            icon = self.icons["keyboard"]
        elif cat=="pcmcia_socket":
            icon = self.icons["cardbus"]
        elif cat=="video4linux":
            icon = self.icons["video"]
        elif cat=="dvb":
            icon = self.icons["video"]
        elif cat=="camera":
            icon = self.icons["camera"]
        elif cat=="storage" or cat=="storage.removable":
            if device.properties.has_key("storage.media"):
                media = device.properties["storage.media"]
                if media=="cdrom":
                    icon = self.icons["cdrom"]
                elif media=="floppy":
                    icon = self.icons["floppy"]
                elif media=="disk":
                    icon = self.icons["harddisk"]
                elif media=="flash":
                    icon = self.icons["flash"]
        elif cat=="net":
            icon = self.icons["network"]
        elif cat=="multimedia.audio":
            icon = self.icons["audio"]
        elif cat=="serial":
            icon = self.icons["serial"]

        return icon
