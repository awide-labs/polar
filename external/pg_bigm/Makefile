MODULE_big = pg_bigm
OBJS = bigm_op.o bigm_gin.o

EXTENSION = pg_bigm
DATA = pg_bigm--1.2.sql
PGFILEDESC = "pg_bigm - bigram matching"

REGRESS = pg_bigm pg_bigm_ja
ORA_REGRESS = pg_bigm pg_bigm_ja

# Apply locale and encoding settings only for "make check"
ifeq ($(MAKECMDGOALS),check)
# We need a UTF8 database
ENCODING = UTF8
NO_LOCALE = 1

check: check-locale-message

check-locale-message:
	@echo "===================================================="
	@echo " Running pg_bigm tests with encoding: UTF-8"
	@echo " Database encoding: UTF8"
	@echo "===================================================="
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/pg_bigm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
