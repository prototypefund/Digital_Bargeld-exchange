# This Makefile.am is in the public domain
all:
	@echo -e \
"Generate documentation:\n" \
"\tmake full - full documentation with dependency graphs (slow)\n" \
"\tmake fast - fast mode without dependency graphs"

full: taler.doxy
	doxygen $<

fast: taler.doxy
	sed 's/\(HAVE_DOT.*=\).*/\1 NO/' $< | doxygen -

clean:
	rm -rf html

EXTRA_DIST = \
   taler.doxy
