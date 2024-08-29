EXTENSION = roaringbitmap
TESTS        = $(wildcard sql/*.sql)
REGRESS      = $(patsubst sql/%.sql,%,$(TESTS))

MODULE_big = roaringbitmap
OBJS = roaringbitmap.o
DATA = $(wildcard *--*.sql)

roaringbitmap.o: override CFLAGS += -std=c99 -Wno-error=maybe-uninitialized \
	-Wno-declaration-after-statement -Wno-missing-prototypes

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/roaringbitmap
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
