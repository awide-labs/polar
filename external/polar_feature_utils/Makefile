MODULE_big = polar_feature_utils
OBJS = polar_feature_utils.o $(WIN32RES)

EXTENSION    = polar_feature_utils
DATA = ${EXTENSION}--1.0.sql
PGFILEDESC = "polar_feature_utils - Track the feature utilization of PolarDB"
REGRESS = polar_feature_utils

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_feature_utils
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
