MODULE_big = polar_px
OBJS = polar_px.o $(WIN32RES)

EXTENSION    = polar_px
DATA = polar_px--1.0.sql
PGFILEDESC = "polar_px - parallel execution extension"
REGRESS = polar_px

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_px
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
