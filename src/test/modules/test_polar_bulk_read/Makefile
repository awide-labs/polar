# src/test/modules/test_polar_bulk_read/Makefile

MODULE_big = test_polar_bulk_read
OBJS = test_polar_bulk_read.o $(WIN32RES)
PGFILEDESC = "test_polar_bulk_read - test code for bulk read"

EXTENSION = test_polar_bulk_read
DATA = test_polar_bulk_read--1.0.sql

REGRESS = test_polar_bulk_read test_polar_temp_table_bulk_read

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_polar_bulk_read
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
