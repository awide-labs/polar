# src/test/modules/xact/Makefile

MODULE_big	= test_csn_xact
OBJS		= test_csn_xact.o

EXTENSION = test_csn_xact
DATA = test_csn_xact--1.0.sql

REGRESS = test_csn_xact
TEMP_CONFIG = csn_xact.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_csn_xact
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
