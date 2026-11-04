/*-------------------------------------------------------------------------
 *
 * px_disp_query.c
 *	  Functions to dispatch command string or plan to PX executors.
 *
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/dispatcher/px_disp_query.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/execUtils_px.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "tcop/tcopprot.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

#include "px/px_conn.h"
#include "px/px_disp.h"
#include "px/px_disp_query.h"
#include "px/px_dispatchresult.h"
#include "px/px_gang.h"
#include "px/px_mutate.h"
#include "px/px_srlz.h"
#include "px/px_util.h"
#include "px/px_vars.h"
#include "px/tupleremap.h"
#include "px/px_snapshot.h"
#include "utils/faultinjector.h"

#define QUERY_STRING_TRUNCATE_SIZE (1024)

extern bool px_test_print_direct_dispatch_info;

/*
 * We need an array describing the relationship between a slice and
 * the number of "child" slices which depend on it.
 */
typedef struct
{
	int			sliceIndex;
	int			children;
	ExecSlice  *slice;
} SliceVec;

/*
 * Parameter structure for Greenplum Database Queries
 */
typedef struct DispatchCommandQueryParms
{
	/*
	 * The SQL command
	 */
	const char *strCommand;
	int			strCommandlen;
	char	   *serializedQuerytree;
	int			serializedQuerytreelen;
	char	   *serializedPlantree;
	int			serializedPlantreelen;
	char	   *serializedQueryDispatchDesc;
	int			serializedQueryDispatchDesclen;
	char	   *serializedParams;
	int			serializedParamslen;

	/*
	 * Additional information.
	 */
	char	   *serializedOidAssignments;
	int			serializedOidAssignmentslen;

	/*
	 * serialized Snapshot string
	 */
	char	   *serializedSnapshot;
	int			serializedSnapshotlen;
} DispatchCommandQueryParms;

static int	fillSliceVector(SliceTable *sliceTable,
							int sliceIndex,
							SliceVec *sliceVector,
							int len);

static char *buildPXQueryString(DispatchCommandQueryParms *pQueryParms, int *finalLen);

static DispatchCommandQueryParms *pxdisp_buildPlanQueryParms(struct QueryDesc *queryDesc, bool planRequiresTxn);

static void pxdisp_dispatchX(QueryDesc *queryDesc,
							 bool planRequiresTxn,
							 bool cancelOnError);

static char *serializeParamListInfo(ParamListInfo paramLI, int *len_p);

/*
 * Compose and dispatch the POLARPX commands corresponding to a plan tree
 * within a complete parallel plan. (A plan tree will correspond either
 * to an initPlan or to the main plan.)
 *
 * If cancelOnError is true, then any dispatching error, a cancellation
 * request from the client, or an error from any of the associated PXs,
 * may cause the unfinished portion of the plan to be abandoned or canceled;
 * and in the event this occurs before all gangs have been dispatched, this
 * function does not return, but waits for all PXs to stop and exits to
 * the caller's error catcher via ereport(ERROR,...). Otherwise this
 * function returns normally and errors are not reported until later.
 *
 * If cancelOnError is false, the plan is to be dispatched as fully as
 * possible and the PXs allowed to proceed regardless of cancellation
 * requests, errors or connection failures from other PXs, etc.
 *
 * The PxDispatchResults objects allocated for the plan are returned
 * in *pPrimaryResults. The caller, after calling
 * PxCheckDispatchResult(), can examine the PxDispatchResults
 * objects, can keep them as long as needed, and ultimately must free
 * them with pxdisp_destroyDispatcherState() prior to deallocation of
 * the caller's memory context. Callers should use PG_TRY/PG_CATCH to
 * ensure proper cleanup.
 *
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use pxdisp_finishCommand().
 *
 * Note that the slice tree dispatched is the one specified in the EState
 * of the argument QueryDesc as es_cur__slice.
 *
 * Note that the QueryDesc params must include PARAM_EXEC_REMOTE parameters
 * containing the values of any initplans required by the slice to be run.
 * (This is handled by calls to addRemoteExecParamsToParamList() from the
 * functions preprocess_initplans() and ExecutorRun().)
 *
 * Each PX receives its assignment as a message of type 'M' in PostgresMain().
 * The message is deserialized and processed by exec_px_query() in postgres.c.
 */
void
PxDispatchPlan(struct QueryDesc *queryDesc,
			   bool planRequiresTxn,
			   bool cancelOnError)
{
	PlannedStmt *stmt;
	bool		is_SRI = false;
	List	   *cursors;

	Assert(px_role == PX_ROLE_QC);
	Assert(queryDesc != NULL && queryDesc->estate != NULL);

	/*
	 * This function is called only for planned statements.
	 */
	stmt = queryDesc->plannedstmt;
	Assert(stmt);

	/*
	 * Need to be careful not to modify the original PlannedStmt, because it
	 * might be a cached plan. So make a copy. A shallow copy of the fields we
	 * don't modify should be enough.
	 */
	stmt = palloc(sizeof(PlannedStmt));
	memcpy(stmt, queryDesc->plannedstmt, sizeof(PlannedStmt));
	stmt->subplans = list_copy(stmt->subplans);

	stmt->planTree = (Plan *) exec_make_plan_constant(stmt, queryDesc->estate, is_SRI, &cursors);
	queryDesc->plannedstmt = stmt;

	queryDesc->ddesc->cursorPositions = (List *) copyObject(cursors);

	pxdisp_dispatchX(queryDesc, planRequiresTxn, cancelOnError);
}

static DispatchCommandQueryParms *
pxdisp_buildPlanQueryParms(struct QueryDesc *queryDesc,
						   bool planRequiresTxn)
{
	char	   *splan,
			   *sddesc,
			   *sparams;

	int			splan_len,
				splan_len_uncompressed,
				sddesc_len,
				sparams_len;

	uint64		plan_size_in_kb;

	DispatchCommandQueryParms *pQueryParms = NULL;


	pQueryParms = (DispatchCommandQueryParms *) palloc0(sizeof(*pQueryParms));

	/*
	 * serialized plan tree. Note that we're called for a single slice tree
	 * (corresponding to an initPlan or the main plan), so the parameters are
	 * fixed and we can include them in the prefix.
	 */
	splan = serializeNode((Node *) queryDesc->plannedstmt, &splan_len, &splan_len_uncompressed);

	plan_size_in_kb = ((uint64) splan_len_uncompressed) / (uint64) 1024;

	elog(((px_log_gang >= PXVARS_VERBOSITY_TERSE) ? LOG : DEBUG1),
		 "Query plan size to dispatch: " UINT64_FORMAT "KB", plan_size_in_kb);

	if (0 < px_max_plan_size && plan_size_in_kb > px_max_plan_size)
	{
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 (errmsg("Query plan size limit exceeded, current size: "
						 UINT64_FORMAT "KB, max allowed size: %dKB",
						 plan_size_in_kb, px_max_plan_size),
				  errhint("Size controlled by px_max_plan_size"))));
	}

	Assert(splan != NULL && splan_len > 0 && splan_len_uncompressed > 0);

	if (queryDesc->params != NULL && queryDesc->params->numParams > 0)
	{
		sparams = serializeParamListInfo(queryDesc->params, &sparams_len);
	}
	else
	{
		sparams = NULL;
		sparams_len = 0;
	}

	sddesc = serializeNode((Node *) queryDesc->ddesc, &sddesc_len, NULL /* uncompressed_size */ );

	pQueryParms->strCommand = queryDesc->sourceText;
	pQueryParms->serializedQuerytree = NULL;
	pQueryParms->serializedQuerytreelen = 0;
	pQueryParms->serializedPlantree = splan;
	pQueryParms->serializedPlantreelen = splan_len;
	pQueryParms->serializedParams = sparams;
	pQueryParms->serializedParamslen = sparams_len;
	pQueryParms->serializedQueryDispatchDesc = sddesc;
	pQueryParms->serializedQueryDispatchDesclen = sddesc_len;
	pQueryParms->serializedSnapshot = pxsn_get_serialized_snapshot();
	pQueryParms->serializedSnapshotlen = pxsn_get_serialized_snapshot_size();

	return pQueryParms;
}

/*
 * Three Helper functions for pxdisp_dispatchX:
 *
 * Used to figure out the dispatch order for the sliceTable by
 * counting the number of dependent child slices for each slice; and
 * then sorting based on the count (all indepenedent slices get
 * dispatched first, then the slice above them and so on).
 *
 * fillSliceVector: figure out the number of slices we're dispatching,
 * and order them.
 *
 * count_dependent_children(): walk tree counting up children.
 *
 * compare_slice_order(): comparison function for qsort(): order the
 * slices by the number of dependent children. Empty slices are
 * sorted last (to make this work with initPlans).
 *
 */
static int
compare_slice_order(const void *aa, const void *bb)
{
	SliceVec   *a = (SliceVec *) aa;
	SliceVec   *b = (SliceVec *) bb;

	if (a->slice == NULL)
		return 1;
	if (b->slice == NULL)
		return -1;

	/*
	 * Put the slice not going to dispatch in the last
	 */
	if (a->slice->primaryGang == NULL)
	{
		Assert(a->slice->gangType == GANGTYPE_UNALLOCATED);
		return 1;
	}
	if (b->slice->primaryGang == NULL)
	{
		Assert(b->slice->gangType == GANGTYPE_UNALLOCATED);
		return -1;
	}

	/*
	 * sort slice with larger size first because it has a bigger chance to
	 * contain writers
	 */
	if (a->slice->primaryGang->size > b->slice->primaryGang->size)
		return -1;

	if (a->slice->primaryGang->size < b->slice->primaryGang->size)
		return 1;

	if (a->children == b->children)
		return 0;
	else if (a->children > b->children)
		return 1;
	else
		return -1;
}

/*
 * Quick and dirty bit mask operations
 */
static void
mark_bit(char *bits, int nth)
{
	int			nthbyte = nth >> 3;
	char		nthbit = 1 << (nth & 7);

	bits[nthbyte] |= nthbit;
}

static void
or_bits(char *dest, char *src, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		dest[i] |= src[i];
}

static int
count_bits(char *bits, int nbyte)
{
	int			i;
	int			nbit = 0;

	int			bitcount[] =
	{
		0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
	};

	for (i = 0; i < nbyte; i++)
	{
		nbit += bitcount[bits[i] & 0x0F];
		nbit += bitcount[(bits[i] >> 4) & 0x0F];
	}

	return nbit;
}

/*
 * We use a bitmask to count the dep. childrens.
 * Because of input sharing, the slices now are DAG. We cannot simply go down the
 * tree and add up number of children, which will return too big number.
 */
static int
markbit_dep_children(SliceTable *sliceTable, int sliceIdx,
					 SliceVec *sliceVec, int bitmasklen, char *bits)
{
	ListCell   *sublist;
	ExecSlice  *slice = &sliceTable->slices[sliceIdx];

	foreach(sublist, slice->children)
	{
		int			childIndex = lfirst_int(sublist);
		char	   *newbits = palloc0(bitmasklen);

		markbit_dep_children(sliceTable, childIndex,
							 sliceVec, bitmasklen, newbits);
		or_bits(bits, newbits, bitmasklen);
		mark_bit(bits, childIndex);
		pfree(newbits);
	}

	sliceVec[sliceIdx].sliceIndex = sliceIdx;
	sliceVec[sliceIdx].children = count_bits(bits, bitmasklen);
	sliceVec[sliceIdx].slice = slice;

	return sliceVec[sliceIdx].children;
}

/*
 * Count how many dependent childrens and fill in the sliceVector of dependent childrens.
 */
static int
count_dependent_children(SliceTable *sliceTable, int sliceIndex,
						 SliceVec *sliceVector, int len)
{
	int			ret = 0;
	int			bitmasklen = (len + 7) >> 3;
	char	   *bitmask = palloc0(bitmasklen);

	ret = markbit_dep_children(sliceTable, sliceIndex, sliceVector, bitmasklen, bitmask);
	pfree(bitmask);

	return ret;
}

static int
fillSliceVector(SliceTable *sliceTbl, int rootIdx,
				SliceVec *sliceVector, int nTotalSlices)
{
	int			top_count;

	/*
	 * count doesn't include top slice add 1, note that sliceVector would be
	 * modified in place by count_dependent_children.
	 */
	top_count = 1 + count_dependent_children(sliceTbl, rootIdx, sliceVector, nTotalSlices);

	qsort(sliceVector, nTotalSlices, sizeof(SliceVec), compare_slice_order);

	return top_count;
}

/*
 * Build a query string to be dispatched to PX.
 */
static char *
buildPXQueryString(DispatchCommandQueryParms *pQueryParms, int *finalLen)
{

#define INT32_ENCODE(value)\
	u32 = htonl(value);\
	memcpy(pos, &u32, sizeof(value));\
	pos += sizeof(value);

#define UINT64_ENCODE(value)\
	u32 = (uint32) (value >> 32);\
	u32 = htonl(u32);\
	memcpy(pos, &u32, sizeof(u32));\
	pos += sizeof(u32);\
	\
	u32 = (uint32) value;\
	u32 = htonl(u32);\
	memcpy(pos, &u32, sizeof(u32));\
	pos += sizeof(u32);

#define STRN_ENCODE(len, str)\
	u32 = htonl(len);\
	memcpy(pos, &u32, sizeof(len));\
	pos += sizeof(len);\
	if (len > 0)\
	{\
		memcpy(pos, str, len);\
		pos[len - 1] = '\0';\
		pos += len;\
	}

#define STR_ENCODE(len, str)\
	u32 = htonl(len);\
	memcpy(pos, &u32, sizeof(len));\
	pos += sizeof(len);\
	if (len > 0)\
	{\
		memcpy(pos, str, len);\
		pos += len;\
	}

	const char *command = pQueryParms->strCommand;
	int			command_len;
	const char *querytree = pQueryParms->serializedQuerytree;
	int			querytree_len = pQueryParms->serializedQuerytreelen;
	const char *plantree = pQueryParms->serializedPlantree;
	int			plantree_len = pQueryParms->serializedPlantreelen;
	const char *params = pQueryParms->serializedParams;
	int			params_len = pQueryParms->serializedParamslen;
	const char *sddesc = pQueryParms->serializedQueryDispatchDesc;
	int			sddesc_len = pQueryParms->serializedQueryDispatchDesclen;
	const char *sdsnapshot = pQueryParms->serializedSnapshot;
	int			sdsnapshot_len = pQueryParms->serializedSnapshotlen;
	int64		currentStatementStartTimestamp = GetCurrentStatementStartTimestamp();
	Oid			sessionUserId = GetSessionUserId();
	Oid			outerUserId = GetOuterUserId();
	Oid			currentUserId = GetUserId();

	int			len;
	uint32		u32;
	int			total_query_len;
	char	   *shared_query;
	char	   *pos;
	MemoryContext oldContext;

	/*
	 * Must allocate query text within px_DispatcherContext,
	 */
	Assert(px_DispatcherContext);
	oldContext = MemoryContextSwitchTo(px_DispatcherContext);

	/*
	 * If either querytree or plantree is set then the query string is not so
	 * important, dispatch a truncated version to increase the performance.
	 *
	 * Here we only need to determine the truncated size, the actual work is
	 * done later when copying it to the result buffer.
	 */
	if (querytree || plantree)
		command_len = strnlen(command, QUERY_STRING_TRUNCATE_SIZE - 1) + 1;
	else
		command_len = strlen(command) + 1;


	total_query_len = 1 /* 'M' */ +
		sizeof(len) /* message length */ +
		sizeof(px_serialize_version) +
		sizeof(sessionUserId) /* sessionUserIsSuper */ +
		sizeof(outerUserId) /* outerUserIsSuper */ +
		sizeof(currentUserId) +
		sizeof(currentStatementStartTimestamp) /* currentStatementStartTimestamp */ +
		sizeof(sql_trace_id.uval) /* sqlTraceId */ +
		sizeof(command_len) + command_len +
		sizeof(querytree_len) + querytree_len +
		sizeof(plantree_len) + plantree_len +
		sizeof(params_len) + params_len +
		sizeof(sddesc_len) + sddesc_len +
		sizeof(sdsnapshot_len) + sdsnapshot_len;

	shared_query = palloc0(total_query_len);
	pos = shared_query;
	*pos++ = 'M';
	len = total_query_len - 1;

	INT32_ENCODE(len);
	INT32_ENCODE(px_serialize_version);
	INT32_ENCODE(sessionUserId);
	INT32_ENCODE(outerUserId);
	INT32_ENCODE(currentUserId);

	UINT64_ENCODE(currentStatementStartTimestamp);
	UINT64_ENCODE(sql_trace_id.uval);

	STRN_ENCODE(command_len, command);
	STR_ENCODE(querytree_len, querytree);
	STR_ENCODE(plantree_len, plantree);
	STR_ENCODE(params_len, params);
	STR_ENCODE(sddesc_len, sddesc);
	STR_ENCODE(sdsnapshot_len, sdsnapshot);

	Assert(len + 1 == total_query_len);

	if (finalLen)
		*finalLen = total_query_len;

	MemoryContextSwitchTo(oldContext);

	return shared_query;
}

void
px_log_querydesc(QueryDispatchDesc *ddesc)
{
	SliceTable *sliceT = ddesc->sliceTable;
	ExecSlice  *es = sliceT->slices;
	int			i;

	ereport((px_enable_print ? LOG : DEBUG1),
			(errmsg("begin exec px query: sessid %d, trace_id %ld, num slices %d, local slice %d, px_sql_wal_lsn %lX",
					px_session_id, sql_trace_id.uval, sliceT->numSlices,
					sliceT->localSlice, px_sql_wal_lsn)));

	if (px_info_debug)
	{
		for (i = 0; i < sliceT->numSlices; i++)
		{
			List	   *ps = es[i].primaryProcesses;
			ListCell   *lc;

			ereport(LOG, (errmsg("sliceIndex: %d, rootIndex: %d, parentIndex: %d, numSegs: %d, gangType: %d",
								 es[i].sliceIndex, es[i].rootIndex, es[i].parentIndex, es[i].planNumSegments, es[i].gangType), errhidestmt(true)));
			foreach(lc, ps)
			{
				PxProcess  *p = (PxProcess *) lfirst(lc);

				ereport(LOG, (errmsg("    remote addr: %s, remote port: %d, local listen port: %d, pid: %d, contentid: %d, contentCount: %d, identifier: %d",
									 p->listenerAddr, p->remotePort, p->listenerPort, p->pid,
									 p->contentid, p->contentCount, p->identifier), errhidestmt(true)));
			}
		}
	}
}

/*
 * This function is used for dispatching sliced plans
 */
static void
pxdisp_dispatchX(QueryDesc *queryDesc,
				 bool planRequiresTxn,
				 bool cancelOnError)
{
	SliceVec   *sliceVector = NULL;
	int			nSlices = 1;	/* slices this dispatch cares about */
	int			nTotalSlices = 1;	/* total slices in sliceTbl */

	int			iSlice;
	int			rootIdx;
	char	   *queryText = NULL;
	int			queryTextLength = 0;
	struct SliceTable *sliceTbl;
	struct EState *estate;
	PxDispatcherState *ds;
	ErrorData  *pxError = NULL;
	DispatchCommandQueryParms *pQueryParms;

	if (px_log_dispatch_stats)
		ResetUsage();

	estate = queryDesc->estate;
	sliceTbl = estate->es_sliceTable;
	Assert(sliceTbl != NULL);

	rootIdx = RootSliceIndex(estate);

	ds = pxdisp_makeDispatcherState(queryDesc->extended_query);

	/*
	 * Since we intend to execute the plan, inventory the slice tree, allocate
	 * gangs, and associate them with slices.
	 *
	 * On return, gangs have been allocated and PXProcess lists have been
	 * filled in in the slice table.)
	 *
	 * Notice: This must be done before pxdisp_buildPlanQueryParms
	 */
	AssignGangs(ds, queryDesc);

	/*
	 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
	 * vector of slice indexes specifying the order of [potential] dispatch.
	 */
	nTotalSlices = sliceTbl->numSlices;
	sliceVector = palloc0(nTotalSlices * sizeof(SliceVec));
	nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, nTotalSlices);
	/* Each slice table has a unique-id. */
	sliceTbl->ic_instance_id = ++px_interconnect_id;

	pQueryParms = pxdisp_buildPlanQueryParms(queryDesc, planRequiresTxn);
	queryText = buildPXQueryString(pQueryParms, &queryTextLength);

	px_log_querydesc(queryDesc->ddesc);

	/*
	 * Allocate result array with enough slots for PXs of primary gangs.
	 */
	pxdisp_makeDispatchResults(ds, nTotalSlices, cancelOnError);
	pxdisp_makeDispatchParams(ds, nTotalSlices, queryText, queryTextLength);

	px_total_plans++;
	px_total_slices += nSlices;
	if (nSlices > px_max_slices)
		px_max_slices = nSlices;

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,
						(errmsg("duration to start of dispatch send (root %d): %s ms",
								rootIdx, msec_str)));
				break;
		}
	}

	for (iSlice = 0; iSlice < nSlices; iSlice++)
	{
		Gang	   *primaryGang = NULL;
		ExecSlice  *slice;
		int			si = -1;

		Assert(sliceVector != NULL);

		slice = sliceVector[iSlice].slice;
		si = slice->sliceIndex;

		/*
		 * Is this a slice we should dispatch?
		 */
		if (slice && slice->gangType == GANGTYPE_UNALLOCATED)
		{
			Assert(slice->primaryGang == NULL);

			/*
			 * Most slices are dispatched, however, in many cases the root
			 * runs only on the QC and is not dispatched to the PXs.
			 */
			continue;
		}

		primaryGang = slice->primaryGang;
		Assert(primaryGang != NULL);
		AssertImply(queryDesc->extended_query,
					primaryGang->type == GANGTYPE_PRIMARY_READER ||
					primaryGang->type == GANGTYPE_SINGLETON_READER ||
					primaryGang->type == GANGTYPE_ENTRYDB_READER);

		if (px_test_print_direct_dispatch_info)
			elog(INFO, "(slice %d) Dispatch command to %s", slice->sliceIndex,
				 segmentsToContentStr(slice->segments));

		/*
		 * Bail out if already got an error or cancellation request.
		 */
		if (cancelOnError)
		{
			if (ds->primaryResults->errcode)
				break;
			if (IS_PX_NEED_CANCELED())
				break;
		}
		pxdisp_dispatchToGang(ds, primaryGang, si);
	}

	pfree(sliceVector);

	/* Start a background libpq thread */
	pxdisp_startPqThread(ds);
	/* If libpq is not run in background */
	if (!pxdisp_isDsThreadRuning())
		pxdisp_waitDispatchFinish(ds);

	/*
	 * If bailed before completely dispatched, stop PXs and throw error.
	 */
	if (iSlice < nSlices
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("pxdisp_dispatch_slices") == FaultInjectorTypeEnable
#endif
		)
	{
		elog(px_debug_cancel_print ? LOG : DEBUG2,
			 "Plan dispatch canceled; dispatched %d of %d slices",
			 iSlice, nSlices);

		/*
		 * Cancel any PXs still running, and wait for them to terminate.
		 */
		pxdisp_cancelDispatch(ds);

		/*
		 * Check and free the results of all gangs. If any PX had an error,
		 * report it and exit via PG_THROW.
		 */
		pxdisp_getDispatchResults(ds, &pxError);

		/* stop libpq thread if run */
		pxdisp_finishPqThread();

		if (pxError)
		{
			FlushErrorState();
			ReThrowError(pxError);
		}

		/*
		 * Wasn't an error, must have been an interrupt.
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Strange! Not an interrupt either.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_PX_INTERNAL_ERROR),
				 errmsg_internal("unable to dispatch plan")));
	}

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,
						(errmsg("duration to dispatch out (root %d): %s ms",
								rootIdx, msec_str)));
				break;
		}
	}

	/* TODO: consider to move this assignment closer to ds initialization */
	estate->dispatcherState = ds;
}

/*
 * Serialization of query parameters (ParamListInfos).
 *
 * When a query is dispatched from QC to PX, we also need to dispatch any
 * query parameters, contained in the ParamListInfo struct. We need to
 * serialize ParamListInfo, but there are a few complications:
 *
 * - ParamListInfo is not a Node type, so we cannot use the usual
 * nodeToStringBinary() function directly. We turn the array of
 * ParamExternDatas into a List of SerializedParamExternData nodes,
 * which we can then pass to nodeToStringBinary().
 *
 * - The paramFetch callback, which could be used in this process to fetch
 * parameter values on-demand, cannot be used in a different process.
 * Therefore, fetch all parameters before serializing them. When
 * deserializing, leave the callbacks NULL.
 *
 * - In order to deserialize correctly, the receiver needs the typlen and
 * typbyval information for each datatype. The receiver has access to the
 * catalogs, so it could look them up, but for the sake of simplicity and
 * robustness in the receiver, we include that information in
 * SerializedParamExternData.
 *
 * - RECORD types. Type information of transient record is kept only in
 * backend private memory, indexed by typmod. The recipient will not know
 * what a record type's typmod means. And record types can also be nested.
 * Because of that, if there are any RECORD, we include a copy of the whole
 * transient record type cache.
 *
 * If there are no record types involved, we dispatch a list of
 * SerializedParamListInfos, i.e.
 *
 * List<SerializedParamListInfo>
 *
 * With record types, we dispatch:
 *
 * List(List<TupleDescNode>, List<SerializedParamListInfo>)
 *
 * XXX: Sending *all* record types can be quite bulky, but ATM there is no
 * easy way to extract just the needed record types.
 */
static char *
serializeParamListInfo(ParamListInfo paramLI, int *len_p)
{
	int			i;
	List	   *sparams;
	bool		found_records = false;

	/* Construct a list of SerializedParamExternData */
	sparams = NIL;
	for (i = 0; i < paramLI->numParams; i++)
	{
		ParamExternData *prm;
		SerializedParamExternData *sprm;
		ParamExternData prmdata;

		if (paramLI->paramFetch != NULL)
			prm = paramLI->paramFetch(paramLI, i + 1,
									  true, &prmdata);
		else
			prm = &paramLI->params[i];

		/*
		 * First, use paramFetch to fetch any "lazy" parameters. (The callback
		 * function is of no use in the PX.)
		 */
		if (paramLI->paramFetch && !OidIsValid(prm->ptype))
			(*paramLI->paramFetch) (paramLI, i + 1, false, &prmdata);

		sprm = makeNode(SerializedParamExternData);

		sprm->value = prm->value;
		sprm->isnull = prm->isnull;
		sprm->pflags = prm->pflags;
		sprm->ptype = prm->ptype;

		if (OidIsValid(prm->ptype))
		{
			get_typlenbyval(prm->ptype, &sprm->plen, &sprm->pbyval);

			if (prm->ptype == RECORDOID && !prm->isnull)
			{
				/*
				 * Note: We don't want to use lookup_rowtype_tupdesc_copy
				 * here, because it copies defaults and constraints too. We
				 * don't want those.
				 */
				found_records = true;
			}
		}
		else
		{
			sprm->plen = 0;
			sprm->pbyval = true;
		}

		sparams = lappend(sparams, sprm);
	}

	/*
	 * If there were any record types, include the transient record type
	 * cache.
	 */
	if (found_records)
		sparams = lcons(build_tuple_node_list(0), sparams);

	return nodeToBinaryStringFast(sparams, len_p);
}

ParamListInfo
deserializeParamListInfo(const char *str, int slen)
{
	List	   *sparams;
	ListCell   *lc;
	TupleRemapper *remapper;
	ParamListInfo paramLI;
	int			numParams;
	int			iparam;

	sparams = (List *) readNodeFromBinaryString(str, slen);
	if (!sparams)
		return NULL;
	if (!IsA(sparams, List))
		elog(ERROR, "could not deserialize query parameters");

	/*
	 * If a transient record type cache was included, load it into a
	 * TupleRemapper.
	 */
	if (IsA(linitial(sparams), List))
	{
		List	   *typelist = (List *) linitial(sparams);

		sparams = list_delete_first(sparams);

		remapper = CreateTupleRemapper();
		TRHandleTypeLists(remapper, typelist);
	}
	else
		remapper = NULL;

	/*
	 * Build a new ParamListInfo.
	 */
	numParams = list_length(sparams);

	paramLI = palloc(offsetof(ParamListInfoData, params) + numParams * sizeof(ParamExternData));
	/* this clears the callback fields, among others */
	memset(paramLI, 0, offsetof(ParamListInfoData, params));
	paramLI->numParams = numParams;

	/*
	 * Read the ParamExternDatas
	 */
	iparam = 0;
	foreach(lc, sparams)
	{
		SerializedParamExternData *sprm = (SerializedParamExternData *) lfirst(lc);
		ParamExternData *prm = &paramLI->params[iparam];

		if (!IsA(sprm, SerializedParamExternData))
			elog(ERROR, "could not deserialize query parameters");

		prm->ptype = sprm->ptype;
		prm->isnull = sprm->isnull;
		prm->pflags = sprm->pflags;

		/* If remapping record types is needed, do it. */
		if (remapper && prm->ptype != InvalidOid)
			prm->value = TRRemapDatum(remapper, sprm->ptype, sprm->value);
		else
			prm->value = sprm->value;

		iparam++;
	}

	return paramLI;
}
