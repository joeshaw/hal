
if MAN_PAGES_ENABLED

MAN_IN_FILES = hald.1.in lshal.1.in hal-get-property.1.in hal-set-property.1.in hal-find-by-property.1.in hal-find-by-capability.1.in hal-is-caller-locked-out.1.in

man_MANS = hald.1 lshal.1 hal-get-property.1 hal-set-property.1 hal-find-by-property.1 hal-find-by-capability.1 hal-is-caller-locked-out.1

endif # MAN_PAGES_ENABLED

EXTRA_DIST=$(man_MANS) $(MAN_IN_FILES)

clean-local:
	rm -f *~
