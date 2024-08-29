# src/test/modules/test_local_cache/Makefile

MODULE_big = test_local_cache
OBJS = test_local_cache.o $(WIN32RES)
PGFILEDESC = "test_local_cache - test code for local_cache"

EXTENSION = test_local_cache
DATA = test_local_cache--1.0.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_local_cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
