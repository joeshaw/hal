
if DOCBOOK_DOCS_ENABLED

EXTRA_DIST =                          \
	hal-spec.xml

FIGURE_FILES =                        \
	hal-arch.png                  \
	hal-devices1.png              \
	hal-fdi-example1.png

EXTRA_DIST += hal-spec.html $(FIGURE_FILES)

htmldocdir = $(datadir)/doc/hal-$(VERSION)/spec
htmldoc_DATA = hal-spec.html $(FIGURE_FILES)

hal-spec.html : hal-spec.xml $(FIGURE_FILES)
	$(DOCBOOK) --nochunks hal-spec.xml -o .

clean-local:
	rm -f *~
	rm -f hal-spec.html


endif # DOCBOOK_DOCS_ENABLED
