# external/polar_monitor/Makefile

MODULE_big = polar_monitor
OBJS = polar_monitor.o polar_monitor_buf.o procstat.o polar_monitor_io.o polar_monitor_pfs.o $(WIN32RES)
OBJS += polar_monitor_proxy.o
OBJS += polar_monitor_rsc.o
OBJS += polar_monitor_logindex.o polar_monitor_xlogbuf.o
OBJS += polar_monitor_async_lock_replay.o

EXTENSION = polar_monitor
DATA = polar_monitor--1.0.sql
PGFILEDESC = "polar_monitor - monitoring of PolarDB information in real-time"
REGRESS = polar_monitor
REGRESS_OPTS = --temp-config polar_monitor.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_monitor
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
