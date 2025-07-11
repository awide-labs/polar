# external/polar_audit/Makefile

MODULE_big = polar_audit
OBJS = polar_audit.o $(WIN32RES)

EXTENSION = polar_audit
DATA = polar_audit--1.0.sql
PGFILEDESC = "polar_audit - An audit logging extension for PostgreSQL"
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_audit
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
