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

# check-all split into groups that make can run concurrently: the tests that
# replay the schedules prepared by 100_prepare_consistency_test_cases depend
# on it, the others run alongside it. The groups share tmp_check, which is
# cleaned once up front, and write their own JUnit files.
DC_BASIC_TESTS = $(patsubst $(srcdir)/%,%,$(wildcard $(srcdir)/t/0*.pl))
DC_PREPARE_TESTS = t/100_prepare_consistency_test_cases.pl
DC_REPLAY_TESTS = $(filter-out $(DC_PREPARE_TESTS),\
	$(patsubst $(srcdir)/%,%,$(wildcard $(srcdir)/t/1*.pl)))

define dc_prove_group
echo "# +++ tap check in $(subdir) ($(1)) +++" && \
cd $(srcdir) && \
   export with_pgbench=1 DCCHECK_ALL=1 JUNIT_OUTPUT_FILE=junit_output_$(1).xml && \
   TESTLOGDIR='$(CURDIR)/tmp_check/log' \
   TESTDATADIR='$(CURDIR)/tmp_check' \
   $(with_temp_install) \
   PGPORT='6$(DEF_PGPORT)' top_builddir='$(CURDIR)/$(top_builddir)' \
   PG_REGRESS='$(CURDIR)/$(top_builddir)/src/test/regress/pg_regress' \
   $(PROVE) $(PG_PROVE_FLAGS) $(PROVE_FLAGS) $(2)
endef

check-all-parallel: check-all-basic check-all-replay

check-all-clean: | temp-install
	rm -rf '$(CURDIR)'/tmp_check
	$(MKDIR_P) '$(CURDIR)'/tmp_check

check-all-basic: check-all-clean
	$(call dc_prove_group,basic,$(DC_BASIC_TESTS))

check-all-prepare: check-all-clean
	$(call dc_prove_group,prepare,$(DC_PREPARE_TESTS))

check-all-replay: check-all-prepare
	$(call dc_prove_group,replay,$(DC_REPLAY_TESTS))

else

check: | temp-install
	$(prove_check)

check-all: | temp-install
	$(prove_check)

installcheck:
	$(prove_installcheck)

installcheck-all: all
	$(prove_installcheck)

check-all-parallel: check-all

endif

.PHONY: check-all-parallel check-all-clean check-all-basic check-all-prepare \
	check-all-replay

clean:
	rm -rf tmp_check

distclean maintainer-clean:
	rm -rf tmp_check
	rm -f dccheck_schedule_list
