/*-------------------------------------------------------------------------
 *
 * px_vars.c
 *	  Provides storage areas and processing routines for Greenplum Database variables
 *	  managed by GUC.
 *
 * Portions Copyright (c) 2003-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/px_vars.c
 *
 *
 * NOTES
 *	  See src/backend/utils/misc/guc.c for variable external specification.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include "px/px_util.h"
#include "px/px_vars.h"
#include "parser/parse_func.h"

/*
 * ----------------
 *		GUC/global variables
 *
 *	Initial values are set by guc.c function "InitializeGUCOptions" called
 *	*very* early during postmaster, postgres, or bootstrap initialization.
 * ----------------
 */

PxRoleValue px_role = PX_ROLE_QC;	/* Role paid by this Greenplum Database
									 * backend */
bool		px_is_writer;		/* is this qExec a "writer" process. */

int			px_session_id = InvalidPxSessionId; /* global unique id for
												 * session. */

struct TraceId sql_trace_id;

char	   *px_auth_cert = NULL;	/* PX QC authentication certificate NULL
									 * terminated */
char	   *px_qc_hostname;		/* QC hostname */
int			px_qc_port;			/* Master Segment Postmaster port. */

const char	PX_INTERNAL_USER[] = "px-internal";

int			px_command_count;	/* num of commands from client */

bool		debug_print_slice_table;	/* Shall we log the slice table? */

int			px_cached_px_workers;	/* How many gangs to keep around from stmt
									 * to stmt. */

int			px_worker_connect_timeout = 180;	/* Maximum time (in seconds)
												 * allowed for a new worker
												 * process to start or a
												 * mirror to respond. */

/*
 * When we have certain types of failures during gang creation which indicate
 * that a segment is in recovery mode we may be able to retry.
 */
int			px_gang_creation_retry_count = 5;	/* disable by default */
int			px_gang_creation_retry_timer = 2000;	/* 2000ms */

/*
 * TCP port the Interconnect listens on for incoming connections from other
 * backends.  Assigned by initMotionLayerIPC() at process startup.  This port
 * is used for the duration of this process and should never change.
 */
uint32		px_listener_port;

int			px_interconnect_max_packet_size;	/* max Interconnect packet
												 * size */

int			px_interconnect_queue_depth = 4;	/* max number of messages
												 * waiting in rx-queue before
												 * we drop. */
int			px_interconnect_snd_queue_depth = 2;
int			px_interconnect_timer_period = 5;
int			px_interconnect_timer_checking_period = 20;
int			px_interconnect_default_rtt = 20;
int			px_interconnect_min_rto = 20;
int			px_interconnect_fc_method = INTERCONNECT_FC_METHOD_LOSS;
int			px_interconnect_transmit_timeout = 3600;
int			px_interconnect_min_retries_before_timeout = 100;
int			px_interconnect_debug_retry_interval = 10;

int			interconnect_setup_timeout = 7200;

int			px_interconnect_type = INTERCONNECT_TYPE_UDPIFC;
bool		px_use_global_function = false;

/* listener backlog is calculated at listener-creation time */
int			px_interconnect_tcp_listener_backlog = 128;

bool		px_interconnect_aggressive_retry = true;	/* fast-track app-level
														 * retry */

bool		px_interconnect_full_crc = false;	/* sanity check UDP data. */

bool		px_interconnect_log_stats = false;	/* emit stats at log-level */

bool		px_interconnect_cache_future_packets = true;

int			px_interconnect_udp_bufsize_k;	/* UPD recv buf size, in KB */

#ifdef USE_ASSERT_CHECKING
/*
 * UDP-IC Test hooks (for fault injection).
 *
 * Dropseg: specifies which segment to apply the drop_percent to.
 */
int			px_interconnect_udpic_dropseg = UNDEF_SEGMENT;
int			px_interconnect_udpic_dropxmit_percent = 0;
int			px_interconnect_udpic_dropacks_percent = 0;
int			px_interconnect_udpic_fault_inject_percent = 0;
int			px_interconnect_udpic_fault_inject_bitmap = 0;
#endif

/*
 * Each slice table has a unique ID (certain commands like "vacuum analyze"
 * run many many slice-tables for each px_command_id).
 */
uint32		px_interconnect_id = 0;

/* --------------------------------------------------------------------------------------------------
 * Greenplum Optimizer GUCs
 */

double		px_motion_cost_per_row = 0;

double		px_selectivity_damping_factor = 1;

/* Analyzing aid */
int			px_motion_slice_noop = 0;

/* Database Experimental Feature GUCs */
bool		px_enable_explain_all_stat = false;

#ifdef USE_ASSERT_CHECKING
bool		px_mk_sort_check = false;
#endif

/* Max size of dispatched plans; 0 if no limit */
int			px_max_plan_size = 0;

int			px_workfile_caching_loglevel = DEBUG1;

/* Enable single-mirror pair dispatch. */
bool		px_enable_direct_dispatch = true;

/* Force core dump on memory context error */
bool		coredump_on_memerror = false;

/* ----------------
 * Non-GUC globals
 */

int			currentSliceId = UNSET_SLICE_ID;	/* used by elog to show the
												 * current slice the process
												 * is executing. */

/* ----------------
 * This variable is initialized by the postmaster from command line arguments
 *
 * Any code needing the "numsegments"
 * can simply #include pxvars.h, and use PxIdentity.numsegments
 */
PxId		PxIdentity = {UNINITIALIZED_PX_IDENTITY_VALUE, UNINITIALIZED_PX_IDENTITY_VALUE};

/*
 * Keep track of a few dispatch-related  statistics:
 */
int			px_total_slices = 0;
int			px_total_plans = 0;
int			px_serialize_version = 1;
char	   *px_workerid_funcname = "polar_px_workerid";
Oid			px_workerid_funcid = InvalidOid;

/*
 * Implements the makePXWorkerIndexFilterExpr() function to return the worker index
 * of the current worker.
 */
int32
get_px_workerid(void)
{
	return PxIdentity.workerid;
}

uint32
get_px_workerid_funcid(void)
{
	update_px_workerid_funcid();
	return px_workerid_funcid;
}

void
update_px_workerid_funcid(void)
{
	List	   *funcname = NIL;
	Oid			fargtypes[1];	/* dummy */

	funcname = list_make2(makeString("public"), makeString(px_workerid_funcname));
	px_workerid_funcid = LookupFuncName(funcname, 0, fargtypes, true);

	if (px_workerid_funcid == InvalidOid)
		elog(ERROR, "polar_px: load polar_px_workerid() failed, try \"CREATE EXTENSION polar_px;\"");
}

bool
polar_is_stmt_enable_px(void)
{
	return polar_enable_px;
}

int
polar_get_stmt_px_dop(void)
{
	return (px_use_global_function
			? 1
			: px_dop_per_node);
}

/* --------------------------------------------------------------------------------------------------
 * Logging
 */


/*
 * px_log_gangs (string)
 *
 * Should creation, reallocation and cleanup of gangs of PX processes be logged?
 * "OFF"	 -> only errors are logged
 * "TERSE"	 -> terse logging of routine events, e.g. creation of new qExecs
 * "VERBOSE" -> gang allocation per command is logged
 * "DEBUG"	 -> additional events are logged at severity level DEBUG1 to DEBUG5
 *
 * The messages that are enabled by the TERSE and VERBOSE settings are
 * written with a severity level of LOG.
 */
int			px_log_gang;

/*
 * px_interconnect_log  (string)
 *
 * Should connections between internal processes be logged?  (qDisp/qExec/etc)
 * "OFF"	 -> connection errors are logged
 * "TERSE"	 -> terse logging of routine events, e.g. successful connections
 * "VERBOSE" -> most interconnect setup events are logged
 * "DEBUG"	 -> additional events are logged at severity level DEBUG1 to DEBUG5.
 *
 * The messages that are enabled by the TERSE and VERBOSE settings are
 * written with a severity level of LOG.
 */
int			px_interconnect_log;

bool		px_is_executing;
bool		px_is_planning = false;
bool		px_adaptive_paging;
bool		px_adaptive_scan_setup = false;
bool		px_prefetch_inner_executing = false;
bool		cached_px_enable_replay_wait;

/* Parallel DML */
int			local_px_insert_dop_num;
