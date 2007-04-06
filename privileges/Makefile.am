
if HAVE_POLKIT
polkit_privilegedir = $(sysconfdir)/PolicyKit/privileges

dist_polkit_privilege_DATA =    \
	hal-storage.priv	\
	hal-power.priv		\
	hal-killswitch.priv

if HAVE_ACLMGMT
dist_polkit_privilege_DATA += hal-device-file.priv
endif

check:
	for f in $(dist_polkit_privilege_DATA); do \
          echo "Validating privilege file: $$f"; \
          $(POLKIT_PRIVILEGE_FILE_VALIDATE) --file $(srcdir)/$$f; \
          if [ "$$?" != "0" ]; then \
            echo "failed"; \
            exit 1; \
          fi; \
        done

endif

clean-local :
	rm -f *~
