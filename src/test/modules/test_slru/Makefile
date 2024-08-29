# src/test/modules/test_slru/Makefile

MODULE_big = test_slru
OBJS = test_slru.o $(WIN32RES)
PGFILEDESC = "test_slru - test code for slru"

EXTENSION = test_slru
DATA = test_slru--1.0.sql

TEMP_CONFIG = "test_slru.conf"
REGRESS = test_slru

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_slru
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
