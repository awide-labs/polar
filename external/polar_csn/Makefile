# contrib/polar_worker/Makefile

MODULE_big = polar_csn
OBJS = polar_csn.o

EXTENSION = polar_csn
DATA = polar_csn--1.0.sql
REGRESS = polar_csn
TEMP_CONFIG = "polar_csn.conf"
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_csn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
