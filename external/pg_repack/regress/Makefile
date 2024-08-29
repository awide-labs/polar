#
# pg_repack: regress/Makefile
#
#  Portions Copyright (c) 2008-2012, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
#  Portions Copyright (c) 2011, Itagaki Takahiro
#  Portions Copyright (c) 2012-2020, The Reorg Development Team
#


#
# Test suite
#

# POLAR: no tablespace test now
REGRESS := init-extension repack-setup repack-run error-on-invalid-idx after-schema repack-check \
	nosuper get_order_by trigger polar_temp_table polar_declarative_partitioned_table \
	polar_forbidden_options polar_dc_check polar_statistics polar_repack_drop

EXTRA_INSTALL = external/pg_repack

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/pg_repack/regress/
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

PATH := $(abs_top_builddir)/tmp_install$(bindir):$(CURDIR)/../bin:$(PATH)
# POLAR: pg_repack does not support USE_MODULE_DB, only support contrib_regression_pg_repack
REGRESS_OPTS = --dbname=contrib_regression_pg_repack --temp-config=pg_repack.conf
