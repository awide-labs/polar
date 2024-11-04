# src/test/modules/test_bulkio/Makefile

MODULE_big = test_bulkio
OBJS = test_bulkio.o $(WIN32RES)
PGFILEDESC = "test_bulkio - test code for bulk IO interface"

EXTENSION = test_bulkio
DATA = test_bulkio--1.0.sql
REGRESS = test_bulkio

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_bulkio
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
