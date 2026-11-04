/*--------------------------------------------------------------------
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * Copyright (c) 2000-2022, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * src/include/utils/guc.h
 *--------------------------------------------------------------------
 */
#ifndef GUC_H
#define GUC_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"
#include "utils/array.h"
#include "utils/polar_cluster_settings.h"


/* upper limit for GUC variables measured in kilobytes of memory */
/* note that various places assume the byte size fits in a "long" variable */
#if SIZEOF_SIZE_T > 4 && SIZEOF_LONG > 4
#define MAX_KILOBYTES	INT_MAX
#else
#define MAX_KILOBYTES	(INT_MAX / 1024)
#endif

/*
 * Automatic configuration file name for ALTER SYSTEM.
 * This file will be used to store values of configuration parameters
 * set by ALTER SYSTEM command.
 */
#define PG_AUTOCONF_FILENAME		"postgresql.auto.conf"

/* POLAR */
/*
 * Size of static PX buffer. The buffer is used to collect log messages
 * produced by PX planner during optimization stage.
 */
#define PXOPT_ERROR_BUFFER_SIZE 		(10 * 1024 * 1024)

/*
 * MAX audit log len is set to size of px error buffer to collect debugging
 * information about px_optimizer decisions.
 */
#define POLAR_MAX_AUDIT_LOG_LEN 		PXOPT_ERROR_BUFFER_SIZE
#define POLAR_DEFAULT_MAX_AUDIT_LOG_LEN 2048
/* POLAR end */

/*
 * Certain options can only be set at certain times. The rules are
 * like this:
 *
 * INTERNAL options cannot be set by the user at all, but only through
 * internal processes ("server_version" is an example).  These are GUC
 * variables only so they can be shown by SHOW, etc.
 *
 * POSTMASTER options can only be set when the postmaster starts,
 * either from the configuration file or the command line.
 *
 * SIGHUP options can only be set at postmaster startup or by changing
 * the configuration file and sending the HUP signal to the postmaster
 * or a backend process. (Notice that the signal receipt will not be
 * evaluated immediately. The postmaster and the backend check it at a
 * certain point in their main loop. It's safer to wait than to read a
 * file asynchronously.)
 *
 * BACKEND and SU_BACKEND options can only be set at postmaster startup,
 * from the configuration file, or by client request in the connection
 * startup packet (e.g., from libpq's PGOPTIONS variable).  SU_BACKEND
 * options can be set from the startup packet only when the user is a
 * superuser.  Furthermore, an already-started backend will ignore changes
 * to such an option in the configuration file.  The idea is that these
 * options are fixed for a given backend once it's started, but they can
 * vary across backends.
 *
 * SUSET options can be set at postmaster startup, with the SIGHUP
 * mechanism, or from the startup packet or SQL if you're a superuser.
 *
 * USERSET options can be set by anyone any time.
 */
typedef enum
{
	PGC_INTERNAL,
	PGC_POSTMASTER,
	PGC_SIGHUP,
	PGC_SU_BACKEND,
	PGC_BACKEND,
	PGC_SUSET,
	PGC_USERSET
} GucContext;

/*
 * POLAR:
 * Virtual GucContext to load cluster polar_settings.conf, treate as PGC_POSTMASTER
 */
#define PGC_POSTMASTER_ONLY_LOAD_POLAR_SETTINGS 0x7fff
/* POLAR end */

/*
 * The following type records the source of the current setting.  A
 * new setting can only take effect if the previous setting had the
 * same or lower level.  (E.g, changing the config file doesn't
 * override the postmaster command line.)  Tracking the source allows us
 * to process sources in any convenient order without affecting results.
 * Sources <= PGC_S_OVERRIDE will set the default used by RESET, as well
 * as the current value.
 *
 * PGC_S_INTERACTIVE isn't actually a source value, but is the
 * dividing line between "interactive" and "non-interactive" sources for
 * error reporting purposes.
 *
 * PGC_S_TEST is used when testing values to be used later.  For example,
 * ALTER DATABASE/ROLE tests proposed per-database or per-user defaults this
 * way, and CREATE FUNCTION tests proposed function SET clauses this way.
 * This is an interactive case, but it needs its own source value because
 * some assign hooks need to make different validity checks in this case.
 * In particular, references to nonexistent database objects generally
 * shouldn't throw hard errors in this case, at most NOTICEs, since the
 * objects might exist by the time the setting is used for real.
 *
 * When setting the value of a non-compile-time-constant PGC_INTERNAL option,
 * source == PGC_S_DYNAMIC_DEFAULT should typically be used so that the value
 * will show as "default" in pg_settings.  If there is a specific reason not
 * to want that, use source == PGC_S_OVERRIDE.
 *
 * NB: see GucSource_Names in guc.c if you change this.
 */
typedef enum
{
	PGC_S_DEFAULT,				/* hard-wired default ("boot_val") */
	PGC_S_DYNAMIC_DEFAULT,		/* default computed during initialization */
	PGC_S_ENV_VAR,				/* postmaster environment variable */
	PGC_S_FILE,					/* postgresql.conf */
	PGC_S_ARGV,					/* postmaster command line */
	PGC_S_GLOBAL,				/* global in-database setting */
	PGC_S_DATABASE,				/* per-database setting */
	PGC_S_USER,					/* per-user setting */
	PGC_S_DATABASE_USER,		/* per-user-and-database setting */
	PGC_S_CLIENT,				/* from client connection request */
	PGC_S_OVERRIDE,				/* special case to forcibly set default */
	PGC_S_INTERACTIVE,			/* dividing line for error reporting */
	PGC_S_TEST,					/* test per-database or per-user setting */
	PGC_S_SESSION				/* SET command */
} GucSource;

/*
 * Parsing the configuration file(s) will return a list of name-value pairs
 * with source location info.  We also abuse this data structure to carry
 * error reports about the config files.  An entry reporting an error will
 * have errmsg != NULL, and might have NULLs for name, value, and/or filename.
 *
 * If "ignore" is true, don't attempt to apply the item (it might be an error
 * report, or an item we determined to be duplicate).  "applied" is set true
 * if we successfully applied, or could have applied, the setting.
 */
typedef struct ConfigVariable
{
	char	   *name;
	char	   *value;
	char	   *errmsg;
	char	   *filename;
	int			sourceline;
	bool		ignore;
	bool		applied;
	struct ConfigVariable *next;
} ConfigVariable;

extern bool ParseConfigFile(const char *config_file, bool strict,
							const char *calling_file, int calling_lineno,
							int depth, int elevel,
							ConfigVariable **head_p, ConfigVariable **tail_p);
extern bool ParseConfigFp(FILE *fp, const char *config_file,
						  int depth, int elevel,
						  ConfigVariable **head_p, ConfigVariable **tail_p);
extern bool ParseConfigDirectory(const char *includedir,
								 const char *calling_file, int calling_lineno,
								 int depth, int elevel,
								 ConfigVariable **head_p,
								 ConfigVariable **tail_p);
extern void FreeConfigVariables(ConfigVariable *list);
extern char *DeescapeQuotedString(const char *s);

/*
 * The possible values of an enum variable are specified by an array of
 * name-value pairs.  The "hidden" flag means the value is accepted but
 * won't be displayed when guc.c is asked for a list of acceptable values.
 */
struct config_enum_entry
{
	const char *name;
	int			val;
	bool		hidden;
};

/*
 * Signatures for per-variable check/assign/show hook functions
 */
typedef bool (*GucBoolCheckHook) (bool *newval, void **extra, GucSource source);
typedef bool (*GucIntCheckHook) (int *newval, void **extra, GucSource source);
typedef bool (*GucRealCheckHook) (double *newval, void **extra, GucSource source);
typedef bool (*GucStringCheckHook) (char **newval, void **extra, GucSource source);
typedef bool (*GucEnumCheckHook) (int *newval, void **extra, GucSource source);

typedef void (*GucBoolAssignHook) (bool newval, void *extra);
typedef void (*GucIntAssignHook) (int newval, void *extra);
typedef void (*GucRealAssignHook) (double newval, void *extra);
typedef void (*GucStringAssignHook) (const char *newval, void *extra);
typedef void (*GucEnumAssignHook) (int newval, void *extra);

typedef const char *(*GucShowHook) (void);

/*
 * Miscellaneous
 */
typedef enum
{
	/* Types of set_config_option actions */
	GUC_ACTION_SET,				/* regular SET command */
	GUC_ACTION_LOCAL,			/* SET LOCAL command */
	GUC_ACTION_SAVE				/* function SET option, or temp assignment */
} GucAction;

#define GUC_QUALIFIER_SEPARATOR '.'

/*
 * bit values in "flags" of a GUC variable
 */
#define GUC_LIST_INPUT			0x0001	/* input can be list format */
#define GUC_LIST_QUOTE			0x0002	/* double-quote list elements */
#define GUC_NO_SHOW_ALL			0x0004	/* exclude from SHOW ALL */
#define GUC_NO_RESET_ALL		0x0008	/* exclude from RESET ALL */
#define GUC_REPORT				0x0010	/* auto-report changes to client */
#define GUC_NOT_IN_SAMPLE		0x0020	/* not in postgresql.conf.sample */
#define GUC_DISALLOW_IN_FILE	0x0040	/* can't set in postgresql.conf */
#define GUC_CUSTOM_PLACEHOLDER	0x0080	/* placeholder for custom variable */
#define GUC_SUPERUSER_ONLY		0x0100	/* show only to superusers */
#define GUC_IS_NAME				0x0200	/* limit string to NAMEDATALEN-1 */
#define GUC_NOT_WHILE_SEC_REST	0x0400	/* can't set if security restricted */
#define GUC_DISALLOW_IN_AUTO_FILE 0x0800	/* can't set in
											 * PG_AUTOCONF_FILENAME */

#define GUC_UNIT_KB				0x1000	/* value is in kilobytes */
#define GUC_UNIT_BLOCKS			0x2000	/* value is in blocks */
#define GUC_UNIT_XBLOCKS		0x3000	/* value is in xlog blocks */
#define GUC_UNIT_MB				0x4000	/* value is in megabytes */
#define GUC_UNIT_BYTE			0x8000	/* value is in bytes */
#define GUC_UNIT_MEMORY			0xF000	/* mask for size-related units */

#define GUC_UNIT_MS			   0x10000	/* value is in milliseconds */
#define GUC_UNIT_S			   0x20000	/* value is in seconds */
#define GUC_UNIT_MIN		   0x30000	/* value is in minutes */
#define GUC_UNIT_TIME		   0xF0000	/* mask for time-related units */

#define GUC_EXPLAIN			  0x100000	/* include in explain */

/*
 * GUC_RUNTIME_COMPUTED is intended for runtime-computed GUCs that are only
 * available via 'postgres -C' if the server is not running.
 */
#define GUC_RUNTIME_COMPUTED  0x200000
#define GUC_ALLOW_IN_PARALLEL 0x400000	/* allow setting in parallel mode */

#define GUC_UNIT				(GUC_UNIT_MEMORY | GUC_UNIT_TIME)

/* POLAR: Shared Server */
#define GUC_SESSION_DEDICATED   0x10000000	/* guc need in dedicated mode */
#define GUC_ASSIGN_IN_TRANS		0x20000000	/* assign need in trans */
/* POLAR end */

/* POLAR defines start */
#define MAX_NUM_OF_PARALLEL_BGWRITER	64
/* POLAR end */

/*
 * POLAR: parameter manage
 * flag for hints displayed in the DBass console.
 * default value for no flags are is_visiabe = false and is_user_chanagable = true
 */
#define POLAR_GUC_IS_VISIBLE		0x1000000	/* visible for user */
#define POLAR_GUC_IS_INVISIBLE		0x2000000	/* invisible for user */
#define	POLAR_GUC_IS_CHANGABLE		0x4000000	/* changable for user or
												 * manger */
#define	POLAR_GUC_IS_UNCHANGABLE	0x8000000	/* unchangable for user or
												 * manger */
extern struct config_generic *polar_parameter_check_name_internal(const char *guc_name);

/* POLAR end */

/* GUC vars that are actually declared in guc.c, rather than elsewhere */
extern PGDLLIMPORT bool Debug_print_plan;
extern PGDLLIMPORT bool Debug_print_parse;
extern PGDLLIMPORT bool Debug_print_rewritten;
extern PGDLLIMPORT bool Debug_pretty_print;

extern PGDLLIMPORT bool log_parser_stats;
extern PGDLLIMPORT bool log_planner_stats;
extern PGDLLIMPORT bool log_executor_stats;
extern PGDLLIMPORT bool log_statement_stats;
extern PGDLLIMPORT bool log_btree_build_stats;

extern PGDLLIMPORT bool check_function_bodies;
extern PGDLLIMPORT bool session_auth_is_superuser;

extern PGDLLIMPORT bool log_duration;
extern PGDLLIMPORT int log_parameter_max_length;
extern PGDLLIMPORT int log_parameter_max_length_on_error;
extern PGDLLIMPORT int log_min_error_statement;
extern PGDLLIMPORT int log_min_messages;
extern PGDLLIMPORT int client_min_messages;
extern PGDLLIMPORT int log_min_duration_sample;
extern PGDLLIMPORT int log_min_duration_statement;
extern PGDLLIMPORT int log_temp_files;
extern PGDLLIMPORT double log_statement_sample_rate;
extern PGDLLIMPORT double log_xact_sample_rate;
extern PGDLLIMPORT char *backtrace_functions;
extern PGDLLIMPORT char *backtrace_symbol_list;

extern PGDLLIMPORT int temp_file_limit;

extern PGDLLIMPORT int num_temp_buffers;

extern PGDLLIMPORT char *cluster_name;
extern PGDLLIMPORT char *ConfigFileName;
extern PGDLLIMPORT char *HbaFileName;
extern PGDLLIMPORT char *IdentFileName;
extern PGDLLIMPORT char *external_pid_file;

extern PGDLLIMPORT char *application_name;

extern PGDLLIMPORT int tcp_keepalives_idle;
extern PGDLLIMPORT int tcp_keepalives_interval;
extern PGDLLIMPORT int tcp_keepalives_count;
extern PGDLLIMPORT int tcp_user_timeout;

/* POLAR */
extern int	polar_hostid;
extern char *polar_datadir;
extern bool polar_enable_shared_storage_mode;
extern bool polar_enable_debug;

extern char *polar_disk_name;
extern char *polar_storage_cluster_name;

/* POLAR end */

#ifdef TRACE_SORT
extern PGDLLIMPORT bool trace_sort;
#endif

/* POLAR GUCs start */

/* POLAR: buffer manager */
extern bool polar_enable_strategy_reject_buffer;
extern bool polar_hot_standby_enable_vm;
extern bool polar_enable_control_vm_flush;
extern bool polar_force_flush_buffer;
extern bool polar_ignore_ro_latency;
extern bool polar_enable_flushlist;

extern bool polar_enable_flush_dispatcher;
extern bool polar_enable_normal_bgwriter;
extern bool polar_enable_dynamic_parallel_bgwriter;
extern bool polar_enable_early_launch_parallel_bgwriter;
extern bool polar_enable_lru_log;
extern double polar_lru_works_threshold;
extern int	polar_parallel_flush_workers;
extern int	polar_parallel_bgwriter_check_interval;
extern int	polar_new_bgwriter_flush_factor;
extern int	polar_parallel_new_bgwriter_threshold_lag;
extern int	polar_parallel_new_bgwriter_threshold_time;
extern int	polar_bgwriter_flush_batch_size;
extern int	polar_bgwriter_batch_size;
extern int	polar_lru_bgwriter_max_pages;
extern int	polar_lru_batch_pages;
extern int	polar_parallel_bgwriter_delay;
extern int	polar_bgwriter_sleep_lsn_lag;
extern int	polar_bgwriter_sleep_lru_lap;

extern int	polar_copy_buffers;
extern int	polar_buffer_copy_min_modified_count;
extern int	polar_buffer_copy_threshold_lag;

extern bool polar_enable_incremental_checkpoint;
extern bool polar_enable_inc_end_of_recovery_checkpoint;
extern int	polar_check_checkpoint_interval;
extern bool polar_enable_alloc_checkinterrupts;

extern bool polar_enable_simply_redo_error_log;

/* POLAR: buffer manager end */

/* POLAR audit */
extern int	polar_max_log_files;
extern bool polar_log_statement_with_duration;
extern int	polar_max_auditlog_files;
extern int	polar_max_slowlog_files;
extern int	polar_auditlog_max_query_length;
extern int	polar_audit_log_flush_timeout;

/* POLAR: GUCs for transaction rw-split */
extern bool polar_enable_xact_split;
extern bool polar_enable_xact_split_debug;

extern bool polar_enable_using_reserved_name;
extern bool polar_enable_send_stop;
extern bool polar_enable_switch_wal_in_backup;

/* POLAR end */

extern bool polar_enable_ring_buffer;
extern int	polar_ring_buffer_bulkread_size;
extern int	polar_ring_buffer_bulkwrite_size;
extern int	polar_ring_buffer_vacuum_size;

/*
 * POLAR
 * instance specification for cpu and memory
 */
extern double polar_instance_spec_cpu;
extern int	polar_instance_spec_mem;

extern bool polar_enable_async_lock_replay;
extern bool polar_enable_async_lock_replay_debug;

extern bool polar_enable_shm_aset;

/* POLAR wal pipeline */

/*
 * For now, we just use native PG spin lock strategy to tune
 * wal pipeline spin and wait.
 */

/*
 * 1000 spin corresponds to 4us, we should spin at most 1ms
 * 0 indicates no spin
 */
#define POLAR_MIN_WAIT_SPINS 	0
#define POLAR_MAX_WAIT_SPINS 	250000

/*
 * how long does it take to wake up on timeout, depending on timeout:
 *	1us ->    57us
 *	10us ->   66us
 *	20us ->   76us
 *	50us ->   106us
 *	100us ->  156us
 *	1000us -> 1100us
 *	reference MySQL 8.0
 *  0 indicates no timeout
 */
#define POLAR_MIN_WAIT_TIMEOUT_USEC		0
#define POLAR_MAX_WAIT_TIMEOUT_USEC		100000

extern bool polar_wal_pipeline_enable;
extern int	polar_wal_pipeline_mode;
extern int	polar_wal_pipeline_wait_timeout;
extern int	polar_wal_pipeline_flush_event_array_size;
extern int	polar_wal_pipeline_flush_event_slot_size;
extern int	polar_wal_pipeline_unflushed_xlog_array_size;
extern int	polar_wal_pipeline_recent_written_array_size;

extern int	polar_wal_pipeline_commit_wait_spin_delay;
extern int	polar_wal_pipeline_commit_wait_timeout;

extern int	polar_wal_pipeline_advance_worker_spin_delay;
extern int	polar_wal_pipeline_advance_worker_timeout;
extern int	polar_wal_pipeline_advance_worker_write_max_size;

extern int	polar_wal_pipeline_write_worker_spin_delay;
extern int	polar_wal_pipeline_write_worker_timeout;

extern int	polar_wal_pipeline_flush_worker_spin_delay;
extern int	polar_wal_pipeline_flush_worker_timeout;

extern int	polar_wal_pipeline_notify_worker_spin_delay;
extern int	polar_wal_pipeline_notify_worker_timeout;
#define POLAR_WAL_PIPELINE_NOTIFY_WORKER_NUM_MAX	4
#define POLAR_WAL_PIPELINE_NOTIFY_WORKER_NUM_MIN	1
extern int	polar_wal_pipeline_notify_worker_num;

/* Polar wal pipeline should enable on instanch with cpu vcores >= 8 */
#define POLAR_INSTANCE_SPEC_WAL_PIPELINE_IS_AVAILABLE()	(polar_instance_spec_cpu == 0 || polar_instance_spec_cpu >= 8)

extern bool polar_csn_enable;
extern bool polar_csn_elog_panic_enable;
extern bool polar_csnlog_upperbound_enable;
extern bool polar_csn_xid_snapshot;

extern int	polar_csnlog_slot_size;
extern int	polar_csnlog_max_local_cache_segments;

/*
 * POLAR: datamax mode selector.  PGC_POSTMASTER enum GUC.
 *
 * POLAR_DATAMAX_OFF		regular operation, datamax disabled.
 * POLAR_DATAMAX_STANDALONE	when standby.signal is also present, the node enters
 *							datamax mode at startup.
 */
typedef enum
{
	POLAR_DATAMAX_OFF = 0,
	POLAR_DATAMAX_STANDALONE,
}			polar_datamax_mode_t;

extern int	polar_datamax_mode;

extern int	polar_datamax_remove_archivedone_wal_timeout;
extern int	polar_datamax_archive_timeout;
extern int	polar_datamax_save_replication_slots_timeout;
extern int	polar_datamax_prealloc_walfile_timeout;
extern int	polar_datamax_prealloc_walfile_num;

/* POLAR GUCs end */

extern bool polar_enable_coredump_print;
extern bool polar_enable_coredump_cleanup;
extern int	polar_save_stack_info_level;
extern char *polar_ignore_coredump_functions;
extern char *polar_ignore_coredump_function_list;
extern int	polar_ignore_coredump_level;
extern bool polar_ignore_coredump_fuzzy_match;

/*
 * Functions exported by guc.c
 */
extern void SetConfigOption(const char *name, const char *value,
							GucContext context, GucSource source);

extern void DefineCustomBoolVariable(const char *name,
									 const char *short_desc,
									 const char *long_desc,
									 bool *valueAddr,
									 bool bootValue,
									 GucContext context,
									 int flags,
									 GucBoolCheckHook check_hook,
									 GucBoolAssignHook assign_hook,
									 GucShowHook show_hook);

extern void DefineCustomIntVariable(const char *name,
									const char *short_desc,
									const char *long_desc,
									int *valueAddr,
									int bootValue,
									int minValue,
									int maxValue,
									GucContext context,
									int flags,
									GucIntCheckHook check_hook,
									GucIntAssignHook assign_hook,
									GucShowHook show_hook);

extern void DefineCustomRealVariable(const char *name,
									 const char *short_desc,
									 const char *long_desc,
									 double *valueAddr,
									 double bootValue,
									 double minValue,
									 double maxValue,
									 GucContext context,
									 int flags,
									 GucRealCheckHook check_hook,
									 GucRealAssignHook assign_hook,
									 GucShowHook show_hook);

extern void DefineCustomStringVariable(const char *name,
									   const char *short_desc,
									   const char *long_desc,
									   char **valueAddr,
									   const char *bootValue,
									   GucContext context,
									   int flags,
									   GucStringCheckHook check_hook,
									   GucStringAssignHook assign_hook,
									   GucShowHook show_hook);

extern void DefineCustomEnumVariable(const char *name,
									 const char *short_desc,
									 const char *long_desc,
									 int *valueAddr,
									 int bootValue,
									 const struct config_enum_entry *options,
									 GucContext context,
									 int flags,
									 GucEnumCheckHook check_hook,
									 GucEnumAssignHook assign_hook,
									 GucShowHook show_hook);

extern void MarkGUCPrefixReserved(const char *className);

/* old name for MarkGUCPrefixReserved, for backwards compatibility: */
#define EmitWarningsOnPlaceholders(className) MarkGUCPrefixReserved(className)

extern const char *GetConfigOption(const char *name, bool missing_ok,
								   bool restrict_privileged);
extern const char *GetConfigOptionResetString(const char *name);
extern int	GetConfigOptionFlags(const char *name, bool missing_ok);
extern void ProcessConfigFile(GucContext context);
extern char *convert_GUC_name_for_parameter_acl(const char *name);
extern bool check_GUC_name_for_parameter_acl(const char *name);
extern void InitializeGUCOptions(void);
extern void InitializeWalConsistencyChecking(void);
extern bool SelectConfigFiles(const char *userDoption, const char *progname, const bool polar_is_show_guc_mode);
extern void ResetAllOptions(void);
extern void AtStart_GUC(void);
extern int	NewGUCNestLevel(void);
extern void AtEOXact_GUC(bool isCommit, int nestLevel);
extern void BeginReportingGUCOptions(void);
extern void ReportChangedGUCOptions(void);
extern void ParseLongOption(const char *string, char **name, char **value);
extern bool parse_int(const char *value, int *result, int flags,
					  const char **hintmsg);
extern bool parse_real(const char *value, double *result, int flags,
					   const char **hintmsg);
extern int	set_config_option(const char *name, const char *value,
							  GucContext context, GucSource source,
							  GucAction action, bool changeVal, int elevel,
							  bool is_reload);
extern int	set_config_option_ext(const char *name, const char *value,
								  GucContext context, GucSource source,
								  Oid srole,
								  GucAction action, bool changeVal, int elevel,
								  bool is_reload);
extern void AlterSystemSetConfigFile(AlterSystemStmt *altersysstmt);

/* POLAR: ALTER SYSTEM FOR CLUSTER */
extern void AlterSystemSetConfigFileInternal(char *name, char *value, bool resetall, bool polar_redo, int polar_options);	/* split from
																															 * AlterSystemSetConfigFile */
extern char *GetConfigOptionByName(const char *name, const char **varname,
								   bool missing_ok);
extern void GetConfigOptionByNum(int varnum, const char **values, bool *noshow);
extern int	GetNumConfigOptions(void);

extern void SetPGVariable(const char *name, List *args, bool is_local);
extern void GetPGVariable(const char *name, DestReceiver *dest);
extern TupleDesc GetPGVariableResultDesc(const char *name);

extern void ExecSetVariableStmt(VariableSetStmt *stmt, bool isTopLevel);
extern char *ExtractSetVariableArgs(VariableSetStmt *stmt);

extern void ProcessGUCArray(ArrayType *array,
							GucContext context, GucSource source, GucAction action);
extern ArrayType *GUCArrayAdd(ArrayType *array, const char *name, const char *value);
extern ArrayType *GUCArrayDelete(ArrayType *array, const char *name);
extern ArrayType *GUCArrayReset(ArrayType *array);

#ifdef EXEC_BACKEND
extern void write_nondefault_variables(GucContext context);
extern void read_nondefault_variables(void);
#endif

extern void *guc_malloc(int elevel, size_t size);

/* GUC serialization */
extern Size EstimateGUCStateSpace(void);
extern void SerializeGUCState(Size maxsize, char *start_address);
extern void RestoreGUCState(void *gucstate);

/* Support for messages reported from GUC check hooks */

extern PGDLLIMPORT char *GUC_check_errmsg_string;
extern PGDLLIMPORT char *GUC_check_errdetail_string;
extern PGDLLIMPORT char *GUC_check_errhint_string;

extern void GUC_check_errcode(int sqlerrcode);

#define GUC_check_errmsg \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	GUC_check_errmsg_string = format_elog_string

#define GUC_check_errdetail \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	GUC_check_errdetail_string = format_elog_string

#define GUC_check_errhint \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	GUC_check_errhint_string = format_elog_string


/*
 * The following functions are not in guc.c, but are declared here to avoid
 * having to include guc.h in some widely used headers that it really doesn't
 * belong in.
 */

/* in commands/tablespace.c */
extern bool check_default_tablespace(char **newval, void **extra, GucSource source);
extern bool check_temp_tablespaces(char **newval, void **extra, GucSource source);
extern void assign_temp_tablespaces(const char *newval, void *extra);

/* in catalog/namespace.c */
extern bool check_search_path(char **newval, void **extra, GucSource source);
extern void assign_search_path(const char *newval, void *extra);

/* in access/transam/xlog.c */
extern bool check_wal_buffers(int *newval, void **extra, GucSource source);
extern void assign_xlog_sync_method(int new_sync_method, void *extra);

/* in access/transam/xlogprefetcher.c */
extern bool check_recovery_prefetch(int *new_value, void **extra, GucSource source);
extern void assign_recovery_prefetch(int new_value, void *extra);

/* POLAR */
extern char *polar_commit;
extern char *polar_version;
extern char *polardb_version;
extern char *polar_release_date;
extern char *polar_instance_name;

extern bool polar_allow_huge_alloc;
extern bool polar_disable_escape_inside_gbk_character;
extern bool polar_enable_stat_wait_info;
extern bool polar_enable_promote_wait_for_walreceive_done;
extern bool polar_enable_track_lock_stat;
extern bool polar_enable_track_lock_timing;
extern bool polar_enable_track_network_stat;
extern bool polar_enable_track_network_timing;

/* POLAR px */
typedef struct PxFunctionOidArray
{
	int			count;
	int			oid[FLEXIBLE_ARRAY_MEMBER];
} PxFunctionOidArray;

extern void *guc_malloc(int elevel, size_t size);
extern int	guc_name_compare(const char *namea, const char *nameb);

extern bool px_enable_check_workers;
extern bool px_info_debug;
extern bool px_enable_replay_wait;
extern bool px_enable_transaction;
extern bool px_enable_plan_cache;
extern bool px_enable_sort_distinct;
extern bool px_enable_join_prefetch_inner;
extern int	px_max_workers_number;
extern bool px_enable_plpgsql;
extern bool px_enable_procedure;
extern bool px_enable_prepare_statement;
extern bool px_enable_check_csn;
extern bool px_enable_create_table_as;

extern bool px_execute_pruned_plan;
extern bool px_enable_print;
extern bool px_debug_cancel_print;
extern bool px_log_dispatch_stats;
extern bool px_optimizer_enable_relsize_collection;

/* Macros to define the level of memory accounting to show in EXPLAIN ANALYZE */
#define EXPLAIN_MEMORY_VERBOSITY_SUPPRESS	0	/* Suppress memory reporting
												 * in explain analyze */
#define EXPLAIN_MEMORY_VERBOSITY_SUMMARY	1	/* Summary of memory usage for
												 * each owner in explain
												 * analyze */
#define EXPLAIN_MEMORY_VERBOSITY_DETAIL		2	/* Detail memory accounting
												 * tree for each slice in
												 * explain analyze */

/* Optimizer related gucs */
extern bool polar_enable_px;
extern bool px_enable_executor;
extern bool px_enable_join;
extern bool px_enable_window_function;
extern bool px_enable_subquery;
extern int	px_dop_per_node;
extern bool px_enable_cte;
extern bool px_enable_spi_read_all_namespaces;
extern bool px_enable_transaction;
extern bool px_enable_join_prefetch_inner;
extern bool px_enable_check_workers;

/* ORCA related definitions */
#define OPTIMIZER_XFORMS_COUNT	400 /* number of transformation rules */

/* types of optimizer failures */
#define OPTIMIZER_ALL_FAIL 			0	/* all failures */
#define OPTIMIZER_UNEXPECTED_FAIL 	1	/* unexpected failures */
#define OPTIMIZER_EXPECTED_FAIL 	2	/* expected failures */

/* optimizer minidump mode */
#define OPTIMIZER_MINIDUMP_FAIL		0	/* create optimizer minidump on
										 * failure */
#define OPTIMIZER_MINIDUMP_ALWAYS 	1	/* always create optimizer minidump */

/* optimizer cost model */
#define OPTIMIZER_GPDB_CALIBRATED	1	/* GPDB's calibrated cost model */
#define OPTIMIZER_POLARDB			2	/* PolarDB's cost model */

/*  TODO: pg15_px: adjust it to future Shared Server related options */
#define GUC_PX_NEED_SYNC		0x40000000	/* guc value is synced between
											 * master and primary */
#define GUC_PX_NO_SYNC			0x80000000	/* guc value is not synced between
											 * master and primary */

/* array of xforms disable flags */
extern bool optimizer_xforms[OPTIMIZER_XFORMS_COUNT];
extern char *px_optimizer_search_strategy_path;

/* GUCs to tell Optimizer to enable a physical operator */
extern bool px_optimizer_enable_indexjoin;
extern bool px_optimizer_enable_motions_masteronly_queries;
extern bool px_optimizer_enable_motions;
extern bool px_optimizer_enable_motion_broadcast;
extern bool px_optimizer_enable_motion_gather;
extern bool px_optimizer_enable_motion_redistribute;
extern bool px_optimizer_enable_sort;
extern bool px_optimizer_enable_materialize;
extern bool px_optimizer_enable_partition_propagation;
extern bool px_optimizer_enable_partition_selection;
extern bool px_optimizer_enable_outerjoin_rewrite;
extern bool px_optimizer_enable_multiple_distinct_aggs;
extern bool px_optimizer_enable_hashjoin_redistribute_broadcast_children;
extern bool px_optimizer_enable_broadcast_nestloop_outer_child;
extern bool px_optimizer_enable_streaming_material;
extern bool px_optimizer_enable_gather_on_segment_for_dml;
extern bool px_optimizer_enable_assert_maxonerow;
extern bool px_optimizer_enable_constant_expression_evaluation;
extern bool px_optimizer_enable_outerjoin_to_unionall_rewrite;
extern bool px_optimizer_enable_ctas;
extern bool px_optimizer_enable_dml;
extern bool px_optimizer_enable_dml_triggers;
extern bool px_optimizer_enable_dml_constraints;
extern bool px_optimizer_enable_direct_dispatch;
extern bool px_optimizer_enable_master_only_queries;
extern bool px_optimizer_enable_dynamictablescan;
extern bool px_optimizer_enable_dynamicindexscan;
extern bool px_optimizer_expand_fulljoin;
extern bool px_optimizer_enable_hashagg;
extern bool px_optimizer_enable_groupagg;
extern bool px_optimizer_enable_mergejoin;
extern bool px_optimizer_enable_hashjoin;
extern bool px_optimizer_enable_seqscan;
extern bool px_optimizer_enable_seqsharescan;
extern bool px_optimizer_enable_indexscan;
extern bool px_optimizer_enable_shareindexscan;
extern bool px_optimizer_enable_dynamicshareindexscan;
extern bool px_optimizer_enable_indexonlyscan;
extern bool px_optimizer_enable_brinscan;
extern bool px_optimizer_enable_bitmapscan;
extern bool px_optimizer_enable_nestloopjoin;
extern bool px_optimizer_enable_lasj_notin;
extern bool px_optimizer_enable_crossproduct;

/* Optimizer related gucs */
extern bool px_optimizer_log;
extern int	px_optimizer_log_failure;
extern bool px_optimizer_trace_fallback;
extern int	px_optimizer_cost_model;
extern bool px_optimizer_metadata_caching;
extern int	px_optimizer_mdcache_size;

/* Optimizer debugging GUCs */
extern bool px_optimizer_print_query;
extern bool px_optimizer_print_plan;
extern bool px_optimizer_print_xform;
extern bool px_optimizer_print_memo_after_exploration;
extern bool px_optimizer_print_memo_after_implementation;
extern bool px_optimizer_print_memo_after_optimization;
extern bool px_optimizer_print_job_scheduler;
extern bool px_optimizer_print_expression_properties;
extern bool px_optimizer_print_group_properties;
extern bool px_optimizer_print_optimization_context;
extern bool px_optimizer_print_optimization_stats;
extern bool px_optimizer_print_xform_results;
extern bool px_optimizer_print_memo_enforcement;
extern bool px_optimizer_print_required_columns;
extern bool px_optimizer_print_equiv_distr_specs;

/* Optimizer plan enumeration related GUCs */
extern bool px_optimizer_enumerate_plans;
extern bool px_optimizer_sample_plans;
extern int	px_optimizer_plan_id;
extern int	px_optimizer_samples_number;

/* Cardinality estimation related GUCs used by the Optimizer */
extern bool px_optimizer_extract_dxl_stats;
extern bool px_optimizer_extract_dxl_stats_all_nodes;
extern bool px_optimizer_print_missing_stats;
extern bool px_optimizer_dpe_stats;
extern bool px_optimizer_enable_derive_stats_all_groups;
extern double px_optimizer_damping_factor_filter;
extern double px_optimizer_damping_factor_join;
extern double px_optimizer_damping_factor_groupby;

/* Costing or tuning related GUCs used by the Optimizer */
extern int	px_optimizer_segments;
extern int	px_optimizer_penalize_broadcast_threshold;
extern double px_optimizer_cost_threshold;
extern double px_optimizer_nestloop_factor;
extern double px_optimizer_sort_factor;
extern double px_optimizer_share_tablescan_factor;
extern double px_optimizer_share_indexscan_factor;

/* Optimizer hints */
extern int	px_optimizer_array_expansion_threshold;
extern int	px_optimizer_join_order_threshold;
extern int	px_optimizer_join_arity_for_associativity_commutativity;
extern int	px_optimizer_cte_inlining_bound;
extern int	px_optimizer_push_group_by_below_setop_threshold;
extern bool px_optimizer_force_multistage_agg;
extern bool px_optimizer_force_expanded_distinct_aggs;
extern bool px_optimizer_force_agg_skew_avoidance;
extern bool px_optimizer_penalize_skew;
extern bool px_optimizer_prune_computed_columns;
extern bool px_optimizer_push_requirements_from_consumer_to_producer;
extern bool px_optimizer_enforce_subplans;
extern bool px_optimizer_apply_left_outer_to_union_all_disregarding_stats;
extern bool px_optimizer_use_external_constant_expression_evaluation_for_ints;
extern bool px_optimizer_remove_order_below_dml;
extern bool px_optimizer_multilevel_partitioning;
extern bool px_optimizer_cte_inlining;
extern bool px_optimizer_enable_space_pruning;
extern bool px_optimizer_use_px_allocators;
extern bool px_enable_opfamily_for_distribution;

extern int	px_optimizer_minidump;
extern int	px_optimizer_join_order;
extern bool px_optimizer_force_three_stage_scalar_dqa;
extern bool px_optimizer_parallel_union;
extern bool px_optimizer_array_constraints;
extern bool px_optimizer_enable_eageragg;
extern bool px_optimizer_prune_unused_columns;
extern bool px_optimizer_enable_associativity;

/* GUCs for slice table*/
extern int	px_max_slices;
extern char *polar_px_nodes;
extern char *polar_px_ignore_function;
extern PxFunctionOidArray *px_function_oid_array;
extern bool px_use_master;
extern bool px_use_standby;
extern bool polar_px_ignore_unusable_nodes;
extern bool polar_enable_send_node_info;
extern bool polar_enable_send_cluster_info;

/* The number of blocks to scan table */
extern int	px_scan_unit_size;
extern int	px_scan_unit_bit;
extern bool px_enable_adaptive_scan;
extern bool px_enable_adps_explain_analyze;

extern bool px_interconnect_udpic_network_enable_ipv6;
extern bool px_enable_dispatch_async;
extern bool px_enable_udp_testmode;
extern bool px_enable_tcp_testmode;
extern bool px_enable_remove_redundant_results;
extern bool px_enable_btbuild;
extern bool px_enable_btbuild_cic_phase2;
extern int	px_btbuild_batch_size;
extern int	px_btbuild_mem_size;
extern int	px_btbuild_queue_size;
extern bool px_enable_cte_shared_scan;
extern int	polar_bt_write_page_buffer_size;
extern bool px_enable_partition;
extern bool px_enable_partition_hash;
extern bool px_enable_relsize_collection;
extern bool px_enable_tableless_scan;
extern bool px_enable_pre_optimizer_check;
extern bool px_enable_left_index_nestloop_join;
extern bool px_enable_result_hash_filter;
extern bool px_enable_insert_select;
extern bool px_enable_insert_partition_table;
extern int	px_wait_lock_timeout;
extern int	px_insert_dop_num;
extern bool px_enable_insert_from_tableless;
extern bool px_enable_insert_order_sensitive;
extern bool px_enable_partitionwise_join;
extern bool px_enable_update;
extern int	px_update_dop_num;
extern bool px_enable_delete;
extern int	px_delete_dop_num;
extern bool px_optimizer_remove_superfluous_order;
extern bool px_enable_sort_distinct;

extern bool px_allow_strat_seqscan;
extern bool px_allow_strat_seqscan;
extern bool px_allow_strat_bitmapscan;
extern bool px_allow_sync_seqscan;
extern bool px_allow_sync_bitmapscan;
extern bool px_allow_pagemode_seqscan;
extern bool px_allow_pagemode_bitmapscan;

/* Polar QC authentication */
extern char *px_peer_cert_file;
extern char *px_peer_key_file;
extern char *px_ca_file;
extern bool px_enable_auth_on_qe;

/* optimizer join heuristic models */
#define JOIN_ORDER_IN_QUERY				0
#define JOIN_ORDER_GREEDY_SEARCH		1
#define JOIN_ORDER_EXHAUSTIVE_SEARCH	2
#define JOIN_ORDER_EXHAUSTIVE2_SEARCH	3

extern void px_need_reload_ca(const char *newval, void *extra);
extern void px_need_reload_cert_pkey(const char *newval, void *extra);

/* POLAR end */

extern bool polar_find_in_string_list(const char *itemname, const char *stringlist);

#endif							/* GUC_H */
