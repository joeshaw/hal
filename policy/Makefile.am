
if HAVE_POLKIT
polkit_policydir = $(datadir)/PolicyKit/policy

dist_polkit_policy_DATA =    				\
	org.freedesktop.hal.policy			\
	org.freedesktop.hal.storage.policy		\
	org.freedesktop.hal.power-management.policy	\
	org.freedesktop.hal.killswitch.policy		\
	org.freedesktop.hal.wol.policy

if HAVE_ACLMGMT
dist_polkit_policy_DATA += org.freedesktop.hal.device-access.policy
endif

check:
	$(POLKIT_POLICY_FILE_VALIDATE) $(dist_polkit_policy_DATA)

endif

clean-local :
	rm -f *~
