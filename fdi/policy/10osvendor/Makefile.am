
fdidir = $(datadir)/hal/fdi/policy/10osvendor

dist_fdi_DATA = 			\
	10-storage-policy.fdi		\
	10-power-mgmt-policy.fdi	\
	10-laptop-panel-mgmt-policy.fdi	\
	15-storage-luks.fdi

if HAVE_SELINUX
dist_fdi_DATA += 20-storage-add-selinux.fdi
endif

if FSTAB_SYNC_ENABLED
dist_fdi_DATA += 90-fstab-sync.fdi
endif

clean-local :
	rm -f *~
