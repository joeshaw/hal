
fdidir = $(sysconfdir)/hal/policy/10osvendor

dist_fdi_DATA = 10-storage-policy.fdi 10-power-mgmt-policy.fdi

if FSTAB_SYNC_ENABLED
dist_fdi_DATA += 90-fstab-sync.fdi
endif

clean-local :
	rm -f *~
