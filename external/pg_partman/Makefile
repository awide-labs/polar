EXTENSION = pg_partman
EXTVERSION = $(shell grep default_version $(EXTENSION).control | \
               sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DOCS = $(wildcard doc/*.md)
MODULES = src/pg_partman_bgw

# The suites in test/ are pgTAP scripts that produce their own TAP output, so
# there is no expected/ output for pg_regress to compare.
TAP_TESTS = 1

# If user does not want the background worker, run: make NO_BGW=1
ifneq ($(NO_BGW),)
	MODULES=
endif

.PHONY: all
all: sql/$(EXTENSION)--$(EXTVERSION).sql

SCRIPTS = bin/common/*.py

sql/$(EXTENSION)--$(EXTVERSION).sql: $(wildcard sql/types/*.sql sql/tables/*.sql sql/functions/*.sql sql/procedures/*.sql)
	cat $^ > $@

DATA = $(wildcard updates/*--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/pg_partman
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
PG_CONFIG = $(DESTDIR)$(bindir)/pg_config
endif

.PHONY: lint # Lint the project
lint: .pre-commit-config.yaml
	@pre-commit run --show-diff-on-failure --color=always --all-files

## .git/hooks/pre-commit: Install the pre-commit hook
.git/hooks/pre-commit:
	@printf "#!/bin/sh\nmake lint\n" > $@
	@chmod +x $@
