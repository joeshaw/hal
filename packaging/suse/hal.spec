#
# spec file for package hal (Version 0.5.4)
#
# Copyright (c) 2005 SUSE LINUX Products GmbH, Nuernberg, Germany.
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#
# Please submit bugfixes or comments via http://www.suse.de/feedback/
#

# norootforbuild
# neededforbuild  dbus-1 dbus-1-devel dbus-1-glib docbook-dsssl-stylesheets docbook-utils docbook_4 doxygen expat gtk2-devel-packages html-dtd intltool iso_ent libcap libpng libselinux libselinux-devel openjade opensp pciutils perl-XML-Parser popt popt-devel python python-devel sgml-skel xmlto

BuildRequires: aaa_base acl attr bash bind-utils bison bzip2 coreutils cpio cpp cracklib cvs cyrus-sasl db devs diffutils e2fsprogs file filesystem fillup findutils flex gawk gdbm-devel gettext-devel glibc glibc-devel glibc-locale gpm grep groff gzip info insserv klogd less libacl libattr libcom_err libgcc libnscd libselinux libstdc++ libxcrypt libzio m4 make man mktemp module-init-tools ncurses ncurses-devel net-tools netcfg openldap2-client openssl pam pam-modules patch permissions popt procinfo procps psmisc pwdutils rcs readline sed strace sysvinit tar tcpd texinfo timezone unzip util-linux vim zlib zlib-devel atk atk-devel autoconf automake binutils cairo cairo-devel dbus-1 dbus-1-devel dbus-1-glib docbook-dsssl-stylesheets docbook-utils docbook_4 doxygen expat fontconfig fontconfig-devel freetype2 freetype2-devel gcc gdbm gettext glib2 glib2-devel glitz glitz-devel gnome-filesystem gtk2 gtk2-devel html-dtd intltool iso_ent libcap libpixman libpixman-devel libpng libpng-devel libselinux-devel libtool libxml2 openjade opensp pango pango-devel pciutils perl perl-XML-Parser pkgconfig popt-devel python python-devel rpm sgml-skel update-desktop-files xmlto xorg-x11-devel xorg-x11-libs

Name:         hal
URL:          http://freedesktop.org/Software/hal
%define dbus_version            0.35.2
%define dbus_release            1
License:      Other License(s), see package, Other uncritical OpenSource License
Group:        System/Daemons
Version:      0.5.4
Release:      1
Autoreqprov:  on
Summary:      Daemon for Collecting Hardware Information
Source0:      %{name}-%{version}.tar.bz2
Source1:      rc.hal
Prereq:       /usr/sbin/groupadd /usr/sbin/useradd /etc/init.d/boot.localfs
Requires:     dbus-1 >= %{dbus_version}-%{dbus_release}, dbus-1-glib >= %{dbus_version}-%{dbus_release}, aaa_base
BuildRoot:    %{_tmppath}/%{name}-%{version}-build
%package -n hal-devel
Summary:      Developer package for HAL
Requires:     %{name} = %{version}-%{release}, dbus-1-devel >= %{dbus_version}-%{dbus_release}
Autoreqprov:  on
Group:        Development/Libraries/Other
%package -n hal-gnome
Summary:      GNOME based device manager for HAL
Requires:     %{name} = %{version}-%{release}, dbus-1-python >= %{dbus_version}-%{dbus_release}, python-gtk, python-gnome
Autoreqprov:  on
Group:        System/GUI/GNOME

%description
HAL is a hardware abstraction layer and aims to provide a live list of
devices present in the system at any point in time. HAL tries to
understand both physical devices (such as PCI, USB) and the device
classes (such as input, net, and block) physical devices have and it
allows merging of information from device info files specific to a
device.

HAL provides a network API through D-BUS for querying devices and
notifying when things change. Finally, HAL provides some monitoring (in
an unintrusive way) of devices. Presently, ethernet link detection and
volume mounts are monitored.

This, and more, is all described in the HAL specification.



Authors:
--------
    Danny Kukawka <danny.kukawka@web.de>
    Kay Sievers <kay.sievers@vrfy.org>
    Joe Shaw <joeshaw@novell.com>
    David Zeuthen <david@fubar.dk>


%description -n hal-devel
HAL is a hardware abstraction layer and aims to provide a live list of
devices present in the system at any point in time. HAL tries to
understand both physical devices (such as PCI, USB) and the device
classes (such as input, net and block) physical devices have, and it
allows merging of information from so called device info files specific
to a device.

HAL provides a network API through D-BUS for querying devices and
notifying when things change. Finally, HAL provides some monitoring (in
an unintrusive way) of devices, presently ethernet link detection and
volume mounts are monitored.

This, and more, is all described in the HAL specification



Authors:
--------
    Danny Kukawka <danny.kukawka@web.de>
    Kay Sievers <kay.sievers@vrfy.org>
    Joe Shaw <joeshaw@novell.com>
    David Zeuthen <david@fubar.dk>


%description -n hal-gnome
GNOME program for displaying the devices detected by HAL



Authors:
--------
    Danny Kukawka <danny.kukawka@web.de>
    Kay Sievers <kay.sievers@vrfy.org>
    Joe Shaw <joeshaw@novell.com>
    David Zeuthen <david@fubar.dk>

%prep
%setup -n %{name}-%{version} -q
#rm -rf py-compile 4> /dev/null || :
#ln -s /usr/share/automake-1.9/py-compile py-compile 2> /dev/null || :

%build
export CFLAGS="${RPM_OPT_FLAGS}"
export CXXFLAGS="${RPM_OPT_FLAGS}"
./autogen.sh
#autoreconf -fi
./configure 						\
	--prefix=%{_prefix} 				\
	--sysconfdir=%{_sysconfdir} 			\
	--localstatedir=%{_localstatedir} 		\
	--libdir=%{_libdir} 				\
	--libexecdir=%{_sbindir} 			\
	--mandir=%{_mandir} 				\
        --with-init-scripts=suse 			\
	--with-hwdata=/usr/share 			\
	--enable-hotplug-map				\
	--with-pid-file=/var/run/hal/haldaemon.pid 	\
	--with-dbus-sys=/etc/dbus-1/system.d 		\
	--with-hal-user=haldaemon 			\
	--with-hal-group=haldaemon 			\
	--with-doc-dir=%{_datadir}/doc/packages/hal	\
	--enable-selinux 				\
	--enable-pcmcia-support 			\
	--enable-sysfs-carrier 				\
	--disable-acpi-proc				\
	--enable-docbook-docs 				\
	--enable-doxygen-docs
make

%install
make DESTDIR=$RPM_BUILD_ROOT install
mkdir -p $RPM_BUILD_ROOT/etc/hal
mkdir -p $RPM_BUILD_ROOT/etc/dbus-1/system.d
mkdir -p $RPM_BUILD_ROOT/etc/init.d
mkdir -p $RPM_BUILD_ROOT/usr/sbin
mkdir -p $RPM_BUILD_ROOT/usr/share/hal/device-manager
install -m 755 %{SOURCE1} $RPM_BUILD_ROOT/%{_sysconfdir}/init.d/haldaemon
ln -sf %{_sysconfdir}/init.d/haldaemon $RPM_BUILD_ROOT/%{_sbindir}/rchal
rm -f $RPM_BUILD_ROOT%{_libdir}/*.la
install -d $RPM_BUILD_ROOT/%{_localstatedir}/run/hal

%clean
rm -rf %{buildroot}

%pre
/usr/sbin/groupadd -r haldaemon 2> /dev/null || :
/usr/sbin/useradd -r -o -s /bin/false -c "User for haldaemon" -d /var/run/hal -g haldaemon haldaemon 2> /dev/null || :

%preun
%{stop_on_removal haldaemon}

%post
%{insserv_force_if_yast haldaemon}
%{run_ldconfig}

%postun
%{restart_on_update haldaemon}
%{insserv_cleanup}
%{run_ldconfig}

%files
%defattr(-, root, root)
%dir %{_sysconfdir}/dbus-1/system.d
%dir %{_sysconfdir}/hal
%dir %{_sysconfdir}/init.d/haldaemon
%dir %{_datadir}/doc/packages/hal/conf
%dir %{_datadir}/doc/packages/hal/spec
%dir %{_datadir}/hal
%dir %{_datadir}/hal/fdi
%config %{_sysconfdir}/dbus-1/system.d/hal.conf
%{_bindir}/lshal
%{_bindir}/hal-*-property
%{_bindir}/hal-device
%{_bindir}/hal-find-by-capability
%{_datadir}/hal/fdi/*
%{_datadir}/locale/*/LC_MESSAGES/hal.mo
%{_datadir}/doc/packages/hal/conf/*
%{_datadir}/doc/packages/hal/spec/*
%{_libdir}/*hal*.so.*
%{_sbindir}/hal*
%{_sbindir}/rchal
%attr(-,haldaemon,haldaemon) %{_localstatedir}/run/hal

%files -n hal-devel
%defattr(-, root, root)
%dir %{_datadir}/doc/packages/hal
%dir %{_datadir}/doc/packages/hal/api
%{_includedir}/*
%{_libdir}/lib*.a
%{_libdir}/lib*.so
%{_libdir}/pkgconfig/*
%{_datadir}/doc/packages/hal/api/*

%files -n hal-gnome
%defattr(-, root, root)
%dir %{_datadir}/hal/device-manager
%{_datadir}/hal/device-manager/*
%{_bindir}/hal-device-manager

%changelog -n hal
#add hal.changelog to generate entries
