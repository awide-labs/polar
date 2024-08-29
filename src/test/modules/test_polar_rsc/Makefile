# src/test/modules/test_polar_rsc/Makefile

MODULE_big = test_polar_rsc
OBJS = test_polar_rsc.o $(WIN32RES)
PGFILEDESC = "test_polar_rsc - test code for PolarDB-PG relation size cache"

EXTENSION = test_polar_rsc
DATA = test_polar_rsc--1.0.sql
REGRESS = test_polar_rsc
TEMP_CONFIG = "test_polar_rsc.conf"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_polar_rsc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
