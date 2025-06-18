# contrib/plaudit/Makefile

MODULE_big = plaudit
OBJS = plaudit.o $(WIN32RES)

EXTENSION = plaudit
DATA = plaudit--1.0.sql
PGFILEDESC = "plaudit - An audit logging extension for PostgreSQL"
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/plaudit
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
