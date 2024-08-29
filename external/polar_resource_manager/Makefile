# external/polar_resource_manager/Makefile

MODULE_big = polar_resource_manager
OBJS = polar_resource_manager.o throttle_mem.o

EXTENSION = polar_resource_manager
PGFILEDESC = "polar_resource_manager - a background process that forcibly frees user session process memory"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_resource_manager
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
