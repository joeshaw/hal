"""This module contains the GtkAttributesFacade class."""

from gobject import GObject


class GtkAttributesFacade:

    """Wrap a GTK instance to simplify the way attributes are referenced.

    Given a GTK instance i, make it possible for any functions i.get_foo() and
    i.set_foo(value) to be accessed via i.foo like a normal attribute.

    The following attributes are used:

    instance - This is the GTK instance that is being wrapped.

    """

    def __init__(self, instance):
        """Accept the instance."""
        self.__dict__["instance"] = instance

    def __setattr__(self, name, value):
        """Simplify the way attributes are referenced.

        When trying to do self.foo = something, if there is a
        self.instance.set_foo() method, use it.  Otherwise, just set the
        attribute in self.

        Return value so that chaining is possible.
        
        """
        setter = "set_" + name
        if hasattr(self.instance, setter):
            apply(getattr(self.instance, setter), [value])
        else:
            self.__dict__[name] = value
        return value

    def __getattr__(self, name):
        """Simplify the way attributes are referenced.

        Remember that this method is called after a failed lookup in self.  Try
        looking for self.instance.foo.  Next, try looking for a
        self.instance.get_foo() method, and call it if it exists.  Otherwise,
        raise an exception.

        If a value is successfully looked up, and if it is a subclass of 
        GObject, wrap it in a GtkAttributesFacade before returning it.
        
        """
        getter = "get_" + name
        if hasattr(self.instance, name):
            ret = getattr(self.instance, name)
        elif hasattr(self.instance, getter):
            ret = apply(getattr(self.instance, getter))
        else:
            raise AttributeError(
                "%s instance has no attribute '%s'" % 
                    (self.instance.__class__.__name__, name))
        if isinstance(ret, GObject):
            ret = GtkAttributesFacade(ret)
        return ret
