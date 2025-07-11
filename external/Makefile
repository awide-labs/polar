# external/Makefile

subdir = external
top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS =

# multi-arch/0-dependency/fast-compile extensions can be added here
# sort extention by names, less git conflict
SUBDIRS += polar_audit
SUBDIRS += polar_feature_utils
SUBDIRS += polar_io_stat
SUBDIRS += polar_login_history
SUBDIRS += polar_monitor
SUBDIRS += polar_monitor_preload
SUBDIRS += polar_parameter_manager
SUBDIRS += polar_password_policy
SUBDIRS += polar_proxy_utils
SUBDIRS += polar_resource_manager
SUBDIRS += polar_smgrperf
SUBDIRS += polar_stat_env
SUBDIRS += polar_worker

ifeq ($(enable_minimal),no)
# will be ignored under minimal mode
# sort extention by names, less git conflict
SUBDIRS += hll
SUBDIRS += hypopg
SUBDIRS += log_fdw
SUBDIRS += pase
SUBDIRS += pg_bigm
SUBDIRS += pg_jieba
SUBDIRS += pg_repack
SUBDIRS += pgvector
SUBDIRS += polar_sql_mapping
SUBDIRS += roaringbitmap
endif # enable_minimal

ifeq ($(enable_fault_injector),yes)
SUBDIRS += faultinjector
else
ALWAYS_SUBDIRS += faultinjector
endif

$(recurse)
$(recurse_always)
