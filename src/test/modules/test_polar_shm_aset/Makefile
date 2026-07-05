# src/test/modules/test_polar_shm_aset/Makefile

MODULE_big = test_polar_shm_aset
OBJS = test_polar_shm_aset.o $(WIN32RES)
PGFILEDESC = "test_polar_shm_aset - test AllocSet based on dsa."

EXTENSION = test_polar_shm_aset
DATA = test_polar_shm_aset--1.0.sql
TEMP_CONFIG = "test_polar_shm_aset.conf"
REGRESS = test_polar_shm_aset

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_polar_shm_aset
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
