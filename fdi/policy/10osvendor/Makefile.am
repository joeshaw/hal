
fdidir = $(datadir)/hal/fdi/policy/10osvendor

dist_fdi_DATA = 			\
	10-keyboard-policy.fdi		\
	10-power-mgmt-policy.fdi	\
	10-toshiba-buttons.fdi		\
	10-laptop-panel-mgmt-policy.fdi	\
	15-storage-luks.fdi		\
	20-storage-methods.fdi

if BUILD_CPUFREQ
dist_fdi_DATA += 10-cpufreq.fdi
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
