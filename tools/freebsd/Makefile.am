## Process this file with automake to produce Makefile.in

scriptdir = $(libexecdir)/scripts/freebsd

script_SCRIPTS =					\
	hal-system-power-suspend-freebsd		\
	hal-system-power-shutdown-freebsd		\
	hal-system-power-reboot-freebsd			\
	hal-system-lcd-get-brightness-freebsd		\
	hal-system-lcd-set-brightness-freebsd		\
	hal-system-power-set-power-save-freebsd

EXTRA_DIST = $(script_SCRIPTS)

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

