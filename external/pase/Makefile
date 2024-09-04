EXTENSION = pase
OBJS = $(patsubst %.c,%.o,$(wildcard type/*.c hnsw/*.c *.c ivfflat/*.c utils/*.c))

MODULE_big = $(EXTENSION)
PG_LDFLAGS = -fopenmp
PG_CPPFLAGS = -fopenmp -Wno-uninitialized
PG_LIBS = -fopenmp
DATA = pase--0.0.1.sql
PGFILEDESC = "pase - ai similarity search"
REGRESS = $(EXTENSION)

ifeq ($(GCC), yes)
override CFLAGS := -ftree-coalesce-vars
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/pase
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
# pase is compatible with the arm architecture, 
# and is compatible by using sse under different architectures
ifeq ($(host_cpu), x86_64)
PG_LDFLAGS += -msse4 -mpopcnt
PG_CPPFLAGS += -msse4 -mpopcnt -fcf-protection=none  -I/usr/lib64/openmpi/share/vampirtrace
endif
include $(top_srcdir)/contrib/contrib-global.mk
endif

perl-check:
	$(prove_check)
