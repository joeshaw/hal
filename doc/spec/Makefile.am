
if DOCBOOK_DOCS_ENABLED

FIGURE_FILES =                        	\
	hal-arch.png                  	\
	hal-devices1.png              	\
	hal-fdi-example1.png      	\
	hal-fdi-example2.png      	\
	hal-fdi-example3.png      	\
	hal-linux26.png	


htmldocdir = $(datadir)/doc/hal-$(VERSION)/spec
htmldoc_DATA = hal-spec.html $(FIGURE_FILES)

hal-spec.html : hal-spec.xml $(FIGURE_FILES)
	$(DOCBOOK) --nochunks hal-spec.xml -o .

EXTRA_DIST = hal-spec.html       \
	     hal-spec.xml.in     \
	     example-manager.py	 \
	     hal-arch.dia	 \
	     hal-linux26.dia     \
	     $(FIGURE_FILES)

clean-local:
	rm -f *~
	rm -f hal-spec.html hal-spec.xml


endif # DOCBOOK_DOCS_ENABLED
