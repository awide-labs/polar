# external/polar_monitor/Makefile

MODULE_big = polar_monitor
OBJS = polar_monitor.o polar_monitor_buf.o $(WIN32RES)
OBJS += polar_monitor_async_lock_replay.o
OBJS += procstat.o polar_monitor_io.o polar_monitor_pfs.o
OBJS += polar_monitor_csnlog.o
OBJS += polar_monitor_logindex.o
OBJS += polar_monitor_proxy.o
OBJS += polar_monitor_rsc.o
OBJS += polar_monitor_xlogbuf.o
OBJS += polar_monitor_wal_buffer.o
OBJS += polar_monitor_walsnd.o

EXTENSION = polar_monitor
DATA = polar_monitor--1.0.sql \
	polar_monitor--1.0--1.1.sql \
	polar_monitor--1.1--1.2.sql \
	polar_monitor--1.2--1.3.sql
PGFILEDESC = "polar_monitor - monitoring of PolarDB information in real-time"
REGRESS = polar_monitor
POLAR_NO_EXTENSION_USER = 1

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
