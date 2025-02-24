# external/polar_login_history/Makefile

MODULE_big = polar_login_history
OBJS = polar_login_history.o
EXTENSION = polar_login_history
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_login_history
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

check:
	$(prove_check)
