# contrib/polar_worker/Makefile

MODULE_big = polar_worker
OBJS = polar_worker.o test_read_core_pattern.o

EXTENSION = polar_worker
DATA = polar_worker--1.0.sql
REGRESS = polar_worker
TEMP_CONFIG = polar_worker.conf

LDFLAGS_SL += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_worker
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
