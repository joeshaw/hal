## Process this file with automake to produce Makefile.in

SUBDIRS = libhal libhal-storage partutil  hald hald-runner tools \
          fdi doc policy

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = hal.pc hal-storage.pc

dbusdir = $(DBUS_SYS_DIR)
dist_dbus_DATA = hal.conf

# Creating ChangeLog from git log (taken from cairo/Makefile.am):
ChangeLog: $(srcdir)/ChangeLog

$(srcdir)/ChangeLog:
	@if test -d "$(srcdir)/.git"; then \
	  (cd "$(srcdir)" && \
	  ./missing --run git-log --stat) | fmt --split-only > $@.tmp \
	  && mv -f $@.tmp $@ \
	  || ($(RM) $@.tmp; \
	      echo Failed to generate ChangeLog, your ChangeLog may be outdated >&2; \
	      (test -f $@ || echo git-log is required to generate this file >> $@)); \
	else \
	  test -f $@ || \
	  (echo A git checkout and git-log is required to generate ChangeLog >&2 && \
	  echo A git checkout and git-log is required to generate this file >> $@); \
	fi

.PHONY: ChangeLog $(srcdir)/ChangeLog

MAINTAINERCLEANFILES = ChangeLog
DISTCLEANFILES = hal.pc hal-storage.pc

EXTRA_DIST = HACKING hal.pc.in hal-storage.pc.in hal.conf.in ChangeLog

clean-local :
	rm -f *~
