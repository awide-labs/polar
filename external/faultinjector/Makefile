MODULES = faultinjector

EXTENSION = faultinjector
DATA = faultinjector--1.0.sql

REGRESS = faultinjector_test

# POLAR: faultinjector may cause the database shutdown and make other parallel regress cases failed
# so only 'make check' permitted for faultinjector
NO_INSTALLCHECK = 1

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LDFLAGS = $(libpq)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
SHLIB_PREREQS = submake-libpq
subdir = external/faultinjector
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
