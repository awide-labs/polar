# src/test/modules/test_coredump_handler/Makefile

MODULE_big = test_coredump_handler
OBJS = test_coredump_handler.o $(WIN32RES)
PGFILEDESC = "test_coredump_handler - test code for coredump handler"

EXTENSION = test_coredump_handler
DATA = test_coredump_handler--1.0.sql
TAP_TESTS = 1

export with_libunwind

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_coredump_handler
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
