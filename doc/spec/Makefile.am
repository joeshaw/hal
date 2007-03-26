
FIGURE_FILES =				\
	hal-arch.png			\
	hal-devices1.png

SPEC_XML_EXTRA_FILES =			\
	hal-spec-introduction.xml	\
	hal-spec-fdi-files.xml		\
	hal-spec-properties.xml		\
	hal-spec-locking.xml		\
	hal-spec-interfaces.xml	

if DOCBOOK_DOCS_ENABLED

htmldocdir = $(DOCDIR)/spec
htmldoc_DATA = hal-spec.html $(FIGURE_FILES) docbook.css

hal-spec.html : hal-spec.xml $(SPEC_XML_EXTRA_FILES) $(FIGURE_FILES)
	${XMLLINT} --xinclude hal-spec.xml > ~hal-spec.xml || rm -f ~hal-spec.xml
	mv -f ~hal-spec.xml hal-spec.xml
	$(XMLTO) html-nochunks -m config.xsl hal-spec.xml  || rm -f hal-spec.xml
	rm -f hal-spec.xml

clean-local:
	rm -f *~
	rm -f *.html hal-spec.xml

endif # DOCBOOK_DOCS_ENABLED

EXTRA_DIST =				\
	hal-spec.xml.in			\
	config.xsl			\
	docbook.css			\
	$(SPEC_XML_EXTRA_FILES)		\
	hal-arch.dia			\
	$(FIGURE_FILES)
