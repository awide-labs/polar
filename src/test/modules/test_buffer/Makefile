# src/test/modules/test_buffer/Makefile

MODULE_big = test_buffer
OBJS = test_buffer.o $(WIN32RES)
PGFILEDESC = "test_buffer - test code for buffer manager library"

EXTENSION = test_buffer
DATA = test_buffer--1.0.sql
REGRESS = test_buffer

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_buffer
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
