# src/test/modules/test_px/Makefile
MODULE_big = test_px
OBJS = test_px.o \
	test_px_gpopt.o \
	test_px_gpopt_util.o \
	test_px_partition.o  $(WIN32RES)
PG_CXXFLAGS = \
			  -I$(top_srcdir)/src/backend/px_optimizer/libnaucrates/include \
			  -I$(top_srcdir)/src/backend/px_optimizer/libgpos/include \
			  -std=c++11
# C++ objects are part of MODULE_big; link C++ runtime explicitly.
SHLIB_LINK += -lstdc++
PGFILEDESC = "test_px - test code for Parallel Execution library"

EXTENSION = test_px
DATA = test_px--1.0.sql
TEMP_CONFIG = "test_px.conf"
# Keep this module planner-focused:
# - test_px_optimizer: optimizer/planner API and GUC matrix
# - test_px_partition: partition metadata + EXPLAIN planner output
PX_REGRESS = test_px_optimizer test_px_partition

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_px
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

ifeq ($(enable_fault_injector),yes)
EXTRA_INSTALL += external/faultinjector
PX_REGRESS += test_px_inject_error
endif

#Override COMPILE.cxx.bc BITCODE_CXXFLAGS to prevent errors of llvm compiler,
#inspired from gporca.mk, seems there may be a better approach
override BITCODE_CXXFLAGS += $(PG_CXXFLAGS)

polar-px-installcheck: submake $(REGRESS_PREP)
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(PX_REGRESS)

polar-px-check: checkprep run-temp-cluster
	$(with_temp_install) $(MAKE) bindir="$(temp_install_bindir)" PGHOST=$(temp_sock_dir) polar-px-installcheck && \
	$(with_temp_install) $(MAKE) DESTDIR=$(abs_top_builddir)/tmp_install uninstall ; \
	ERRCODE=$$? ; \
	$(MAKE) cleanup-temp-cluster && exit $$ERRCODE
