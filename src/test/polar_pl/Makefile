#-------------------------------------------------------------------------
#
# Makefile for src/test/polar_pl
#
# src/test/polar_pl/Makefile
#
#-------------------------------------------------------------------------

export enable_fault_injector
export with_ssl

EXTRA_INSTALL = external/polar_monitor
EXTRA_INSTALL += contrib/pg_stat_statements

ifeq ($(with_ssl),openssl)
EXTRA_INSTALL += contrib/sslinfo
endif

subdir = src/test/polar_pl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean maintainer-clean:
	rm -rf tmp_check
