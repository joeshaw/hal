
if HAVE_POLKIT
polkit_privilegedir = $(sysconfdir)/PolicyKit/privilege.d

dist_polkit_privilege_DATA =                              \
	hal-storage-fixed-mount.privilege                 \
	hal-storage-fixed-mount-all-options.privilege     \
	hal-storage-removable-mount.privilege             \
	hal-storage-removable-mount-all-options.privilege \
	hal-power-suspend.privilege                       \
	hal-power-hibernate.privilege                     \
	hal-power-poweroff.privilege                      \
	hal-power-reboot.privilege                        \
	hal-power-cpufreq.privilege
endif

clean-local :
	rm -f *~
