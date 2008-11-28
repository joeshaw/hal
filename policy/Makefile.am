
if HAVE_POLKIT
polkit_policydir = $(datadir)/PolicyKit/policy

dist_polkit_policy_DATA =    				\
	org.freedesktop.hal.policy			\
	org.freedesktop.hal.storage.policy		\
	org.freedesktop.hal.power-management.policy	\
	org.freedesktop.hal.killswitch.policy		\
	org.freedesktop.hal.dockstation.policy		\
	org.freedesktop.hal.leds.policy			\
	org.freedesktop.hal.wol.policy

if HAVE_ACLMGMT
dist_polkit_policy_DATA += org.freedesktop.hal.device-access.policy
endif

check:
	for f in $(dist_polkit_policy_DATA); do \
            echo -n "Validate PolicyKit policy in $$f : "; \
            $(POLKIT_POLICY_FILE_VALIDATE) $(srcdir)/$$f ; \
	    ret=$$?; \
            if  [ "$$ret" == "0" ]; \
	      then \
                echo ok; \
            else \
                echo failed; \ 
		exit 1; \
            fi; \
        done;
endif

clean-local :
	rm -f *~
