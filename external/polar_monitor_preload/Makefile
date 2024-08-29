# external/polar_monitor/Makefile

MODULE_big = polar_monitor_preload
OBJS = polar_monitor_preload.o polar_monitor_mcxt.o polar_monitor_lock.o polar_monitor_network.o $(WIN32RES)

EXTENSION = polar_monitor_preload
DATA = $(wildcard *--*.sql)
PGFILEDESC = "polar_monitor - monitoring of PolarDB information in real-time for preload"
REGRESS = polar_monitor_preload
REGRESS_OPTS = --temp-config polar_monitor_preload.conf
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
top_builddir = ../..
subdir = external/polar_monitor_preload
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
