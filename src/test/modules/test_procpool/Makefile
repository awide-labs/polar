# src/test/modules/test_procpool/Makefile

MODULE_big = test_procpool
OBJS = test_procpool.o $(WIN32RES)

PGFILEDESC = "test_procpool - test code for process pool"

EXTENSION = test_procpool
DATA = test_procpool--1.0.sql

TEMP_CONFIG = "test_procpool.conf"
REGRESS = test_procpool

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_procpool
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
