# external/polar_io_stat/Makefile

MODULE_big = polar_io_stat
OBJS = polar_io_stat.o
EXTENSION = polar_io_stat
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
