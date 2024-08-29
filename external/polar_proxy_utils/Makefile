# external/polar_proxy/Makefile

PGFILEDESC = "polar_proxy_utils - polar proxy related operation"

EXTENSION = polar_proxy_utils
DATA = polar_proxy_utils--1.0.sql
REGRESS = polar_proxy_utils

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_proxy_utils
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
