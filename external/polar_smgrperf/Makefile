# external/polar_smgrperf/Makefile

MODULE_big = polar_smgrperf
OBJS = polar_smgrperf.o $(WIN32RES)

EXTENSION = polar_smgrperf
DATA = polar_smgrperf--1.0.sql
PGFILEDESC = "polar_smgrperf - perf test on smgr"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_smgrperf
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
