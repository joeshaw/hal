## Process this file with automake to produce Makefile.in

udevrulesdir = $(sysconfdir)/udev/rules.d
udevrules_DATA = 90-hal.rules

scriptdir = $(libdir)/hal/scripts/linux

script_SCRIPTS =					\
	hal-luks-setup-linux                          	\
	hal-luks-teardown-linux                       	\
	hal-luks-remove-linux		                \
	hal-system-power-suspend-linux			\
	hal-system-power-hibernate-linux		\
	hal-system-power-shutdown-linux			\
	hal-system-power-reboot-linux			\
	hal-system-lcd-get-brightness-linux		\
	hal-system-lcd-set-brightness-linux		\
	hal-system-power-set-power-save-linux		\
	hal-system-bluetooth-get-power-linux		\
	hal-system-bluetooth-set-power-linux

EXTRA_DIST = 90-hal.rules $(script_SCRIPTS)

check:
	for f in $(script_SCRIPTS); do \
            echo -n "Validate bash syntax in $$f : "; \
            bash -n $(srcdir)/$$f 2> bash.error;\
            if test -s bash.error; then \
                echo failed; \
                cat bash.error; \
                rm -f bash.error; \
                exit 1; \
            else \
                echo ok; \
                rm -f bash.error; \
            fi; \
        done;

