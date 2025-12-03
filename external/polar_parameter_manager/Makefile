# external/polar_parameter_manager/Makefile

MODULE_big = polar_parameter_manager
OBJS = polar_parameter_manager.o

EXTENSION = polar_parameter_manager
DATA = polar_parameter_manager--1.1.sql
REGRESS = polar_parameter_manager polar_alter_force polar_cluster_settings

# Apply locale and encoding settings only for "make check"
ifeq ($(MAKECMDGOALS),check)
NO_LOCALE = 1

check: check-locale-message

check-locale-message:
	@echo "=================================================================="
	@echo " Running polar_parameter_manager tests with collation and ctype: C"
	@echo "=================================================================="
endif

NO_INSTALLCHECK = 1

TAP_TESTS = 1

EXTRA_INSTALL = external/faultinjector
export enable_fault_injector

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
top_builddir = ../..
subdir = external/polar_parameter_manager
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
