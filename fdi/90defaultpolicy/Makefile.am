
fdi90defaultpolicydir = $(datadir)/hal/fdi/90defaultpolicy

dist_fdi90defaultpolicy_DATA = storage-policy.fdi power-mgmt-policy.fdi

check:
	xmllint --noout --dtdvalid $(top_srcdir)/fdi/fdi.dtd $(dist_fdi90defaultpolicy_DATA)


