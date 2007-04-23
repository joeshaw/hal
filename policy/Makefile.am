
if HAVE_POLKIT
polkit_privilegedir = $(sysconfdir)/PolicyKit/policy

dist_polkit_privilege_DATA =    \
	hal-lock.policy		\
	hal-storage.policy	\
	hal-power.policy	\
	hal-killswitch.policy	

if HAVE_ACLMGMT
dist_polkit_privilege_DATA += hal-device-file.policy
endif

check:
	$(POLKIT_POLICY_FILE_VALIDATE) $(dist_polkit_privilege_DATA)

endif

clean-local :
	rm -f *~
