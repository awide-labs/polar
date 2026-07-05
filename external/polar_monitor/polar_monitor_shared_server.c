/*-------------------------------------------------------------------------
 *
 * polar_monitor_shared_server.c
 *	  display some information of polardb buffer.
 *
 *	  external/polar_monitor/polar_monitor_shared_server.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "access/htup_details.h"
#include "funcapi.h"
#include "postmaster/polar_dispatcher.h"
#include "nodes/polar_double_linked_list.h"
#include "pgstat.h"
#include "catalog/pg_authid_d.h"
#include "storage/proc.h"
#include "utils/acl.h"

PG_FUNCTION_INFO_V1(polar_stat_dispatcher);
PG_FUNCTION_INFO_V1(polar_stat_session);
PG_FUNCTION_INFO_V1(polar_dispatcher_pid);
PG_FUNCTION_INFO_V1(polar_session_backend_pid);
PG_FUNCTION_INFO_V1(polar_is_dedicated_backend);

typedef struct DispatcherStateContext
{
	int			dispatcher_id;
	TupleDesc	ret_desc;
} DispatcherStateContext;

/**
 * Return information about dispatcher state.
 * This set-returning functions returns the following columns:
 *
 * id			  			- proxy process identifier
 * pid			  			- proxy process pid
 * n_clients	  			- number of clients connected to dispatcher
 * n_ssl_clients  			- number of clients using SSL protocol
 * n_idle_clients  			- number of idle clients
 * n_pending_clients  		- number of pending clients
 * n_signalling_clients  	- number of signalling clients
 * n_startup_clients  		- number of startup clients
 * n_pools		  			- number of pools (role/dbname combinations) maintained by dispatcher
 * n_backends	  			- total number of backends spawned by this dispatcher (including tainted)
 * n_idle_backends	  		- total number of idle backends spawned by this dispatcher (including tainted)
 * n_dedicated_backends - number of tainted backend
 * tx_bytes		  - amount of data sent from backends to clients
 * rx_bytes		  - amount of data sent from client to backends
 * n_transactions - number of transaction proceeded by all backends of this dispatcher
 */
Datum
polar_stat_dispatcher(PG_FUNCTION_ARGS)
{
	FuncCallContext *srf_ctx;
	MemoryContext old_context;
	DispatcherStateContext *ps_ctx;
	HeapTuple	tuple;
	Datum		values[15];
	bool		nulls[15] = {0};
	int			id;
	int			i = 0;

	if (SRF_IS_FIRSTCALL())
	{
		srf_ctx = SRF_FIRSTCALL_INIT();
		old_context = MemoryContextSwitchTo(srf_ctx->multi_call_memory_ctx);
		ps_ctx = (DispatcherStateContext *) palloc(sizeof(DispatcherStateContext));
		get_call_result_type(fcinfo, NULL, &ps_ctx->ret_desc);
		ps_ctx->dispatcher_id = 0;
		srf_ctx->user_fctx = ps_ctx;
		MemoryContextSwitchTo(old_context);
	}
	srf_ctx = SRF_PERCALL_SETUP();
	ps_ctx = srf_ctx->user_fctx;
	id = ps_ctx->dispatcher_id;
	if (id == polar_ss_dispatcher_count || !POLAR_SHARED_SERVER_RUNNING())
		SRF_RETURN_DONE(srf_ctx);

	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].id);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].pid);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_clients);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_ssl_clients);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_idle_clients);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_pending_clients);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_signalling_clients);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_startup_clients);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_pools);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_backends);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_idle_backends);
	values[i++] = Int32GetDatum(polar_dispatcher_proc[id].n_dedicated_backends);
	values[i++] = Int64GetDatum(polar_dispatcher_proc[id].tx_bytes);
	values[i++] = Int64GetDatum(polar_dispatcher_proc[id].rx_bytes);
	values[i++] = Int64GetDatum(polar_dispatcher_proc[id].n_transactions);

	Assert(i == sizeof(nulls) / sizeof(bool));

	ps_ctx->dispatcher_id += 1;
	tuple = heap_form_tuple(ps_ctx->ret_desc, values, nulls);
	SRF_RETURN_NEXT(srf_ctx, HeapTupleGetDatum(tuple));
}

/*
 * Returns activity of PG shared session.
 */
Datum
polar_stat_session(PG_FUNCTION_ARGS)
{
#define POLAR_GET_REAL_PID_ARG 			(-2)
#define UINT32_ACCESS_ONCE(var)			((uint32)(*((volatile uint32 *)&(var))))
#define HAS_PGSTAT_PERMISSIONS(role)	 (has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) || has_privs_of_role(GetUserId(), role))
#define POLAR_STAT_GET_ACTIVITY_COLS	30
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	int			pid = PG_ARGISNULL(0) ? -1 : PG_GETARG_INT32(0);
	bool		get_real_pid = false;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* POLAR: when we pass POLAR_GET_REAL_PID_ARG(-2), means get all real pid */
	if (pid == POLAR_GET_REAL_PID_ARG)
	{
		get_real_pid = true;
		pid = -1;
	}
	/* POLAR: try to get real pid, the pid here might be a virtual pid */
	if (POLAR_IS_VIRTUAL_PID(pid))
		pid = polar_proxy_get_pid(pid, 0, false);

	/* 1-based index */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		/* for each row */
		Datum		values[POLAR_STAT_GET_ACTIVITY_COLS];
		bool		nulls[POLAR_STAT_GET_ACTIVITY_COLS];
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;
		PGPROC	   *proc;
		const char *wait_event_type = NULL;
		const char *wait_event = NULL;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* Get the next one in the list */
		local_beentry = pgstat_fetch_stat_local_beentry(curr_backend);
		if (!local_beentry)
		{
			/* Ignore missing entries if looking for specific PID */
			if (pid != -1)
				continue;

			MemSet(nulls, true, sizeof(nulls));

			nulls[5] = false;
			values[5] = CStringGetTextDatum("<backend information not available>");

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			continue;
		}

		beentry = &local_beentry->backendStatus;

		if (beentry->st_backendType != B_BACKEND)
			continue;

		/* If looking for specific PID, ignore all the others */
		if (pid != -1 && beentry->st_procpid != pid)
			continue;

		/* Values available to all callers */
		if (beentry->st_databaseid != InvalidOid)
			values[0] = ObjectIdGetDatum(beentry->st_databaseid);
		else
			nulls[0] = true;

		/* POLAR: return virtual pid if available */
		if (get_real_pid)
			values[1] = Int32GetDatum(beentry->st_procpid);
		else
			values[1] = Int32GetDatum(polar_get_session_id_by_beentry(beentry));

		if (beentry->st_userid != InvalidOid)
			values[2] = ObjectIdGetDatum(beentry->st_userid);
		else
			nulls[2] = true;

		if (beentry->st_appname)
			values[3] = CStringGetTextDatum(beentry->st_appname);
		else
			nulls[3] = true;

		if (TransactionIdIsValid(local_beentry->backend_xid))
			values[15] = TransactionIdGetDatum(local_beentry->backend_xid);
		else
			nulls[15] = true;

		if (TransactionIdIsValid(local_beentry->backend_xmin))
			values[16] = TransactionIdGetDatum(local_beentry->backend_xmin);
		else
			nulls[16] = true;

		if (beentry->st_ssl)
		{
			values[18] = BoolGetDatum(true);	/* ssl */
			values[19] = CStringGetTextDatum(beentry->st_sslstatus->ssl_version);
			values[20] = CStringGetTextDatum(beentry->st_sslstatus->ssl_cipher);
			values[21] = Int32GetDatum(beentry->st_sslstatus->ssl_bits);
			if (beentry->st_sslstatus->ssl_client_dn[0])
				values[22] = CStringGetTextDatum(beentry->st_sslstatus->ssl_client_dn);
			else
				nulls[22] = true;

			if (beentry->st_sslstatus->ssl_client_serial[0])
				values[23] = DirectFunctionCall3(numeric_in,
												 CStringGetDatum(beentry->st_sslstatus->ssl_client_serial),
												 ObjectIdGetDatum(InvalidOid),
												 Int32GetDatum(-1));
			else
				nulls[23] = true;

			if (beentry->st_sslstatus->ssl_issuer_dn[0])
				values[24] = CStringGetTextDatum(beentry->st_sslstatus->ssl_issuer_dn);
			else
				nulls[24] = true;
		}
		else
		{
			values[18] = BoolGetDatum(false);	/* ssl */
			nulls[19] = nulls[20] = nulls[21] = nulls[22] = nulls[23] = true;
			nulls[24] = true;
		}

		values[25] = Int32GetDatum(beentry->dispatcher_pid);
		values[26] = Int32GetDatum(beentry->session_local_id);
		values[27] = Int32GetDatum(beentry->last_backend_pid);
		values[28] = Int32GetDatum(beentry->saved_guc_count);
		if (beentry->last_wait_start_timestamp != 0)
			values[29] = TimestampTzGetDatum(beentry->last_wait_start_timestamp);
		else
			nulls[29] = true;

		/* Values only available to role member or pg_read_all_stats */
		if (HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		{
			SockAddr	zero_clientaddr;
			char	   *clipped_activity;

			switch (beentry->st_state)
			{
				case STATE_IDLE:
					values[4] = CStringGetTextDatum("idle");
					break;
				case STATE_RUNNING:
					values[4] = CStringGetTextDatum("active");
					break;
				case STATE_IDLEINTRANSACTION:
					values[4] = CStringGetTextDatum("idle in transaction");
					break;
				case STATE_FASTPATH:
					values[4] = CStringGetTextDatum("fastpath function call");
					break;
				case STATE_IDLEINTRANSACTION_ABORTED:
					values[4] = CStringGetTextDatum("idle in transaction (aborted)");
					break;
				case STATE_DISABLED:
					values[4] = CStringGetTextDatum("disabled");
					break;
				case STATE_UNDEFINED:
					nulls[4] = true;
					break;
			}

			clipped_activity = pgstat_clip_activity(beentry->st_activity_raw);
			values[5] = CStringGetTextDatum(clipped_activity);
			pfree(clipped_activity);

			proc = BackendPidGetProc(beentry->st_procpid);
			if (proc != NULL)
			{
				uint32		raw_wait_event;

				raw_wait_event = UINT32_ACCESS_ONCE(proc->wait_event_info);
				wait_event_type = pgstat_get_wait_event_type(raw_wait_event);
				wait_event = pgstat_get_wait_event(raw_wait_event);

			}

			if (wait_event_type)
				values[6] = CStringGetTextDatum(wait_event_type);
			else
				nulls[6] = true;

			if (wait_event)
				values[7] = CStringGetTextDatum(wait_event);
			else
				nulls[7] = true;

			/*
			 * Don't expose transaction time for walsenders; it confuses
			 * monitoring, particularly because we don't keep the time up-to-
			 * date.
			 */
			if (beentry->st_xact_start_timestamp != 0 &&
				beentry->st_backendType != B_WAL_SENDER)
				values[8] = TimestampTzGetDatum(beentry->st_xact_start_timestamp);
			else
				nulls[8] = true;

			if (beentry->st_activity_start_timestamp != 0)
				values[9] = TimestampTzGetDatum(beentry->st_activity_start_timestamp);
			else
				nulls[9] = true;

			if (beentry->st_proc_start_timestamp != 0)
				values[10] = TimestampTzGetDatum(beentry->st_proc_start_timestamp);
			else
				nulls[10] = true;

			if (beentry->st_state_start_timestamp != 0)
				values[11] = TimestampTzGetDatum(beentry->st_state_start_timestamp);
			else
				nulls[11] = true;

			/* A zeroed client addr means we don't know */
			memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
			if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
					   sizeof(zero_clientaddr)) == 0)
			{
				nulls[12] = true;
				nulls[13] = true;
				nulls[14] = true;
			}
			else
			{
				if (beentry->st_clientaddr.addr.ss_family == AF_INET
#ifdef HAVE_IPV6
					|| beentry->st_clientaddr.addr.ss_family == AF_INET6
#endif
					)
				{
					char		remote_host[NI_MAXHOST];
					char		remote_port[NI_MAXSERV];
					int			ret;

					SockAddr	real_sock;

					if (beentry->polar_proxy)
						real_sock = beentry->polar_proxy_client_raddr;
					else
						real_sock = beentry->st_clientaddr;

					remote_host[0] = '\0';
					remote_port[0] = '\0';
					ret = pg_getnameinfo_all(&real_sock.addr,
											 real_sock.salen,
											 remote_host, sizeof(remote_host),
											 remote_port, sizeof(remote_port),
											 NI_NUMERICHOST | NI_NUMERICSERV);
					if (ret == 0)
					{
						clean_ipv6_addr(real_sock.addr.ss_family, remote_host);
						values[12] = DirectFunctionCall1(inet_in,
														 CStringGetDatum(remote_host));
						if (beentry->st_clienthostname &&
							beentry->st_clienthostname[0] &&
							!beentry->polar_proxy)	/* POLAR: In proxy mode,
													 * we can't get the
													 * client's hostname */
							values[13] = CStringGetTextDatum(beentry->st_clienthostname);
						else
							nulls[13] = true;
						values[14] = Int32GetDatum(atoi(remote_port));
					}
					else
					{
						nulls[12] = true;
						nulls[13] = true;
						nulls[14] = true;
					}
				}
				else if (beentry->st_clientaddr.addr.ss_family == AF_UNIX)
				{
					/*
					 * Unix sockets always reports NULL for host and -1 for
					 * port, so it's possible to tell the difference to
					 * connections we have no permissions to view, or with
					 * errors.
					 */
					nulls[12] = true;
					nulls[13] = true;
					values[14] = Int32GetDatum(-1);
				}
				else
				{
					/* Unknown address type, should never happen */
					nulls[12] = true;
					nulls[13] = true;
					nulls[14] = true;
				}
			}
			/* Add backend type */
			if (beentry->dispatcher_pid > 0)
				values[17] = CStringGetTextDatum("client session");
			else
				values[17] =
					CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));
		}
		else
		{
			/* No permissions to view data about this session */
			values[5] = CStringGetTextDatum("<insufficient privilege>");
			nulls[4] = true;
			nulls[6] = true;
			nulls[7] = true;
			nulls[8] = true;
			nulls[9] = true;
			nulls[10] = true;
			nulls[11] = true;
			nulls[12] = true;
			nulls[13] = true;
			nulls[14] = true;
			nulls[17] = true;
		}

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/* If only a single backend was requested, and we found it, break. */
		if (pid != -1)
			break;
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

Datum
polar_dispatcher_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(POLAR_SHARED_SERVER_RUNNING() ?
					polar_dispatcher_proc[polar_my_dispatcher_id].pid : -1);
}

Datum
polar_session_backend_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(polar_get_session_id(MyProcPid));
}

Datum
polar_is_dedicated_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!POLAR_SS_NOT_DEDICATED());
}
