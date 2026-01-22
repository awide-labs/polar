# src/test/modules/test_csn/Makefile

MODULE_big = test_csn
OBJS = test_csn.o $(WIN32RES)
PGFILEDESC = "test_csn - test code for polar csn"

EXTENSION = test_csn
DATA = test_csn--1.0.sql
TEMP_CONFIG = "test_csn.conf"
REGRESS = test_csn

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_csn
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif