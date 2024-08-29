# src/test/modules/test_polar_directio/Makefile

MODULE_big = test_directio
OBJS = test_directio.o $(WIN32RES)
PGFILEDESC = "test_directio - test code for polar (p)read or (p)write with PG_O_DIRECT feature"

EXTENSION = test_directio
DATA = test_directio--1.0.sql
REGRESS = test_directio

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_polar_directio
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
