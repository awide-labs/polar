# src/test/modules/test_xlog_buffer/Makefile

MODULE_big = test_xlog_buffer
OBJS = test_xlog_buffer.o $(WIN32RES)
PGFILEDESC = "test_xlog_buffer - test code for xlog buffer"

EXTENSION = test_xlog_buffer
DATA = test_xlog_buffer--1.0.sql
TEMP_CONFIG = "test_xlog_buffer.conf"
REGRESS = test_xlog_buffer
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_xlog_buffer
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
