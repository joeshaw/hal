"""This file contains the Device class."""

import Const

class Device:
    def __init__(self, device_name, parent_name, properties):
        self.device_name = device_name
        self.parent_name = parent_name
        self.parent_device = None
        self.properties = properties
        self.children = []

    def print_tree(self, indent):
        if indent==0:
            print " "*indent + self.device_name
        else:
            print " "*indent + "- " + self.device_name
        for c in self.children:
            c.print_tree(indent+4)

    def populate_gtk_tree(self, tree_model, dont_show_virtual, representation):
        # see if we should show virtual devices
        if dont_show_virtual:
            try:
                if self.properties["info.virtual"]:
                    # do show all block devices, ide channels
                    if not self.properties["info.bus"] in ["block", "ide_host"]:
                        self.row = self.parent_device.row
                        # and recurse the childs
                        for c in self.children:
                            c.populate_gtk_tree(tree_model,
                                                dont_show_virtual,
                                                representation)
                        return
            except:
                pass
        if self.parent_device==None:
            self.row = None
        else:
            self.row = tree_model.append(self.parent_device.row)

        if self.row != None:
            # Get icon
            icon = representation.get_icon(self)

            tree_model.set_value(self.row, Const.PIXBUF_COLUMN, icon)
            try:
                title_name = self.properties["info.product"]
            except KeyError:
                title_name = "Unknown Device"
            except TypeError:
                title_name = "Unknown Device"
            tree_model.set_value(self.row, Const.TITLE_COLUMN, title_name)
            tree_model.set_value(self.row, Const.UDI_COLUMN, self.device_name)

        for c in self.children:
            c.populate_gtk_tree(tree_model,
                                dont_show_virtual,
                                representation)
            
    def find_by_udi(self, device_udi):
        if self.device_name==device_udi:
            return self
        for c in self.children:
            rc = c.find_by_udi(device_udi)
            if rc!=None:
                return rc
        return None
