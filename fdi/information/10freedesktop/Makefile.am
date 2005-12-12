
fdidir = $(datadir)/hal/fdi/information/10freedesktop

dist_fdi_DATA = 10-usb-card-readers.fdi    \
		10-usb-music-players.fdi   \
		10-usb-zip-drives.fdi      \
		10-wireless-mice.fdi	   \
		10-camera-ptp.fdi	   \
		10-usb-pda.fdi		   \
		10-cd-dvd-burner.fdi

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
