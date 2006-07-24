
FIGURE_FILES =                  \
	hal-arch.png            \
	hal-devices1.png        \
	hal-fdi-example1.png    \
	hal-fdi-example2.png    \
	hal-fdi-example3.png	\
	hal-linux26.png	

SPEC_XML_EXTRA_FILES =                    \
	hal-spec-examples.xml       \
	hal-spec-introduction.xml   \
	hal-spec-fdi-files.xml      \
	hal-spec-properties.xml

if DOCBOOK_DOCS_ENABLED

htmldocdir = $(DOCDIR)/spec
htmldoc_DATA = hal-spec.html $(FIGURE_FILES)

hal-spec.html : hal-spec.xml $(SPEC_XML_EXTRA_FILES) $(FIGURE_FILES)
	${XMLLINT} --xinclude hal-spec.xml.in > hal-spec.xml 
	$(DOCBOOK) --nochunks hal-spec.xml -o .

clean-local:
	rm -f *~
	rm -f hal-spec.html hal-spec.xml

endif # DOCBOOK_DOCS_ENABLED

EXTRA_DIST =              \
  hal-spec.html           \
  hal-spec.xml.in         \
  config.xsl              \
  docbook.css             \
  $(SPEC_XML_EXTRA_FILES) \
  example-manager.py	  \
  hal-arch.dia	          \
  hal-linux26.dia         \
  $(FIGURE_FILES)
