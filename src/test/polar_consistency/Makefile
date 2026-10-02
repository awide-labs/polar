#-------------------------------------------------------------------------
#
# Makefile for src/test/polar_consistency
#
# Copyright (c) 2022, Alibaba Group Holding Limited
#
# src/test/polar_consistency/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL = src/test/modules/injection_points contrib/test_decoding external/polar_monitor external/polar_worker

subdir = src/test/polar
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export enable_injection_points
export enable_fault_injector

ifeq ($(enable_tap_tests)$(NO_TAP_TESTS),yes)

check: | temp-install
	export DCCHECK_ALL=0 && $(prove_check)

check-all: | temp-install
	export with_pgbench=1 && export DCCHECK_ALL=1 && $(prove_check)

installcheck:
	export DCCHECK_ALL=0 && $(prove_installcheck)

installcheck-all: all
	export with_pgbench=1 && export DCCHECK_ALL=1 && $(prove_installcheck)

else

check: | temp-install
	$(prove_check)

check-all: | temp-install
	$(prove_check)

installcheck:
	$(prove_installcheck)

installcheck-all: all
	$(prove_installcheck)

endif

clean:
	rm -rf tmp_check

distclean maintainer-clean:
	rm -rf tmp_check
	rm -f dccheck_schedule_list
