"""This module contains the LibGladeApplication class."""

import signal
import gtk
from gtk import glade

from GtkAttributesFacade import GtkAttributesFacade 


class LibGladeApplication:

    """This is the base class for applications that use Glade.
    
    The following attributes are used:

    xml - This is an instance of glade.XML which encapsulates the Glade GUI.

    The following private variables are used:

    _signalsAreDone - The UNIX signal handlers only have to be set once. 

    """

    _signalsAreDone = False

    def __init__(self, gladeFile):
        """Setup the appropriate signal handlers and call setHandlers."""
        if not self._signalsAreDone:
            self._signalsAreDone = True
            signal.signal(signal.SIGINT, signal.SIG_DFL)
        self.xml = glade.XML(gladeFile)
        self.setHandlers()

    def setHandlers(self):
        """Automatically autoconnect all of the handlers.
        
        Any methods (even in subclasses) that start with "on_" will be treated
        as a handler that is automatically connected to by
        xml.signal_autoconnect.

        """
        handlers = {}
        for i in dir(self):
            if i.startswith("on_"):
                handlers[i] = getattr(self, i)
        self.xml.signal_autoconnect(handlers)

    def __getattr__(self, name):
        """If self doesn't have the attribute, check self.xml.

        If self.xml does have the attribute, wrap it in a GtkAttributesFacade 
        instance, cache it in self, and return it.
        
        """
        obj = self.xml.get_widget(name)
        if obj:
            obj = GtkAttributesFacade(obj) 
            setattr(self, name, obj)
            return obj
        raise AttributeError("%s instance has no attribute '%s'" % 
            (self.__class__.__name__, name))

    def on_quit_activate(self, *args):
        """Ignore args and call gtk.mainquit()."""
        gtk.main_quit()
