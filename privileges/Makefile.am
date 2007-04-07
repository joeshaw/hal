
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
	$(POLKIT_PRIVILEGE_FILE_VALIDATE) $(dist_polkit_privilege_DATA)

endif

clean-local :
	rm -f *~
