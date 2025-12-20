# src/test/modules/test_wal_pipeline/Makefile

MODULE_big = test_wal_pipeline
OBJS = test_wal_pipeline.o $(WIN32RES)
PGFILEDESC = "test_wal_pipeline - test code for polar wal pipeline"

EXTENSION = test_wal_pipeline
DATA = test_wal_pipeline--1.0.sql
TEMP_CONFIG = "test_wal_pipeline.conf"
REGRESS = test_wal_pipeline

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_wal_pipeline
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif