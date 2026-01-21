# external/Makefile

subdir = external
top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS =

# multi-arch/0-dependency/fast-compile extensions can be added here
# sort extention by names, less git conflict
SUBDIRS += pg_cron_preload
SUBDIRS += pg_hint_plan
SUBDIRS += polar_csn
SUBDIRS += polar_feature_utils
SUBDIRS += polar_io_stat
SUBDIRS += polar_monitor
SUBDIRS += polar_monitor_preload
SUBDIRS += polar_parameter_manager
SUBDIRS += polar_proxy_utils
SUBDIRS += polar_resource_manager
SUBDIRS += polar_smgrperf
SUBDIRS += polar_worker

ifeq ($(enable_minimal),no)
# will be ignored under minimal mode
# sort extention by names, less git conflict
#SUBDIRS += hll
#SUBDIRS += ip4r
#SUBDIRS += log_fdw
#SUBDIRS += pase
#SUBDIRS += pg_bigm
SUBDIRS += pg_cron
#SUBDIRS += pg_jieba
#SUBDIRS += pg_partman
SUBDIRS += pg_profile
#SUBDIRS += pg_repack
SUBDIRS += pg_similarity
SUBDIRS += pg_squeeze
SUBDIRS += pg_stat_kcache
SUBDIRS += pgaudit
#SUBDIRS += pgtap
SUBDIRS += pldebugger
SUBDIRS += polar_advisor
SUBDIRS += prefix
#SUBDIRS += roaringbitmap
SUBDIRS += sequential_uuids
SUBDIRS += varbitx
endif # enable_minimal

ifeq ($(with_tde),yes)
SUBDIRS += polar_tde_utils
else
ALWAYS_SUBDIRS += polar_tde_utils
endif # with_tde

ifeq ($(enable_fault_injector),yes)
SUBDIRS += faultinjector
else
ALWAYS_SUBDIRS += faultinjector
endif

$(recurse)
$(recurse_always)
