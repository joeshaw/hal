
fdidir = $(datadir)/hal/fdi/policy/10osvendor

dist_fdi_DATA = 			\
	10-keyboard-policy.fdi		\
	10-power-mgmt-policy.fdi	\
	10-toshiba-buttons.fdi		\
	10-bluetooth-switch.fdi		\
	10-laptop-panel-mgmt-policy.fdi	\
	15-storage-luks.fdi		\
	20-storage-methods.fdi

if BUILD_MACBOOKPRO
dist_fdi_DATA += 10-macbookpro-utils.fdi
endif

if BUILD_CPUFREQ
dist_fdi_DATA += 10-cpufreq.fdi
endif

if BUILD_USBCSR
dist_fdi_DATA += 10-usbcsr-mice.fdi
endif

if BUILD_OMAP
dist_fdi_DATA += 10-omap-backlight.fdi
endif

if BUILD_DELL
dist_fdi_DATA += 10-dell-laptop-brightness.fdi
endif

if HAVE_ACLMGMT
dist_fdi_DATA += 20-acl-management.fdi
endif

check:
	for f in $(dist_fdi_DATA); do \
            echo -n "Validate XML in $$f : "; \
            xmllint --noout --dtdvalid $(top_srcdir)/fdi/fdi.dtd $(srcdir)/$$f 2> xmllint.error; \
            if test -s xmllint.error; \
              then \
                echo failed; \
                cat xmllint.error; \
                rm -f xmllint.error; \
                exit 1; \
            else \
                echo ok; \
                rm -f xmllint.error; \
            fi; \
        done;

clean-local :
	rm -f *~
