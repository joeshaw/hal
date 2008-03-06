
fdidir = $(datadir)/hal/fdi/information/10freedesktop

dist_fdi_DATA = 		

if BUILD_DEPRECATED_KEYS
dist_fdi_DATA += 01-deprecated-keys.fdi 
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
