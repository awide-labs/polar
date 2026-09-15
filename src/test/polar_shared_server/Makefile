#-------------------------------------------------------------------------
#
# Makefile for src/test/polar_shared_server
#
# Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/polar_shared_server/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL=contrib/hstore external/polar_monitor

subdir = src/test/polar_shared_server
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean maintainer-clean:
	rm -rf tmp_check
