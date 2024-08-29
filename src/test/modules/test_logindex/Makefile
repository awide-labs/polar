# src/test/modules/test_logindex/Makefile

MODULE_big = test_logindex
OBJS = test_module_init.o test_bitpos.o test_ringbuf.o test_mini_trans.o test_logindex.o \
	  test_fullpage.o test_polar_rel_size_cache.o test_checkpoint_ringbuf.o $(WIN32RES)
PGFILEDESC = "test_logindex - test code for log index library"

EXTENSION = test_logindex
DATA = test_logindex--1.0.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_logindex
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

