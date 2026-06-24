#-------------------------------------------------------------------------
#
# Makefile for src/test/polar_pl
#
# Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/polar_pl/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL=contrib/test_decoding

subdir = src/test/polar_datamax
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean maintainer-clean:
	rm -rf tmp_check
