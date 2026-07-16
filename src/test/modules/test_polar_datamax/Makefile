# src/test/modules/test_polar_datamax/Makefile

MODULE_big = test_polar_datamax
OBJS = test_polar_datamax.o $(WIN32RES)
PGFILEDESC = "test_polar_datamax - test code for polar datamax feature"

EXTENSION = test_polar_datamax
DATA = test_polar_datamax--1.0.sql
TEMP_CONFIG = "test_polar_datamax.conf"
REGRESS = test_polar_datamax

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_polar_datamax
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
