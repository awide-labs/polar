# external/polar_password_policy/Makefile

MODULE_big = polar_password_policy
OBJS = polar_password_policy.o

EXTENSION = polar_password_policy
DATA = polar_password_policy--1.0.sql

NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_password_policy
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
