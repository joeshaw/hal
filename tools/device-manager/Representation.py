"""This file contains the Representation class."""

import gtk

import Const

class Representation:
    """This class maps a device to presentation texts and icons used in
    the GUI"""

    def load_and_scale_icon(self, path):
        """Helper function for loading an icon and scaling it to 16x16"""
        orig = gtk.gdk.pixbuf_new_from_file(path)
        icon = gtk.gdk.Pixbuf(gtk.gdk.COLORSPACE_RGB, gtk.TRUE, 8, 16, 16)
        orig.scale(icon, 0, 0, 16, 16,
                   0, 0,
                   16.0/orig.get_width(),
                   16.0/orig.get_height(),
                   gtk.gdk.INTERP_HYPER)
        return icon

    def __init__(self):
        """Init the class and load neccessary resources."""
        self.icons = {}
        self.icons["bus_pci"] = self.load_and_scale_icon("hal-bus-pci.png")
        self.icons["bus_usb"] = self.load_and_scale_icon("hal-bus-usb.png")
        self.icons["abstract"] = self.load_and_scale_icon("hal-abstract.png");
        self.icons["harddisk"] = self.load_and_scale_icon("hal-harddisk.png");
        self.icons["cdrom"] = self.load_and_scale_icon("hal-cdrom.png");
        self.icons["floppy"] = self.load_and_scale_icon("hal-floppy.png");
        self.icons["unknown"] = self.load_and_scale_icon("hal-unknown.png")
        self.icons["mouse"] = self.load_and_scale_icon("hal-cat-mouse.png")
        self.icons["keyboard"] = self.load_and_scale_icon("hal-cat-keyboard.png")
        self.icons["cardbus"] = self.load_and_scale_icon("hal-cat-cardbus.png")
        self.icons["video"] = self.load_and_scale_icon("hal-video.png")
        self.icons["flash"] = self.load_and_scale_icon("hal-flash.png")
        self.icons["network"] = self.load_and_scale_icon("hal-network.png")
        self.icons["audio"] = self.load_and_scale_icon("hal-audio.png")
        self.icons["camera"] = self.load_and_scale_icon("hal-camera.png")

        
    def get_icon(self, device):
        """Given a Device object return an icon to display"""

        # Default to abstract icon
        icon = self.icons["abstract"]
            
        # First look at bus type, every device got Bus property
        bus = device.properties["Bus"]
        if bus=="usb":
            icon = self.icons["bus_usb"]
        elif bus=="pci":
            icon = self.icons["bus_pci"]

        # Then look at Category, if available
        if not device.properties.has_key("Category"):
            return icon
        cat = device.properties["Category"]
        if cat=="input.mouse":
            icon = self.icons["mouse"]
        elif cat=="input.keyboard":
            icon = self.icons["keyboard"]
        elif cat=="bridge.cardBus":
            icon = self.icons["cardbus"]
        elif cat=="video":
            icon = self.icons["video"]
        elif cat=="removableMedia.cdrom":
            icon = self.icons["cdrom"]
        elif cat=="removableMedia.floppy":
            icon = self.icons["floppy"]
        elif cat=="fixedMedia.harddisk":
            icon = self.icons["harddisk"]
        elif cat=="fixedMedia.flash":
            icon = self.icons["flash"]
        elif cat=="net":
            icon = self.icons["network"]
        elif cat=="multimedia.audio":
            icon = self.icons["audio"]

        return icon
