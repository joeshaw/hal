%define expat_version           1.95.5
%define glib2_version           2.2.0
%define dbus_version            0.21

Summary: Hardware Abstraction Layer
Name: hal
Version: 0.2.95
Release: 1
URL: http://www.freedesktop.org/software/hal/
Source0: %{name}-%{version}.tar.gz
License: AFL/GPL
Group: System Environment/Libraries
BuildRoot: %{_tmppath}/%{name}-root
PreReq: chkconfig /usr/sbin/useradd
Packager: David Zeuthen <david@fubar.dk>
BuildRequires: expat-devel >= %{expat_version}
BuildRequires: glib2-devel >= %{glib2_version}
BuildRequires: dbus-devel  >= %{dbus_version}
BuildRequires: python python-devel
Requires: dbus >= %{dbus_version}
Requires: dbus-glib >= %{dbus_version}
Requires: glib2 >= %{glib2_version}

%description

HAL is daemon for collection and maintaining information from several
sources about the hardware on the system. It provdes a live device
list through D-BUS.


%package gnome
Summary: GNOME based device manager for HAL
Group: Development/Libraries
Requires: %name = %{version}-%{release}
Requires: dbus-python >= %{dbus_version}
Requires: pygtk2 >= 2.0.0
Requires: gnome-python2 >= 2.0.0

%description gnome
GNOME program for displaying the devices detected by HAL


%package devel
Summary: Libraries and headers for HAL
Group: Development/Libraries
Requires: %name = %{version}-%{release}

%description devel

Headers and static libraries for HAL.


%prep
%setup -q

%build

%configure
make

%install
make install DESTDIR=$RPM_BUILD_ROOT

rm -f $RPM_BUILD_ROOT%{_libdir}/*.la

%clean
rm -rf %{buildroot}

%pre
# Add the "haldaemon" user
/usr/sbin/useradd -c 'HAL daemon' \
	-s /sbin/nologin -r -d '/' haldaemon 2> /dev/null || :

%post
/sbin/ldconfig
/sbin/chkconfig --add haldaemon

%preun
if [ $1 = 0 ]; then
    service haldaemon stop > /dev/null 2>&1
    /sbin/chkconfig --del haldaemon
fi

%postun
/sbin/ldconfig
if [ "$1" -ge "1" ]; then
  service haldaemon condrestart > /dev/null 2>&1
fi

%files
%defattr(-,root,root)

%doc COPYING ChangeLog NEWS

%dir %{_sysconfdir}/dbus-1/system.d
%config %{_sysconfdir}/dbus-1/system.d/hal.conf
%config %{_sysconfdir}/rc.d/init.d/*
%{_sysconfdir}/dev.d/default/hal.dev

%{_sbindir}/hald

%{_bindir}/lshal
%{_bindir}/hal-get-property
%{_bindir}/hal-set-property

%{_libdir}/*hal*.so.*

%{_libexecdir}/hal.hotplug
%{_libexecdir}/hal.dev
%{_sysconfdir}/hal/hald.conf
/etc/hotplug.d/default/hal.hotplug

%dir %{_datadir}/hal
%dir %{_datadir}/hal/fdi
%{_datadir}/hal/fdi/*



%files devel
%defattr(-,root,root)

%{_libdir}/lib*.a
%{_libdir}/lib*.so
%{_libdir}/pkgconfig/*
%{_includedir}/*



%files gnome
%defattr(-,root,root)

%dir %{_datadir}/hal/device-manager
%{_datadir}/hal/device-manager/*
%{_bindir}/hal-device-manager




%changelog
* Sat May 15 2004 David Zeuthen <david@fubar.dk> 0.2.91-1
- updated to new version

* Sat May 15 2004 Owen Fraser-Green <owen@discobabe.net> 0.2.90-1
- updated to new version

* Tue Dec 30 2003 David Zeuthen <david@fubar.dk> 0.2.1-1
- initial build

