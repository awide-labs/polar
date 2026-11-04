/*--------------------------------------------------------------------
 * guc_px.c
 *
 * Additional PolarDB-PX-specific GUCs are defined in this file, to
 * avoid adding so much stuff to guc.c. This makes it easier to diff
 * and merge with upstream.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc_px.c
 *
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "utils/builtins.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/varlena.h"

/* POLAR px */
#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "postmaster/syslogger.h"
#include "px/px_disp.h"
#include "px/px_util.h"
#include "px/px_vars.h"
#include "replication/walsender.h"
#include "utils/syscache.h"

bool		px_test_print_direct_dispatch_info = false;
bool		px_debug_cancel_print = false;
bool		px_log_dispatch_stats = false;
bool		px_enable_hashjoin_size_heuristic = false;
bool		px_enable_predicate_propagation = false;
bool		px_enable_minmax_optimization = true;
bool		px_enable_multiphase_agg = true;
bool		px_enable_preunique = true;
bool		px_enable_print = false;
bool		px_eager_preunique = false;
bool		px_hashagg_streambottom = true;
bool		px_enable_agg_distinct = true;
bool		px_enable_dqa_pruning = true;
bool		px_eager_dqa_pruning = false;
bool		px_eager_one_phase_agg = false;
bool		px_eager_two_phase_agg = false;
bool		px_cte_sharing = false;
bool		px_optimizer_enable_relsize_collection = false;
int			px_explain_memory_verbosity = 0;

/* Optimizer related gucs */
bool		polar_enable_px;
bool		px_enable_executor;
bool		px_enable_join;
bool		px_enable_window_function;
bool		px_enable_subquery;
bool		px_enable_cte;
bool		px_enable_partition;
bool		px_enable_partition_hash;
bool		px_enable_relsize_collection;
bool		px_enable_adaptive_scan;
bool		px_enable_adps_explain_analyze;
int			px_dop_per_node = 1;
bool		px_optimizer_log;
int			px_optimizer_log_failure;
bool		px_optimizer_trace_fallback;

bool		px_optimizer_partition_selection_log;
int			px_optimizer_minidump;
int			px_optimizer_cost_model = OPTIMIZER_GPDB_CALIBRATED;
bool		px_optimizer_metadata_caching;
int			px_optimizer_mdcache_size;
bool		px_optimizer_use_px_allocators;
bool		px_enable_opfamily_for_distribution;
bool		px_enable_spi_read_all_namespaces;

/* Optimizer debugging GUCs */
bool		px_optimizer_print_query;
bool		px_optimizer_print_plan;
bool		px_optimizer_print_xform;
bool		px_optimizer_print_memo_after_exploration;
bool		px_optimizer_print_memo_after_implementation;
bool		px_optimizer_print_memo_after_optimization;
bool		px_optimizer_print_job_scheduler;
bool		px_optimizer_print_expression_properties;
bool		px_optimizer_print_group_properties;
bool		px_optimizer_print_optimization_context;
bool		px_optimizer_print_optimization_stats;
bool		px_optimizer_print_xform_results;
bool		px_optimizer_print_memo_enforcement;
bool		px_optimizer_print_required_columns;
bool		px_optimizer_print_equiv_distr_specs;


/* Optimizer Parallel DML */
bool		px_enable_insert_select;
bool		px_enable_insert_partition_table;
int			px_insert_dop_num;
bool		px_enable_insert_from_tableless;
bool		px_enable_insert_order_sensitive;
bool		px_enable_update;
int			px_update_dop_num;
bool		px_enable_delete;
int			px_delete_dop_num;
bool		px_optimizer_remove_superfluous_order;



/* array of xforms disable flags */
bool		optimizer_xforms[OPTIMIZER_XFORMS_COUNT] = {false};
char	   *px_optimizer_search_strategy_path = NULL;

/* GUCs to tell Optimizer to enable a physical operator */
bool		px_optimizer_enable_indexjoin;
bool		px_optimizer_enable_motions_masteronly_queries;
bool		px_optimizer_enable_motions;
bool		px_optimizer_enable_motion_broadcast;
bool		px_optimizer_enable_motion_gather;
bool		px_optimizer_enable_motion_redistribute;
bool		px_optimizer_enable_sort;
bool		px_optimizer_enable_materialize;
bool		px_optimizer_enable_partition_propagation;
bool		px_optimizer_enable_partition_selection;
bool		px_optimizer_enable_outerjoin_rewrite;
bool		px_optimizer_enable_multiple_distinct_aggs;
bool		px_optimizer_enable_direct_dispatch;
bool		px_optimizer_enable_hashjoin_redistribute_broadcast_children;
bool		px_optimizer_enable_broadcast_nestloop_outer_child;
bool		px_optimizer_enable_streaming_material;
bool		px_optimizer_enable_gather_on_segment_for_dml;
bool		px_optimizer_enable_assert_maxonerow;
bool		px_optimizer_enable_constant_expression_evaluation;
bool		px_optimizer_enable_outerjoin_to_unionall_rewrite;
bool		px_optimizer_enable_ctas;
bool		px_optimizer_enable_dml;
bool		px_optimizer_enable_dml_triggers;
bool		px_optimizer_enable_dml_constraints;
bool		px_optimizer_enable_master_only_queries;
bool		px_optimizer_enable_hashjoin;
bool		px_optimizer_enable_dynamictablescan;
bool		px_optimizer_enable_dynamicindexscan;
bool		px_optimizer_enable_hashagg;
bool		px_optimizer_enable_groupagg;
bool		px_optimizer_expand_fulljoin;
bool		px_optimizer_enable_mergejoin;
bool		px_optimizer_prune_unused_columns;
bool		px_optimizer_enable_seqscan;
bool		px_optimizer_enable_seqsharescan;
bool		px_optimizer_enable_indexscan;
bool		px_optimizer_enable_indexonlyscan;
bool		px_optimizer_enable_shareindexscan;
bool		px_optimizer_enable_dynamicshareindexscan;
bool		px_optimizer_enable_brinscan;
bool		px_optimizer_enable_bitmapscan;
bool		px_optimizer_enable_nestloopjoin;
bool		px_optimizer_enable_lasj_notin;
bool		px_optimizer_enable_crossproduct;
bool		px_enable_partitionwise_join;

/* Optimizer plan enumeration related GUCs */
bool		px_optimizer_enumerate_plans;
bool		px_optimizer_sample_plans;
int			px_optimizer_plan_id;
int			px_optimizer_samples_number;

/* Cardinality estimation related GUCs used by the Optimizer */
bool		px_optimizer_extract_dxl_stats;
bool		px_optimizer_extract_dxl_stats_all_nodes;
bool		px_optimizer_print_missing_stats;
double		px_optimizer_damping_factor_filter;
double		px_optimizer_damping_factor_join;
double		px_optimizer_damping_factor_groupby;
bool		px_optimizer_dpe_stats;
bool		px_optimizer_enable_derive_stats_all_groups;

/* Costing related GUCs used by the Optimizer */
int			px_optimizer_segments;
int			px_optimizer_penalize_broadcast_threshold;
double		px_optimizer_cost_threshold;
double		px_optimizer_nestloop_factor;
double		px_optimizer_sort_factor;
double		px_optimizer_share_tablescan_factor;
double		px_optimizer_share_indexscan_factor;

/* Optimizer hints */
int			px_optimizer_join_arity_for_associativity_commutativity;
int			px_optimizer_array_expansion_threshold;
int			px_optimizer_join_order_threshold;
int			px_optimizer_join_order;
int			px_optimizer_cte_inlining_bound;
int			px_optimizer_push_group_by_below_setop_threshold;
bool		px_optimizer_force_multistage_agg;
bool		px_optimizer_force_three_stage_scalar_dqa;
bool		px_optimizer_force_expanded_distinct_aggs;
bool		px_optimizer_force_agg_skew_avoidance;
bool		px_optimizer_penalize_skew;
bool		px_optimizer_prune_computed_columns;
bool		px_optimizer_push_requirements_from_consumer_to_producer;
bool		px_optimizer_enforce_subplans;
bool		px_optimizer_use_external_constant_expression_evaluation_for_ints;
bool		px_optimizer_apply_left_outer_to_union_all_disregarding_stats;
bool		px_optimizer_remove_order_below_dml;
bool		px_optimizer_multilevel_partitioning;
bool		px_optimizer_parallel_union;
bool		px_optimizer_array_constraints;
bool		px_optimizer_cte_inlining;
bool		px_optimizer_enable_space_pruning;
bool		px_optimizer_enable_associativity;
bool		px_optimizer_enable_eageragg;

/* POLAR */
char	   *polar_px_version;
bool		px_enable_check_workers = true;
bool		px_info_debug;
bool		px_enable_replay_wait;
bool		px_enable_transaction;
bool		px_enable_sort_distinct = false;
bool		px_enable_join_prefetch_inner;
int			px_max_workers_number;
char	   *polar_px_ignore_function = NULL;
PxFunctionOidArray *px_function_oid_array = NULL;
bool		px_use_master;
bool		px_use_standby;
bool		polar_px_ignore_unusable_nodes;
char	   *px_peer_cert_file;
char	   *px_peer_key_file;
char	   *px_ca_file;
bool		px_enable_auth_on_qe;

/* Prepare statement, plpgsql function, procedure, csn */
bool		px_enable_plan_cache;
bool		px_enable_plpgsql;
bool		px_enable_procedure;
bool		px_enable_prepare_statement;

/* check csn */
bool		px_enable_check_csn;

/* Security */
bool		px_reject_internal_tcp_conn = true;

/* GUCs for slice table*/
int			px_max_slices;

bool		px_execute_pruned_plan = false;

/* The number of blocks to scan table */
int			px_scan_unit_size = 0;
int			px_scan_unit_bit = 0;

/* Default true. sysattr_len will include PxWorkerIdAttributeNumber */
bool		px_interconnect_udpic_network_enable_ipv6;
bool		px_enable_dispatch_async;
bool		px_enable_udp_testmode;
bool		px_enable_tcp_testmode;
bool		px_enable_remove_redundant_results;
bool		px_enable_btbuild;
bool		px_enable_btbuild_cic_phase2;
int			px_btbuild_batch_size;
int			px_btbuild_mem_size;
int			px_btbuild_queue_size;
bool		px_enable_cte_shared_scan;
bool		px_enable_tableless_scan;
bool		px_enable_pre_optimizer_check;
bool		px_enable_sethintbits;

bool		px_allow_strat_seqscan;
bool		px_allow_strat_bitmapscan;
bool		px_allow_sync_seqscan;
bool		px_allow_sync_bitmapscan;
bool		px_allow_pagemode_seqscan;
bool		px_allow_pagemode_bitmapscan;
bool		px_enable_left_index_nestloop_join;
bool		px_enable_result_hash_filter;
bool		px_enable_create_table_as;

int			px_wait_lock_timeout = 0;

/* POLAR */
static bool px_check_dispatch_log_stats(bool *newval, void **extra, GucSource source);
static bool px_check_scan_unit_size(int *newval, void **extra, GucSource source);
static bool px_check_ignore_function(char **newval, void **extra, GucSource source);
static bool px_check_tls_support(char **newval, void **extra, GucSource source);
static void px_assign_ignore_function(const char *newval, void *extra);
static void _px_need_reload_ca(const char *newval, void *extra);
static void _px_need_reload_cert_pkey(const char *newval, void *extra);
static int	px_guc_array_compare(const void *a, const void *b);

static const struct config_enum_entry px_interconnect_types[] = {
	{"udpifc", INTERCONNECT_TYPE_UDPIFC},
	{"tcp", INTERCONNECT_TYPE_TCP},
	{NULL, 0}
};

static const struct config_enum_entry px_interconnect_fc_methods[] = {
	{"loss", INTERCONNECT_FC_METHOD_LOSS},
	{"capacity", INTERCONNECT_FC_METHOD_CAPACITY},
	{NULL, 0}
};

static const struct config_enum_entry px_log_verbosity[] = {
	{"terse", PXVARS_VERBOSITY_TERSE},
	{"off", PXVARS_VERBOSITY_OFF},
	{"verbose", PXVARS_VERBOSITY_VERBOSE},
	{"debug", PXVARS_VERBOSITY_DEBUG},
	{NULL, 0}
};

static const struct config_enum_entry px_optimizer_cost_model_options[] = {
	{"calibrated", OPTIMIZER_GPDB_CALIBRATED},
	{"polardb", OPTIMIZER_POLARDB},
	{NULL, 0}
};

static const struct config_enum_entry px_optimizer_join_order_options[] = {
	{"query", JOIN_ORDER_IN_QUERY},
	{"greedy", JOIN_ORDER_GREEDY_SEARCH},
	{"exhaustive", JOIN_ORDER_EXHAUSTIVE_SEARCH},
	{"exhaustive2", JOIN_ORDER_EXHAUSTIVE2_SEARCH},
	{NULL, 0}
};

static const struct config_enum_entry px_optimizer_log_failure_options[] = {
	{"all", OPTIMIZER_ALL_FAIL},
	{"unexpected", OPTIMIZER_UNEXPECTED_FAIL},
	{"expected", OPTIMIZER_EXPECTED_FAIL},
	{NULL, 0}
};

static const struct config_enum_entry optimizer_minidump_options[] = {
	{"onerror", OPTIMIZER_MINIDUMP_FAIL},
	{"always", OPTIMIZER_MINIDUMP_ALWAYS},
	{NULL, 0}
};

static const struct config_enum_entry px_explain_memory_verbosity_options[] = {
	{"suppress", EXPLAIN_MEMORY_VERBOSITY_SUPPRESS},
	{"summary", EXPLAIN_MEMORY_VERBOSITY_SUMMARY},
	{"detail", EXPLAIN_MEMORY_VERBOSITY_DETAIL},
	{NULL, 0}
};

/*
 * Here and later we specify POLAR_GUC_IS_INVISIBLE option for DEVELOPER_OPTIONS
 * GUCs and POLAR_GUC_IS_UNCHANGABLE for static informative parameters having
 * PGC_INTERNAL, PGC_POSTMASTER, PGC_SU_BACKEND and PGC_BACKEND contexts.
 *
 * These options just affect to output of `polar_parameter.parameter_infos_memory`
 * view and derived `polar_parameter.parameter_infos`.
 *
 * TODO: consider to specify these options more intently depending on use cases of affected views
 */

struct config_bool ConfigureNamesBool_px[] =
{
	{
		{"polar_enable_px", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable polar_enable_px."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&polar_enable_px,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_executor", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable px_enable_executor."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_executor,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_full_crc", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sanity check incoming data stream."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_full_crc,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_log_stats", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Emit statistics from the UDP-IC at the end of every statement."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_log_stats,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_cache_future_packets", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Control whether future packets are cached."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_cache_future_packets,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_enable_aggressive_retry", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable application-level fast-track interconnect retries"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_NO_RESET_ALL | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_aggressive_retry,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_log", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Log optimizer messages."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_log,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_trace_fallback", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print a message at INFO level, whenever PXOPT falls back."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_trace_fallback,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_partition_selection_log", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Log optimizer partition selection."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_partition_selection_log,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_query", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Prints the optimizer's input query expression tree."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_query,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_plan", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Prints the plan expression tree produced by the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_plan,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_xform", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Prints optimizer transformation information."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_xform,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_metadata_caching", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("This guc enables the optimizer to cache and reuse metadata."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_metadata_caching,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_missing_stats", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print columns with missing statistics."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_missing_stats,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_xform_results", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print the input and output of optimizer transformations."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_xform_results,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_memo_after_exploration", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print optimizer memo structure after the exploration phase."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_memo_after_exploration,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_memo_after_implementation", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print optimizer memo structure after the implementation phase."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_memo_after_implementation,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_memo_after_optimization", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print optimizer memo structure after optimization."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_memo_after_optimization,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_job_scheduler", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print the jobs in the scheduler on each job completion."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_job_scheduler,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_expression_properties", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print expression properties."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_expression_properties,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_group_properties", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print group properties."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_group_properties,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_optimization_context", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print the optimization context."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_optimization_context,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_optimization_stats", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print optimization stats."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_optimization_stats,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_print_memo_enforcement", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print memo enforcement."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_memo_enforcement,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_print_required_columns", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print required columns."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_required_columns,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_print_equiv_distr_specs", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Print equiv distr specs."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_print_equiv_distr_specs,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_extract_dxl_stats", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Extract plan stats in dxl."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_extract_dxl_stats,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_extract_dxl_stats_all_nodes", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Extract plan stats for all physical dxl nodes."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_extract_dxl_stats_all_nodes,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_dpe_stats", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable statistics derivation for partitioned tables with dynamic partition elimination."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_dpe_stats,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_indexjoin", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable index nested loops join plans in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_indexjoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_motions_masteronly_queries", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Motion operators in the optimizer for queries with no distributed tables."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_motions_masteronly_queries,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_motions", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Motion operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_motions,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_motion_broadcast", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Motion Broadcast operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_motion_broadcast,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_motion_gather", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Motion Gather operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_motion_gather,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_motion_redistribute", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Motion Redistribute operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_motion_redistribute,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_sort", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Sort operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_sort,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_materialize", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Materialize operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_materialize,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_partition_propagation", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Partition Propagation operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_partition_propagation,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_partition_selection", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with Partition Selection operators in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_partition_selection,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_outerjoin_rewrite", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable outer join to inner join rewrite in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_outerjoin_rewrite,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_direct_dispatch", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable direct dispatch in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_direct_dispatch,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_space_pruning", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable space pruning in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_space_pruning,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_master_only_queries", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Process master only queries via the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_master_only_queries,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_hashjoin", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's use of hash join plans."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_hashjoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_dynamictablescan", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's use of plans with dynamic table scan."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_dynamictablescan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_dynamicindexscan", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's use of plans with dynamic index scan."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_dynamicindexscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_hashagg", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables PolarDB Parallel Optimizer to use hash aggregates."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_hashagg,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_groupagg", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables PolarDB Parallel Optimizer to use group aggregates."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_groupagg,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_force_agg_skew_avoidance", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Always pick a plan for aggregate distinct that minimizes skew."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_force_agg_skew_avoidance,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_penalize_skew", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Penalize operators with skewed hash redistribute below it."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_penalize_skew,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_multilevel_partitioning", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable optimization of queries on multilevel partitioned tables."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_multilevel_partitioning,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_derive_stats_all_groups", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable stats derivation for all groups after exploration."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_derive_stats_all_groups,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_force_multistage_agg", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Force optimizer to always pick multistage aggregates when such a plan alternative is generated."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_force_multistage_agg,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_multiple_distinct_aggs", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with multiple distinct aggregates in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_multiple_distinct_aggs,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_force_expanded_distinct_aggs", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Always pick plans that expand multiple distinct aggregates into join of single distinct aggregate in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_force_expanded_distinct_aggs,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_prune_computed_columns", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Prune unused computed columns when pre-processing query"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_prune_computed_columns,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_push_requirements_from_consumer_to_producer", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Optimize CTE producer plan on requirements enforced on top of CTE consumer in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_push_requirements_from_consumer_to_producer,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_seqscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of sequential-scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_seqscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_seqsharescan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of sequential-share-scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_seqsharescan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_indexscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of index-scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_indexscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_shareindexscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of share index scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_shareindexscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_dynamicshareindexscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of partition share index scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_dynamicshareindexscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_indexonlyscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of indexonly-scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_indexonlyscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_brinscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of brin index-scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_brinscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_bitmapscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of bitmap-scan plans."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_bitmapscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_hashjoin_redistribute_broadcast_children", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable hash join plans with, Redistribute outer child and Broadcast inner child, in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_hashjoin_redistribute_broadcast_children,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_broadcast_nestloop_outer_child", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable nested loops join plans with replicated outer child in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_broadcast_nestloop_outer_child,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_expand_fulljoin", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's support of expanding full outer joins using union all."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_expand_fulljoin,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_mergejoin", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's support of merge joins."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_mergejoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_streaming_material", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plans with a streaming material node in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_streaming_material,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_gather_on_segment_for_dml", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable DML optimization by enforcing a non-master gather in the optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_gather_on_segment_for_dml,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enforce_subplans", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enforce correlated execution in the optimizer"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enforce_subplans,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_assert_maxonerow", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable Assert MaxOneRow plans to check number of rows at runtime."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_assert_maxonerow,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enumerate_plans", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Enable plan enumeration"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enumerate_plans,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_sample_plans", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable plan sampling"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_sample_plans,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_cte_inlining", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable CTE inlining"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_cte_inlining,
		false,
		NULL, NULL, NULL
	},


	{
		{"polar_px_optimizer_enable_constant_expression_evaluation", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable constant expression evaluation in the optimizer"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_constant_expression_evaluation,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_use_external_const_expr_eval_for_ints", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Use external constant expression evaluation in the optimizer for all integer types"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_use_external_constant_expression_evaluation_for_ints,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_outerjoin_to_unionall_rewrite", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable rewriting Left Outer Join to UnionAll"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_outerjoin_to_unionall_rewrite,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_apply_left_outer_to_union_all_disregarding_stats", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Always apply Left Outer Join to Inner Join UnionAll Left Anti Semi Join without looking at stats."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_apply_left_outer_to_union_all_disregarding_stats,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_ctas", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable CTAS plans in the optimizer"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_ctas,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_remove_order_below_dml", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Remove OrderBy below a DML operation"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_remove_order_below_dml,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_dml", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable DML plans in PolarDB Parallel Optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_dml,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_dml_triggers", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Support DML with triggers."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_dml_triggers,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_dml_constraints", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Support DML with CHECK constraints and NOT NULL constraints."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_dml_constraints,
		false,
		NULL, NULL, NULL
	},

	{
		{"coredump_on_memerror", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Generate core dump on memory error."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&coredump_on_memerror,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_relsize_collection", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("This guc enables relsize collection when stats are not present. If disabled and stats are not present a default "
						 "value is used."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_relsize_collection,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_parallel_union", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable parallel execution for UNION/UNION ALL queries."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_parallel_union,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_array_constraints", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Allows the optimizer's constraint framework to derive array constraints."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_array_constraints,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_eageragg", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable Eager Agg transform for pushing aggregate below an innerjoin."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_eageragg,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_is_writer", PGC_BACKEND, PX_WORKER_IDENTITY,
			gettext_noop("True in a worker process which can directly update its local database segment."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&px_is_writer,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_execute_pruned_plan", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Prune plan to discard unwanted plan nodes for each slice before execution"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_execute_pruned_plan,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_print", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable px print query string on px."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_print,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_explain_all_stat", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Experimental feature: dump stats for all segments in EXPLAIN ANALYZE."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_explain_all_stat,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_join", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable px_enable_join."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_join,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_window_function", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable px window function."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_window_function,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_cte", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable px cte."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_cte,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_reject_internal_tcp_connection", PGC_POSTMASTER,
			DEVELOPER_OPTIONS,
			gettext_noop("Permit internal TCP connections to the master."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&px_reject_internal_tcp_conn,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_force_three_stage_scalar_dqa", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Force optimizer to always pick 3 stage aggregate plan for scalar distinct qualified aggregate."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_force_three_stage_scalar_dqa,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_subquery", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable px_enable_subquery."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_subquery,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_prune_unused_columns", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Prune unused table columns during query optimization."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_prune_unused_columns,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_associativity", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables Join Associativity in optimizer"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_associativity,
		false, NULL, NULL
	},

	/* POLAR */
	{
		{"polar_px_enable_check_workers", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable polar px workers for test"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_check_workers,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_info_debug", PGC_USERSET, UNGROUPED,
			gettext_noop("Enable polar parallel query debug info"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_info_debug,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_debug_cancel_print", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Print cancel detail information."),
			NULL,
			GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_debug_cancel_print,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_log_dispatch_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes dispatcher performance statistics to the server log."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_log_dispatch_stats,
		false,
		px_check_dispatch_log_stats, NULL, NULL
	},

	{
		{"polar_px_enable_replay_wait", PGC_USERSET, UNGROUPED,
			gettext_noop("Enable polar PXs wait for replay wal"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_replay_wait,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_transaction", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable Polar Parallel Execution in transaction"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_transaction,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_plan_cache", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable plan cache when using Polar parallel execution"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_plan_cache,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_sort_distinct", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable sort distinct when using Polar parallel execution"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_sort_distinct,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_join_prefetch_inner", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable join do prefetch inner child to avoid dead lock"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_join_prefetch_inner,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_plpgsql", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable plpgsql function to use parallel execution"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_plpgsql,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_procedure", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable procedure to use parallel execution"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_plpgsql,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_prepare_statement", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Enable prepare statment to use parallel execution"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_prepare_statement,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_udpic_enable_ipv6", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("enable udpic ipv6"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_udpic_network_enable_ipv6,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_dispatch_async", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("enable dispatch async mode"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_dispatch_async,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_udp_testmode", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable or disable px_enable_udp_testmode"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_udp_testmode,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_tcp_testmode", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Enable or disable px_enable_tcp_testmode"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_tcp_testmode,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_check_csn", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable or disable check csn"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_check_csn,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_enable_nestloopjoin", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's support of nestloop joins."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_nestloopjoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_lasj_notin", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's support of left anti semi join not in."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_lasj_notin,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_enable_crossproduct", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Enables the optimizer's support of left anti semi join."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_enable_crossproduct,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_use_px_allocators", PGC_POSTMASTER, QUERY_TUNING_METHOD,
			gettext_noop("Enable px_optimizer_use_px_allocators."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_use_px_allocators,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_remove_redundant_results", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Remove redundant results in plan."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_remove_redundant_results,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_test_print_direct_dispatch_info", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("For testing purposes, print information about direct dispatch decisions."),
			NULL,
			GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_test_print_direct_dispatch_info,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_sethintbits", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Allow to set hintbit in PX query"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_sethintbits,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_btbuild", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Polar PX enable btree build"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_btbuild,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_cte_shared_scan", PGC_USERSET, UNGROUPED,
			gettext_noop("polar enable cte shared scan"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_cte_shared_scan,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_adps", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Enable dynamic seqscan"),
			NULL,
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_adaptive_scan,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_adps_explain_analyze", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Enable dynamic seqscan explain analyze details"),
			NULL,
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_adps_explain_analyze,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_partition", PGC_USERSET, UNGROUPED,
			gettext_noop("polar px partition support"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_partition,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_partition_hash", PGC_USERSET, UNGROUPED,
			gettext_noop("polar px partition support"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_partition_hash,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_relsize_collection", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("This guc enables relsize collection when stats are not present. If disabled and stats are not present a default "
						 "value is used."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_relsize_collection,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_tableless_scan", PGC_USERSET, UNGROUPED,
			gettext_noop("polar enable tableless scan"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_tableless_scan,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_allow_strat_seqscan", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows strat when heap seqscan in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_allow_strat_seqscan,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_pre_optimizer_check", PGC_USERSET, UNGROUPED,
			gettext_noop("polar enable pre optimizer check"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_pre_optimizer_check,
		true,
	},

	{
		{"polar_px_enable_btbuild_cic_phase2", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Polar PX enable btree build for cic phase2"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_btbuild_cic_phase2,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_partitionwise_join", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows partition wise join in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_partitionwise_join,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_allow_strat_bitmapscan", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows strat when heap bitmapscan in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_allow_strat_bitmapscan,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_allow_sync_seqscan", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows sync when heap seqscan in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},

		/*
		 * XXX: it seems this GUC is useless as sync scan flag will be
		 * disabled inside initscan() routine
		 */
		&px_allow_sync_seqscan,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_allow_sync_bitmapscan", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows sync when heap bitmapscan in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_allow_sync_bitmapscan,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_allow_pagemode_seqscan", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows pagemode when heap bitmapscan in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_allow_pagemode_seqscan,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_allow_pagemode_bitmapscan", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows pagemode when heap bitmapscan in PX."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_allow_pagemode_bitmapscan,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_left_index_nestloop_join", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows px support left outer index nestloop join."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_left_index_nestloop_join,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_result_hash_filter", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows px support hash filter in result"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_result_hash_filter,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_create_table_as", PGC_USERSET, UNGROUPED,
			gettext_noop("This GUC allows PX to support create table as in parallel."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_create_table_as,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_insert_select", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows Insert....select on px."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_insert_select,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_insert_partition_table", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows Insert partition table select on px."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_insert_partition_table,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_insert_from_tableless", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows insert ... from tableless on px."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_insert_from_tableless,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_opfamily_for_distribution", PGC_USERSET, UNGROUPED,
			gettext_noop("Allow to use pg_opfamily for hash redistribute."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_opfamily_for_distribution,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_spi_read_all_namespaces", PGC_USERSET, UNGROUPED,
			gettext_noop("Allow px workers to read all tables in SPI."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_spi_read_all_namespaces,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_insert_order_sensitive", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows parallel insert need ordered."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_insert_order_sensitive,
		true,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_update", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows parallel update."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_update,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_enable_delete", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc allows parallel delete."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_delete,
		false,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_remove_superfluous_order", PGC_USERSET, UNGROUPED,
			gettext_noop("This guc remove superfluous order"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_remove_superfluous_order,
		true,
		NULL, NULL, NULL
	},

	{
		{"polar_px_use_master", PGC_USERSET, UNGROUPED,
			gettext_noop("Whether PolarDB PX use master"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_use_master,
		false,
		NULL, (void (*) (bool, void *)) polar_invalid_px_nodes_cache, NULL
	},

	{
		{"polar_px_use_standby", PGC_USERSET, UNGROUPED,
			gettext_noop("Whether PolarDB PX use standby"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_use_standby,
		false,
		NULL, (void (*) (bool, void *)) polar_invalid_px_nodes_cache, NULL
	},

	{
		{"polar_px_ignore_unusable_nodes", PGC_USERSET, UNGROUPED,
			gettext_noop("Whether PolarDB PX skip unusable nodes"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&polar_px_ignore_unusable_nodes,
		false,
		NULL, NULL, NULL
	},

	{
		{"polar_px_enable_auth_on_qe", PGC_SIGHUP, UNGROUPED,
			gettext_noop("Whether incoming PX connections are explicitly authenticated"),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_enable_auth_on_qe,
		true,
		NULL, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL
	}
};

struct config_real ConfigureNamesReal_px[] =
{
	{
		{"polar_px_motion_cost_per_row", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "moving a row between worker processes."),
			gettext_noop("If >0, the planner uses this value -- instead of double the "
						 "cpu_tuple_cost -- for Motion operator cost estimation."),
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_motion_cost_per_row,
		0, 0, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_selectivity_damping_factor", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Factor used in selectivity damping."),
			gettext_noop("Values 1..N, 1 = basic damping, greater values emphasize damping"),
			GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_selectivity_damping_factor,
		1.0, 1.0, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_damping_factor_filter", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("select predicate damping factor in optimizer, 1.0 means no damping"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_damping_factor_filter,
		0.75, 0.0, 1.0,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_damping_factor_join", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("join predicate damping factor in optimizer, 1.0 means no damping"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_damping_factor_join,
		0.01, 0.0, 1.0,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_damping_factor_groupby", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("groupby operator damping factor in optimizer, 1.0 means no damping"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_damping_factor_groupby,
		0.75, 0.0, 1.0,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_cost_threshold", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Set the threshold for plan sampling relative to the cost of best plan, 0.0 means unbounded"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_cost_threshold,
		0.0, 0.0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_nestloop_factor", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Set the nestloop join cost factor in the optimizer"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_nestloop_factor,
		1024.0, 1.0, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_sort_factor", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Set the sort cost factor in the optimizer, 1.0 means same as default, > 1.0 means more costly than default, < 1.0 means means less costly than default"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_sort_factor,
		1.0, 0.0, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_share_tablescan_factor", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Set the table sharescan cost factor in the optimizer, 1.0 means same as default, > 1.0 means more costly than default, < 1.0 means means less costly than default"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_share_tablescan_factor,
		1.0, 0.0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"polar_px_optimizer_share_indexscan_factor", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Set the share indexscan cost factor in the optimizer, 1.0 means same as default, > 1.0 means more costly than default, < 1.0 means means less costly than default"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_share_indexscan_factor,
		1.0, 0.0, DBL_MAX,
		NULL, NULL, NULL
	},


	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0.0, 0.0, 0.0, NULL, NULL
	}
};

struct config_int ConfigureNamesInt_px[] =
{

	{
		{"polar_px_max_slices", PGC_USERSET, PRESET_OPTIONS,
			gettext_noop("Maximum slices for a single query"),
			NULL,
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_max_slices,
		50, 0, INT_MAX, NULL, NULL
	},

	{
		{"polar_px_qc_port", PGC_BACKEND, PX_WORKER_IDENTITY,
			gettext_noop("Shows the Master Postmaster port."),
			gettext_noop("0 for a session's entry process (qDisp)"),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&px_qc_port,
		0, INT_MIN, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_dop_per_node", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("The degree of parallelism per dbid."),
			NULL,
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_dop_per_node,
		1, 1, 128,
		NULL, NULL, NULL
	},

	{
		{"polar_px_cached_px_workers", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the maximum number of px workers to cache between statements."),
			NULL,
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_cached_px_workers,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_worker_connect_timeout", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Maximum time (in seconds) allowed for a new worker process to start or a mirror to respond."),
			gettext_noop("0 indicates 'wait forever'."),
			GUC_UNIT_S | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_worker_connect_timeout,
		180, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_max_packet_size", PGC_BACKEND, PX_ARRAY_TUNING,
			gettext_noop("Sets the max packet size for the Interconnect."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_max_packet_size,
		DEFAULT_PACKET_SIZE, MIN_PACKET_SIZE, MAX_PACKET_SIZE,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_queue_depth", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the maximum size of the receive queue for each connection in the UDP interconnect"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_queue_depth,
		4, 1, 4096,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_snd_queue_depth", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the maximum size of the send queue for each connection in the UDP interconnect"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_snd_queue_depth,
		2, 1, 4096,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_timer_period", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the timer period (in ms) for UDP interconnect"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_MS | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_timer_period,
		5, 1, 100,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_timer_checking_period", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the timer checking period (in ms) for UDP interconnect"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_MS | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_timer_checking_period,
		20, 1, 100,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_default_rtt", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the default rtt (in ms) for UDP interconnect"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_MS | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_default_rtt,
		20, 1, 1000,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_min_rto", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the min rto (in ms) for UDP interconnect"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_MS | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_min_rto,
		20, 1, 1000,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_transmit_timeout", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Timeout (in seconds) on interconnect to transmit a packet"),
			gettext_noop("Used by Interconnect to timeout packet transmission."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_S | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_transmit_timeout,
		3600, 1, 7200,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_min_retries_before_timeout", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the min retries before reporting a transmit timeout in the interconnect."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_min_retries_before_timeout,
		100, 1, 4096,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_debug_retry_interval", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the interval by retry times to record a debug message for retry."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_debug_retry_interval,
		10, 1, 4096,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_udp_bufsize_k", PGC_BACKEND, PX_ARRAY_TUNING,
			gettext_noop("Sets recv buf size of UDP interconnect, for testing."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&px_interconnect_udp_bufsize_k,
		0, 0, 32768,
		NULL, NULL, NULL
	},

#ifdef USE_ASSERT_CHECKING
	{
		{"polar_px_interconnect_udpic_dropseg", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Specifies a segment to which the dropacks, and dropxmit settings will be applied, for testing. (The default is to apply the dropacks and dropxmit settings to all segments)"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_udpic_dropseg,
		UNDEF_SEGMENT, UNDEF_SEGMENT, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_udpic_dropacks_percent", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the percentage of correctly-received acknowledgment packets to synthetically drop, for testing. (affected by px_interconnect_udpic_dropseg)"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_udpic_dropacks_percent,
		0, 0, 100,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_udpic_dropxmit_percent", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the percentage of correctly-received data packets to synthetically drop, for testing. (affected by px_interconnect_udpic_dropseg)"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_udpic_dropxmit_percent,
		0, 0, 100,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_udpic_fault_inject_percent", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the percentage of fault injected into system calls, for testing. (affected by px_interconnect_udpic_dropseg)"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_udpic_fault_inject_percent,
		0, 0, 100,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_udpic_fault_inject_bitmap", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the bitmap for faults injection, for testing. (affected by px_interconnect_udpic_dropseg)"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_udpic_fault_inject_bitmap,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},
#endif
	{
		{"polar_px_interconnect_setup_timeout", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Timeout (in seconds) on interconnect setup that occurs at query start"),
			gettext_noop("Used by Interconnect to timeout the setup of the communication fabric."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_S | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&interconnect_setup_timeout,
		7200, 0, 7200,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_tcp_listener_backlog", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Size of the listening queue for each TCP interconnect socket"),
			gettext_noop("Cooperate with kernel parameter net.core.somaxconn and net.ipv4.tcp_max_syn_backlog to tune network performance."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_tcp_listener_backlog,
		128, 0, 65535,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_plan_id", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Choose a plan alternative"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_plan_id,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_samples_number", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Set the number of plan samples"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_samples_number,
		1000, 1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_cte_inlining_bound", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Set the CTE inlining cutoff"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_cte_inlining_bound,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_segments", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Number of segments to be considered by the optimizer during costing, or 0 to take the actual number of segments."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_segments,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_array_expansion_threshold", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Item limit for expansion of arrays in WHERE clause for constraint derivation."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_array_expansion_threshold,
		100, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_push_group_by_below_setop_threshold", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Maximum number of children setops have to consider pushing group bys below it"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_push_group_by_below_setop_threshold,
		10, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_join_order_threshold", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Maximum number of join children to use dynamic programming based join ordering algorithm."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_join_order_threshold,
		10, 0, 12,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_join_arity_for_associativity_commutativity", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Maximum number of children n-ary-join have without disabling commutativity and associativity transform"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_join_arity_for_associativity_commutativity,
		18, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_penalize_broadcast_threshold", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Maximum number of rows of a relation that can be broadcasted without penalty."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_penalize_broadcast_threshold,
		100000, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_mdcache_size", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the size of MDCache."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_UNIT_KB | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_mdcache_size,
		16384, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_max_workers_number", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Set the max number of px workers"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_max_workers_number,
		30, 0, 1024,
		NULL, NULL, NULL
	},

	{
		{"polar_px_scan_unit_size", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets size of scan unit when using Parallel Execution"),
			NULL,
			GUC_UNIT_BLOCKS | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_scan_unit_size,
		512, 1, 1024,
		px_check_scan_unit_size, NULL, NULL
	},

	{
		{"polar_px_max_plan_size", PGC_SUSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum size of a plan to be dispatched."),
			NULL,
			GUC_UNIT_KB | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_max_plan_size,
		0, 0, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	{
		{"polar_px_btbuild_batch_size", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the batch of a px btbuild index tuples batch."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_btbuild_batch_size,
		512, 16, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_btbuild_mem_size", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the mem size of a px btbuild index tuples batch."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_btbuild_mem_size,
		512, 16, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_btbuild_queue_size", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the size of px btbuild index queue."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_btbuild_queue_size,
		1024, 128, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_wait_lock_timeout", PGC_USERSET, LOCK_MANAGEMENT,
			gettext_noop("Sets the maximum time (in milliseconds) to wait on a lock held by PX process."),
			NULL,
			GUC_UNIT_MS | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_wait_lock_timeout,
		3600000, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"polar_px_insert_dop_num", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the num of px insert dop."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_insert_dop_num,
		6, 1, 128,
		NULL, NULL, NULL
	},
	{
		{"polar_px_update_dop_num", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the num of px update dop."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_update_dop_num,
		6, 1, 128,
		NULL, NULL, NULL
	},
	{
		{"polar_px_delete_dop_num", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the num of px delete dop."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_delete_dop_num,
		6, 1, 128,
		NULL, NULL, NULL
	},
	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL, NULL
	}

};

struct config_string ConfigureNamesString_px[] =
{
	{
		{"polar_px_qc_hostname", PGC_BACKEND, PX_WORKER_IDENTITY,
			gettext_noop("Shows the QC Hostname. Blank when run on the QC"),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&px_qc_hostname,
		"",
		NULL, NULL, NULL
	},

	{
		{"polar_px_nodes", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets px worker nodes, separated by comma"),
			NULL,
			GUC_LIST_INPUT | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&polar_px_nodes,
		"",
		NULL, (void (*) (const char *, void *)) polar_invalid_px_nodes_cache, NULL
	},

	{
		{"polar_px_optimizer_search_strategy_path", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Sets the search strategy used by gp optimizer."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_search_strategy_path,
		"default",
		NULL, NULL, NULL
	},

	{
		{"polar_px_version", PGC_INTERNAL, UNGROUPED,
			gettext_noop("Show the PolarDB PX version."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&polar_px_version,
		PX_VERSION_STR,
		NULL, NULL, NULL
	},

	{
		{"polar_px_ignore_function", PGC_USERSET, UNGROUPED,
			gettext_noop("Add the PolarDB PX functions oids that should be ignored."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&polar_px_ignore_function,
		"1574,1575,1576,1765,3078,4032",
		px_check_ignore_function, px_assign_ignore_function, NULL
	},

	{
		{"polar_px_ca_file", PGC_SIGHUP, PX_WORKER_IDENTITY,
			gettext_noop("Location of the TLS certificate authority file for PX workers."),
			gettext_noop("It's used for verification of incoming certs from QCs."),
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_ca_file,
		"",
		px_check_tls_support, _px_need_reload_ca, NULL
	},

	{
		{"polar_px_peer_cert_file", PGC_SIGHUP, PX_WORKER_IDENTITY,
			gettext_noop("Location of the TLS certificate file for QCs."),
			gettext_noop("It's used for authentication of QCs in PX connections."),
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_peer_cert_file,
		"",
		px_check_tls_support, _px_need_reload_cert_pkey, NULL
	},

	{
		{"polar_px_peer_key_file", PGC_SIGHUP, PX_WORKER_IDENTITY,
			gettext_noop("Location of the TLS private key for QCs."),
			gettext_noop("It's used for authentication of QCs in PX connections."),
			GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_peer_key_file,
		"",
		px_check_tls_support, _px_need_reload_cert_pkey, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, NULL, NULL, NULL, NULL
	}
};
struct config_enum ConfigureNamesEnum_px[] =
{
	{
		{"polar_px_interconnect_type", PGC_BACKEND, PX_ARRAY_TUNING,
			gettext_noop("Sets the protocol used for inter-node communication."),
			gettext_noop("Valid values are \"tcp\" and \"udpifc\"."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_UNCHANGABLE
		},
		&px_interconnect_type,
		INTERCONNECT_TYPE_UDPIFC, px_interconnect_types,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_fc_method", PGC_USERSET, PX_ARRAY_TUNING,
			gettext_noop("Sets the flow control method used for UDP interconnect."),
			gettext_noop("Valid values are \"capacity\" and \"loss\"."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_fc_method,
		INTERCONNECT_FC_METHOD_LOSS, px_interconnect_fc_methods,
		NULL, NULL, NULL
	},

	{
		{"polar_px_interconnect_log", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Sets the verbosity of logged messages pertaining to connections between worker processes."),
			gettext_noop("Valid values are \"off\", \"terse\", \"verbose\" and \"debug\"."),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_interconnect_log,
		PXVARS_VERBOSITY_TERSE, px_log_verbosity,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_cost_model", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Set optimizer cost model."),
			gettext_noop("Valid values are calibrated, polardb"),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_cost_model,
		OPTIMIZER_GPDB_CALIBRATED, px_optimizer_cost_model_options,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_join_order", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Set optimizer join heuristic model."),
			gettext_noop("Valid values are query, greedy, exhaustive and exhaustive2"),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_join_order,
		JOIN_ORDER_EXHAUSTIVE_SEARCH, px_optimizer_join_order_options,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_minidump", PGC_USERSET, LOGGING_WHEN,
			gettext_noop("Generate optimizer minidump."),
			gettext_noop("Valid values are onerror, always"),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_minidump,
		OPTIMIZER_MINIDUMP_FAIL, optimizer_minidump_options,
		NULL, NULL, NULL
	},

	{
		{"polar_px_optimizer_log_failure", PGC_USERSET, LOGGING_WHEN,
			gettext_noop("Sets which optimizer failures are logged."),
			gettext_noop("Valid values are unexpected, expected, all"),
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_optimizer_log_failure,
		OPTIMIZER_UNEXPECTED_FAIL, px_optimizer_log_failure_options,
		NULL, NULL, NULL
	},

	{
		{"polar_px_explain_memory_verbosity", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Experimental feature: show memory account usage in EXPLAIN ANALYZE."),
			gettext_noop("Valid values are SUPPRESS, SUMMARY, DETAIL."),
			POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE
		},
		&px_explain_memory_verbosity,
		EXPLAIN_MEMORY_VERBOSITY_SUPPRESS, px_explain_memory_verbosity_options,
		NULL, NULL, NULL
	},
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, NULL, NULL, NULL
	}
};

static bool
px_check_dispatch_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval &&
		(log_parser_stats || log_planner_stats || log_executor_stats || log_statement_stats))
	{
		if (source >= PGC_S_INTERACTIVE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot enable \"log_dispatch_stats\" when "
							"\"log_statement_stats\", "
							"\"log_parser_stats\", \"log_planner_stats\", "
							"or \"log_executor_stats\" is true")));
		/* source == PGC_S_OVERRIDE means do it anyway, eg at xact abort */
		else if (source != PGC_S_OVERRIDE)
			return false;
	}
	return true;
}

static bool
px_check_scan_unit_size(int *newval, void **extra, GucSource source)
{
	if ((*newval <= 0) || (*newval & (*newval - 1)) != 0)
	{
		GUC_check_errdetail("scan unit size in blocks, it should be power of 2, min size is 1 block");
		return false;
	}

	{
		int			size = *newval;

		px_scan_unit_bit = 0;
		while (size > 1)
		{
			size = size >> 1;
			px_scan_unit_bit++;
		}
	}

	return true;
}

static bool
px_check_ignore_function(char **newval, void **extra, GucSource source)
{
	ListCell   *lc = NULL;
	char	   *rawstring = NULL;
	int			index = 0;
	List	   *elemlist = NULL;
	PxFunctionOidArray *tmp_array = NULL;

	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in function oid list */
		GUC_check_errdetail("polar px ignore function list is invalid, func1,func2,func3, ...");
		goto out_result;
	}

	tmp_array = guc_malloc(FATAL, sizeof(PxFunctionOidArray) + list_length(elemlist) * sizeof(int));
	if (!tmp_array)
		return false;

	tmp_array->count = list_length(elemlist);

	foreach(lc, elemlist)
	{
		char	   *s = (char *) lfirst(lc);
		int			oid = atoi(s);

		if (oid <= 0)
		{
			/* syntax error in function oid list */
			GUC_check_errdetail("polar px ignore function list is invalid, func1,func2,func3, ...");
			goto out_result;
		}
		tmp_array->oid[index++] = oid;
	}

	Assert(tmp_array->count == index);

	*extra = tmp_array;

	return true;

out_result:
	list_free(elemlist);
	if (rawstring)
		pfree(rawstring);
	if (tmp_array)
		free(tmp_array);
	return false;
}

static void
px_assign_ignore_function(const char *newval, void *extra)
{
	px_function_oid_array = (PxFunctionOidArray *) extra;
}

static bool
px_check_tls_support(char **newval, void **extra, GucSource source)
{
#ifndef USE_SSL
	const char *tls_spec_file = *newval;

	if (tls_spec_file[0])
	{
		GUC_check_errdetail("TLS is not supported by this build");
		return false;
	}
#endif

	return true;
}

static void
_px_need_reload_ca(const char *newval, void *extra)
{
#ifdef USE_SSL
	px_need_reload_ca(newval, extra);
#endif
}

static void
_px_need_reload_cert_pkey(const char *newval, void *extra)
{
#ifdef USE_SSL
	px_need_reload_cert_pkey(newval, extra);
#endif
}

/*
 * For system defined GUC must assign a tag either GUC_PX_NEED_SYNC
 * or GUC_PX_NO_SYNC. We deprecated direct define in guc.c, instead,
 * add into sync_guc_names_array or unsync_guc_names_array.
 */
static const char *sync_guc_names_array[] =
{
#include "utils/px_sync_guc_name.h"
};

static const char *unsync_guc_names_array[] =
{
#include "utils/px_unsync_guc_name.h"
};

int			sync_guc_num = 0;
int			unsync_guc_num = 0;

static int
px_guc_array_compare(const void *a, const void *b)
{
	const char *namea = *(const char **) a;
	const char *nameb = *(const char **) b;

	return guc_name_compare(namea, nameb);
}

void
px_assign_sync_flag(struct config_generic **guc_variables, int size, bool predefine)
{
	static bool init = false;
	int			i;

	/* ordering guc_name_array alphabets */
	if (!init)
	{
		sync_guc_num = sizeof(sync_guc_names_array) / sizeof(char *);
		qsort((void *) sync_guc_names_array, sync_guc_num,
			  sizeof(char *), px_guc_array_compare);

		unsync_guc_num = sizeof(unsync_guc_names_array) / sizeof(char *);
		qsort((void *) unsync_guc_names_array, unsync_guc_num,
			  sizeof(char *), px_guc_array_compare);

		init = true;
	}

	for (i = 0; i < size; i++)
	{
		struct config_generic *var = guc_variables[i];
		char	   *res;

		/* if the sync flags is defined in guc variable, skip it */
		if (var->flags & (GUC_PX_NEED_SYNC | GUC_PX_NO_SYNC))
			continue;

		/* if the context is defined as internal postmaster sighup, skip it */
		if (var->context <= PGC_SIGHUP)
			continue;

		res = (char *) bsearch((void *) &var->name,
							   (void *) sync_guc_names_array,
							   sync_guc_num,
							   sizeof(char *),
							   px_guc_array_compare);
		if (!res)
		{
			char	   *res = (char *) bsearch((void *) &var->name,
											   (void *) unsync_guc_names_array,
											   unsync_guc_num,
											   sizeof(char *),
											   px_guc_array_compare);

			/*
			 * for predefined guc, we force its name in one array. for the
			 * third-part libraries gucs introduced by customer we assign
			 * unsync flags as default.
			 */
			if (!res && predefine)
			{
				elog(ERROR, "Neither px_sync_guc_name.h nor "
					 "px_unsync_guc_name.h contains "
					 "guc name: %s", var->name);
			}

			var->flags |= GUC_PX_NO_SYNC;
		}
		else
		{
			var->flags |= GUC_PX_NEED_SYNC;
		}
	}
}
