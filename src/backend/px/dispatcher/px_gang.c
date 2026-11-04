/*-------------------------------------------------------------------------
 *
 * px_gang.c
 *	  Query Executor Factory for gangs of PXs.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	  src/backend/px/dispatcher/px_gang.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/variable.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"			/* MyProcPid */
#include "nodes/execnodes.h"	/* PxProcess, Slice, SliceTable */
#include "pgstat.h"				/* pgstat_report_sessionid() */
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "tcop/pquery.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/guc.h"

#include "px/px_conn.h"			/* PxWorkerDescriptor */
#include "px/px_disp.h"
#include "px/px_disp_query.h"
#include "px/px_gang.h"			/* me */
#include "px/px_gang_async.h"
#include "px/px_util.h"			/* PxNodeInfo */
#include "px/px_vars.h"			/* px_role, etc. */
#include "utils/faultinjector.h"
#include "utils/builtins.h"

/*
 * All PXs are managed by px_workers in QC, QC assigned
 * a unique identifier for each PX, when a PX is created, this
 * identifier is passed along with pxid params, see
 * pxgang_parse_pxid_params()
 *
 * px_identifier is use to go through slice table and find which slice
 * this PX should execute.
 */
int			px_identifier = 0;

/*
 * size of hash table of interconnect connections
 * equals to 2 * (the number of total segments)
 */
int			ic_htab_size = 0;
int			px_logical_worker_idx = -1;
int			px_logical_total_workers = -1;

/* version for compat future, different with remote means not compat*/
uint64		px_compat_version = 2;

/* incluse version and self, used for decode member */
uint64		px_serialize_member_count = 11;

/* version for wal sync */
uint64		px_sql_wal_lsn = 0;

/* value of systemid for authorization */
uint64		px_auth_systemid = 0;

Gang	   *CurrentGangCreating = NULL;

CreateGangFunc pCreateGangFunc = pxgang_createGang_async;

/*
 * pxgang_createGang:
 *
 * Creates a new gang by logging on a session to each segDB involved.
 *
 * call this function in GangContext memory context.
 * elog ERROR or return a non-NULL gang.
 */
Gang *
pxgang_createGang(List *segments, SegmentType segmentType)
{
	Assert(pCreateGangFunc);

	return pCreateGangFunc(segments, segmentType);
}

/*
 * Creates a new gang by logging on a session to each segDB involved.
 *
 * elog ERROR or return a non-NULL gang.
 */
Gang *
AllocateGang(PxDispatcherState *ds, GangType type, List *segments)
{
	MemoryContext oldContext;
	SegmentType segmentType;
	Gang	   *newGang = NULL;
	int			i;

	ELOG_DISPATCHER_DEBUG("AllocateGang begin.");

	if (px_role != PX_ROLE_QC)
	{
		elog(FATAL, "dispatch process called with role %d", px_role);
	}

	if (segments == NIL)
		return NULL;

	Assert(px_DispatcherContext);
	oldContext = MemoryContextSwitchTo(px_DispatcherContext);

	if (type == GANGTYPE_PRIMARY_WRITER)
		segmentType = SEGMENTTYPE_EXPLICT_WRITER;
	/* for extended query like cursor, must specify a reader */
	else if (ds->isExtendedQuery)
		segmentType = SEGMENTTYPE_EXPLICT_READER;
	else
		segmentType = SEGMENTTYPE_ANY;

	newGang = pxgang_createGang(segments, segmentType);
	newGang->allocated = true;
	newGang->type = type;

	/*
	 * Push to the head of the allocated list, later in
	 * pxdisp_destroyDispatcherState() we should recycle them from the head to
	 * restore the original order of the idle gangs.
	 */
	ds->allocatedGangs = lcons(newGang, ds->allocatedGangs);
	ds->largestGangSize = Max(ds->largestGangSize, newGang->size);

	ELOG_DISPATCHER_DEBUG("AllocateGang end.");

	if (type == GANGTYPE_PRIMARY_WRITER)
	{
		/*
		 * set "whoami" for utility statement. non-utility statement will
		 * overwrite it in function getPxProcessList.
		 */
		for (i = 0; i < newGang->size; i++)
			pxconn_setPXIdentifier(newGang->db_descriptors[i], -1);
	}

	MemoryContextSwitchTo(oldContext);

	return newGang;
}

/*
 * Check the segment failure reason by comparing connection error message.
 */
bool
segment_failure_due_to_recovery(const char *error_message)
{
	char	   *fatal = NULL,
			   *ptr = NULL;
	int			fatal_len = 0;

	if (error_message == NULL)
		return false;

	fatal = gettext("FATAL");
	fatal_len = strlen(fatal);

	/*
	 * it would be nice if we could check errcode for
	 * ERRCODE_CANNOT_CONNECT_NOW, instead we wind up looking for at the
	 * strings.
	 *
	 * And because if LC_MESSAGES gets set to something which changes the
	 * strings a lot we have to take extreme care with looking at the string.
	 */
	ptr = strstr(error_message, fatal);
	if ((ptr != NULL) && ptr[fatal_len] == ':')
	{
		if (strstr(error_message, gettext(POSTMASTER_IN_STARTUP_MSG)))
		{
			return true;
		}
		if (strstr(error_message, gettext(POSTMASTER_IN_RECOVERY_MSG)))
		{
			return true;
		}
		/* We could do retries for "sorry, too many clients already" here too */
	}

	return false;
}

/*
 * Reads the GP catalog tables and build a PxNodes structure.
 * It then converts this to a Gang structure and initializes all the non-connection related fields.
 *
 * Call this function in GangContext.
 * Returns a not-null pointer.
 */
Gang *
buildGangDefinition(List *segments, SegmentType segmentType)
{
	Gang	   *newGangDefinition = NULL;
	ListCell   *lc;
	int			i = 0;
	int			totalPxNodes;
	int			workerId;

	totalPxNodes = list_length(segments);

	ELOG_DISPATCHER_DEBUG("buildGangDefinition:Starting %d qExec processes for gang", totalPxNodes);

	Assert(getPxWorkerCount() >= 1);
	Assert(CurrentMemoryContext == px_DispatcherContext);

	/* allocate a gang */
	newGangDefinition = (Gang *) palloc0(sizeof(Gang));
	newGangDefinition->type = GANGTYPE_UNALLOCATED;
	newGangDefinition->size = totalPxNodes;
	newGangDefinition->allocated = false;
	newGangDefinition->db_descriptors =
		(PxWorkerDescriptor **) palloc0(totalPxNodes * sizeof(PxWorkerDescriptor *));

	PG_TRY();
	{
#ifdef FAULT_INJECTOR
		if (SIMPLE_FAULT_INJECTOR("px_build_gang_definition_error") == FaultInjectorTypeEnable)
			elog(ERROR, "px buildGangDefinition inject error");
#endif
		/* initialize db_descriptors */
		foreach_with_count(lc, segments, i)
		{
			workerId = lfirst_int(lc);
			newGangDefinition->db_descriptors[i] =
				pxnode_allocateIdlePX(workerId, totalPxNodes, segmentType);
		}
	}
	PG_CATCH();
	{
		RecycleGang(newGangDefinition, true /* destroy */ );
		PG_RE_THROW();
	}
	PG_END_TRY();

	ELOG_DISPATCHER_DEBUG("buildGangDefinition done");
	return newGangDefinition;
}

/*
 * Add one GUC to the option string.
 */
static void
addOneOption(StringInfo string, struct config_generic *guc)
{
	Assert(guc && (guc->flags & GUC_PX_NEED_SYNC));
	switch (guc->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *bguc = (struct config_bool *) guc;

				appendStringInfo(string, " -c %s=%s", guc->name, *(bguc->variable) ? "true" : "false");
				break;
			}
		case PGC_INT:
			{
				struct config_int *iguc = (struct config_int *) guc;

				appendStringInfo(string, " -c %s=%d", guc->name, *iguc->variable);
				break;
			}
		case PGC_REAL:
			{
				struct config_real *rguc = (struct config_real *) guc;

				appendStringInfo(string, " -c %s=%f", guc->name, *rguc->variable);
				break;
			}
		case PGC_STRING:
			{
				struct config_string *sguc = (struct config_string *) guc;
				const char *str = *sguc->variable;
				int			i;

				if (str == NULL)
					break;

				appendStringInfo(string, " -c %s=", guc->name);

				/*
				 * All whitespace characters must be escaped. See
				 * pg_split_opts() in the backend.
				 */
				for (i = 0; str[i] != '\0'; i++)
				{
					if (isspace((unsigned char) str[i]))
						appendStringInfoChar(string, '\\');

					appendStringInfoChar(string, str[i]);
				}
				break;
			}
		case PGC_ENUM:
			{
				struct config_enum *eguc = (struct config_enum *) guc;
				int			value = *eguc->variable;
				const char *str = config_enum_lookup_by_value(eguc, value);
				int			i;

				if (str == NULL)
					break;

				appendStringInfo(string, " -c %s=", guc->name);

				/*
				 * All whitespace characters must be escaped. See
				 * pg_split_opts() in the backend. (Not sure if an enum value
				 * can have whitespace, but let's be prepared.)
				 */
				for (i = 0; str[i] != '\0'; i++)
				{
					if (isspace((unsigned char) str[i]))
						appendStringInfoChar(string, '\\');

					appendStringInfoChar(string, str[i]);
				}
				break;
			}
		default:
			Insist(false);
	}
}

/*
 * Add GUCs to option string.
 */
char *
makeOptions(void)
{
	struct config_generic **gucs = get_guc_variables();
	int			ngucs = px_get_num_guc_variables();
	PxNodeInfo *qdinfo = NULL;
	StringInfoData string;
	int			i;

	initStringInfo(&string);

	Assert(px_role == PX_ROLE_QC);

	qdinfo = pxnode_getPxNodeInfo(MASTER_CONTENT_ID, SEGMENTTYPE_ANY);
	appendStringInfo(&string, " -c polar_px_qc_hostname=%s", qdinfo->config->hostip);
	appendStringInfo(&string, " -c polar_px_qc_port=%d", qdinfo->config->port);

	for (i = 0; i < ngucs; ++i)
	{
		struct config_generic *guc = gucs[i];

		if ((guc->flags & GUC_PX_NEED_SYNC) &&
			(guc->context == PGC_USERSET ||
			 guc->context == PGC_BACKEND ||
			 px_authenticated_user_is_superuser()))
			addOneOption(&string, guc);
	}

	return string.data;
}

/*
 * build_pxid_param
 *
 * Called from the qDisp process to create the "pxid" parameter string
 * to be passed to a qExec that is being started.  NB: Can be called in a
 * thread, so mustn't use palloc/elog/ereport/etc.
 */
bool
build_pxid_param(char *buf, int bufsz, int identifier, int icHtabSize)
{
#ifdef HAVE_INT64_TIMESTAMP
#define TIMESTAMP_FORMAT INT64_FORMAT
#else
#ifndef _WIN32
#define TIMESTAMP_FORMAT "%.14a"
#else
#define TIMESTAMP_FORMAT "%g"
#endif
#endif
	int			len;
	uint64		systemid;
	uint32		random_data;

	/* add a random data for confusing */
	srandom((unsigned int) (time(NULL)));
	random_data = random();

	systemid = GetSystemIdentifier();

	len = snprintf(buf, bufsz,
				   UINT64_FORMAT ";" UINT64_FORMAT ";"
				   UINT64_FORMAT ";%d;" TIMESTAMP_FORMAT ";"
				   "%d;" UINT64_FORMAT ";"
				   "%d;%d;%u;%u",
				   px_compat_version, px_serialize_member_count,
				   systemid, px_session_id, PgStartTime,
				   px_enable_replay_wait, px_sql_wal_lsn,
				   identifier, icHtabSize, (uint32) px_adaptive_paging, random_data);

	return (len > 0 && len < bufsz);
}

static bool
pxid_next_param(char **cpp, char **npp)
{
	*cpp = *npp;
	if (!*cpp)
		return false;

	*npp = strchr(*npp, ';');
	if (*npp)
	{
		**npp = '\0';
		++*npp;
	}
	return true;
}

/*
 * pxgang_parse_pxid_params
 *
 * Called very early in backend initialization, to interpret the "pxid"
 * parameter value that a qExec receives from its qDisp.
 *
 * At this point, client authentication has not been done; the backend
 * command line options have not been processed; GUCs have the settings
 * inherited from the postmaster; etc; so don't try to do too much in here.
 */
void
pxgang_parse_pxid_params(struct Port *port pg_attribute_unused(),
						 const char *pxid_value)
{
	char	   *pxid = pstrdup(pxid_value);
	char	   *cp;
	char	   *np = pxid;
	uint64		recv_compat_version = 0;
	uint64		recv_serialize_member_count = 0;

	/* always set cache px var to true on px */
	px_is_executing = true;
	px_is_planning = true;
	px_role = PX_ROLE_PX;

	/*
	 * before global csn supported, we use csn2xid_snapshot to support local
	 * csn
	 */
	polar_csn_xid_snapshot = polar_csn_enable;

	/* px_compat_version */
	if (pxid_next_param(&cp, &np))
		recv_compat_version = strtou64(cp, NULL, 10);

	if (recv_compat_version != px_compat_version)
	{
		elog(FATAL, "px compat version not match, local-remote:%lu-%lu", px_compat_version, recv_compat_version);
		goto bad;
	}

	/* px_serialize_member_count */
	if (pxid_next_param(&cp, &np))
		recv_serialize_member_count = strtou64(cp, NULL, 10);

	if (pxid_next_param(&cp, &np))
		px_auth_systemid = strtou64(cp, NULL, 10);

	/* px_session_id */
	if (pxid_next_param(&cp, &np))
		px_session_id = strtou64(cp, NULL, 10);

	/* PgStartTime */
	if (pxid_next_param(&cp, &np))
	{
#ifdef HAVE_INT64_TIMESTAMP
		char	   *endptr;

		errno = 0;
		PgStartTime = strtoi64(cp, &endptr, 10);
		if (errno != 0 || *endptr != '\0')
			goto bad;
#else
		PgStartTime = strtod(cp, NULL);
#endif
	}

	if (pxid_next_param(&cp, &np))
		cached_px_enable_replay_wait = (int) strtol(cp, NULL, 10);

	if (pxid_next_param(&cp, &np))
		px_sql_wal_lsn = strtou64(cp, NULL, 10);

	if (pxid_next_param(&cp, &np))
		px_identifier = (int) strtol(cp, NULL, 10);

	if (pxid_next_param(&cp, &np))
		ic_htab_size = (int) strtol(cp, NULL, 10);

	if (pxid_next_param(&cp, &np))
		px_adaptive_paging = (bool) ((int) strtol(cp, NULL, 10));

	if (pxid_next_param(&cp, &np))
	{
		/* do noting, just param the random data */
	}

	/* Too few items */
	if (!cp)
		goto bad;

	if (np)
	{
		if (recv_serialize_member_count > px_serialize_member_count)
		{
			int			i;

			for (i = 0;
				 i < recv_serialize_member_count - px_serialize_member_count;
				 ++i)
			{
				pxid_next_param(&cp, &np);
				elog(INFO, "extra pxid param %s", cp);
			}
		}
		else
		{
			/* error version or member count */
			goto bad;
		}
	}

	if (px_session_id <= 0 || PgStartTime <= 0 || px_identifier < 0
		|| ic_htab_size <= 0 || px_sql_wal_lsn <= 0)
		goto bad;

	pfree(pxid);
	return;

bad:
	elog(FATAL, "Segment dispatched with invalid option: 'pxid=%s'", pxid_value);
}

static PxProcess *
makePxProcess(PxWorkerDescriptor *pxWorkerDesc)
{
	PxProcess  *process = (PxProcess *) makeNode(PxProcess);
	PxNodeInfo *pxinfo = pxWorkerDesc->pxNodeInfo;

	if (pxinfo == NULL)
	{
		elog(ERROR, "required segment is unavailable");
	}
	else if (pxinfo->config->hostip == NULL)
	{
		elog(ERROR, "required segment IP is unavailable");
	}

	process->listenerAddr = pstrdup(pxinfo->config->hostip);
	process->remotePort = pxinfo->config->port;

	if (px_interconnect_type == INTERCONNECT_TYPE_UDPIFC)
		process->listenerPort = (pxWorkerDesc->motionListener >> 16) & 0x0ffff;
	else if (px_interconnect_type == INTERCONNECT_TYPE_TCP)
		process->listenerPort = (pxWorkerDesc->motionListener & 0x0ffff);

	process->pid = pxWorkerDesc->backendPid;
	process->contentid = pxWorkerDesc->logicalWorkerInfo.idx;
	process->contentCount = pxWorkerDesc->logicalWorkerInfo.total_count;
	process->identifier = pxWorkerDesc->identifier;
	return process;
}

/*
 * Create a list of PxProcess and initialize with Gang information.
 *
 * 1) For primary reader gang and primary writer gang, the elements
 * in this list is order by segment index.
 * 2) For entry DB gang and singleton gang, the list length is 1.
 *
 * @directDispatch: might be null
 */
void
setupPxProcessList(ExecSlice *slice)
{
	int			i = 0;
	Gang	   *gang = slice->primaryGang;

	ELOG_DISPATCHER_DEBUG("getPxProcessList slice%d gangtype=%d gangsize=%d",
						  slice->sliceIndex, gang->type, gang->size);
	Assert(gang);
	Assert(px_role == PX_ROLE_QC);
	Assert(gang->type == GANGTYPE_PRIMARY_WRITER ||
		   gang->type == GANGTYPE_PRIMARY_READER ||
		   (gang->type == GANGTYPE_ENTRYDB_READER && gang->size == 1) ||
		   (gang->type == GANGTYPE_SINGLETON_READER && gang->size == 1));


	for (i = 0; i < gang->size; i++)
	{
		PxWorkerDescriptor *pxWorkerDesc = gang->db_descriptors[i];
		PxProcess  *process = makePxProcess(pxWorkerDesc);

		pxconn_setPXIdentifier(pxWorkerDesc, slice->sliceIndex);

		slice->primaryProcesses = lappend(slice->primaryProcesses, process);
		slice->processesMap = bms_add_member(slice->processesMap, pxWorkerDesc->identifier);

		ELOG_DISPATCHER_DEBUG("Gang assignment: slice%d seg%d count%d identifier%d %s:%d pid=%d",
							  slice->sliceIndex, process->contentid,
							  process->contentCount, process->identifier,
							  process->listenerAddr, process->listenerPort,
							  process->pid);
	}
}

/*
 * getPxProcessForQC:	Manufacture a PxProcess representing the QC,
 * as if it were a worker from the executor factory.
 *
 * NOTE: Does not support multiple (mirrored) QCs.
 */
List *
getPxProcessesForQC(int isPrimary)
{
	List	   *list = NIL;

	PxNodeInfo *qdinfo pg_attribute_unused();
	PxProcess  *proc;

	Assert(px_role == PX_ROLE_QC);

	if (!isPrimary)
	{
		elog(FATAL, "getPxProcessesForQC: unsupported request for master mirror process");
	}

	qdinfo = pxnode_getPxNodeInfo(MASTER_CONTENT_ID, SEGMENTTYPE_ANY);

	Assert(qdinfo->config->node_idx == -1);
	Assert(qdinfo->config->hostip != NULL);

	proc = makeNode(PxProcess);

	/*
	 * Set QC listener address to NULL. This will be filled during starting up
	 * outgoing interconnect connection.
	 */
	proc->listenerAddr = NULL;

	if (px_interconnect_type == INTERCONNECT_TYPE_UDPIFC)
		proc->listenerPort = (px_listener_port >> 16) & 0x0ffff;
	else if (px_interconnect_type == INTERCONNECT_TYPE_TCP)
		proc->listenerPort = (px_listener_port & 0x0ffff);

	proc->pid = MyProcPid;
	proc->contentid = -1;

	list = lappend(list, proc);
	return list;
}

/*
 * Helper functions
 */

const char *
gangTypeToString(GangType type)
{
	const char *ret = "";

	switch (type)
	{
		case GANGTYPE_PRIMARY_WRITER:
			ret = "primary writer";
			break;
		case GANGTYPE_PRIMARY_READER:
			ret = "primary reader";
			break;
		case GANGTYPE_SINGLETON_READER:
			ret = "singleton reader";
			break;
		case GANGTYPE_ENTRYDB_READER:
			ret = "entry DB reader";
			break;
		case GANGTYPE_UNALLOCATED:
			ret = "unallocated";
			break;
		default:
			Assert(false);
	}
	return ret;
}

void
RecycleGang(Gang *gp, bool forceDestroy)
{
	int			i;

	if (!gp)
		return;

	/*
	 * Loop through the px_worker_descriptors array and, for each
	 * PxWorkerDescriptor: 1) discard the query results (if any), 2)
	 * disconnect the session, and 3) discard any connection error message.
	 */
	for (i = 0; i < gp->size; i++)
	{
		PxWorkerDescriptor *pxWorkerDesc = gp->db_descriptors[i];

		Assert(pxWorkerDesc != NULL);

		pxnode_recycleIdlePX(pxWorkerDesc, forceDestroy);
	}
}

PXScanDesc
CreatePXScanDesc(void)
{
	PXScanDesc	px_scan;

	px_scan = palloc0(sizeof(PXScanDescData));
	px_scan->pxs_total_workers = px_logical_total_workers;
	px_scan->pxs_worker_id = px_logical_worker_idx;
	px_scan->pxs_scan_round = -1;
	px_scan->pxs_adaptive_scan = px_adaptive_paging;
	px_scan->pxs_prefetch_inner_scan = px_prefetch_inner_executing;
	elog(DEBUG5, "create px scandesc, worker_id: %d, total_workers: %d",
		 px_scan->pxs_worker_id, px_scan->pxs_total_workers);
	return px_scan;
}

void
get_worker_info_by_identifier(ExecSlice *slice, int *idx, int *total_count)
{
	Assert(slice);
	Assert(slice->primaryProcesses);
	Assert(slice->primaryProcesses->length > 0);

	if (slice &&
		slice->primaryProcesses &&
		slice->primaryProcesses->length > 0 &&
		idx &&
		total_count)
	{
		ListCell   *cell;
		PxProcess  *px_process = NULL;
		bool		found = false;

		/* Just reading, so don't check INS/DEL/UPD permissions. */
		foreach(cell, slice->primaryProcesses)
		{
			px_process = (PxProcess *) lfirst(cell);
			if (px_process && px_process->identifier == px_identifier)
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			elog(ERROR, "could not find PX identifier in db_descriptors");
			return;
		}

		*idx = px_process->contentid;
		*total_count = px_process->contentCount;

		Assert(*idx >= MASTER_CONTENT_ID && *total_count >= 1);
	}
}
