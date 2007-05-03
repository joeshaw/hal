
MAN_IN_FILES = hald.8.in lshal.1.in hal-get-property.1.in hal-set-property.1.in hal-find-by-property.1.in hal-find-by-capability.1.in hal-is-caller-locked-out.1.in hal-lock.1.in hal-disable-polling.1.in

if MAN_PAGES_ENABLED

man_MANS = $(MAN_IN_FILES:.in=)

if HAVE_POLKIT
man_MANS += hal-is-caller-privileged.1
endif

endif # MAN_PAGES_ENABLED

EXTRA_DIST = $(MAN_IN_FILES) hal-is-caller-privileged.1.in

clean-local:
	rm -f *~ *.1 *.8

%: %.in Makefile
	$(edit) $< >$@

edit = sed \
	-e 's|@docdir[@]|$(docdir)|g' \
	-e 's|@sbindir[@]|$(sbindir)|g' \
	-e 's|@sysconfdir[@]|$(sysconfdir)|g'

