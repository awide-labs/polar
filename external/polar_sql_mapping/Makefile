# external/polar_sql_mapping/MakeFile

MODULE_big = polar_sql_mapping
OBJS = polar_sql_mapping.o polar_error_detective.o

EXTENSION = polar_sql_mapping
DATA = polar_sql_mapping--1.0.sql
PGFILEDESC = "polar_sql_mapping - plugin to change error sql"
ifndef MXSCHECK
REGRESS = polar_sql_mapping
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_sql_mapping
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif