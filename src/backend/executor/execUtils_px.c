/*-------------------------------------------------------------------------
 *
 * execUtils_px.c
 *	  Miscellaneous executor utility routines for PX.
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execUtils_px.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/execUtils_px.h"
#include "optimizer/px_walkers.h"
#include "utils/guc.h"
#include "px/px_llize.h"
#include "px/px_util.h"
#include "px/px_vars.h"
#include "px/px_disp_query.h"
#include "px/px_dispatchresult.h"
#include "px/ml_ipc.h"
#include "px/px_motion.h"
#include "px/px_sreh.h"
#include "utils/lsyscache.h"
#include "catalog/catalog.h"
#include "utils/builtins.h"
#include "executor/functions.h"
#include "miscadmin.h"

#define PSW_IGNORE_INITPLAN    0x01
static PxVisitOpt planstate_walk_node_extended(PlanState *planstate,
											   PxVisitOpt (*walker) (PlanState *planstate, void *context),
											   void *context,
											   int flags);

static PxVisitOpt planstate_walk_array(PlanState **planstates,
									   int nplanstate,
									   PxVisitOpt (*walker) (PlanState *planstate, void *context),
									   void *context,
									   int flags);

static PxVisitOpt planstate_walk_kids(PlanState *planstate,
									  PxVisitOpt (*walker) (PlanState *planstate, void *context),
									  void *context,
									  int flags);


/* ---------------------- POLAR px -----------------------*/

typedef struct MotionAssignerContext
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
	List	   *motStack;		/* Motion Stack */
} MotionAssignerContext;


/**
 * This method is used to determine how much memory a specific operator
 * is supposed to use (in KB).
 */
uint64
polar_PlanStateOperatorMemKB(const PlanState *ps)
{
	uint64		result = 0;

	Assert(ps);
	Assert(ps->plan);
	if (ps->plan->operatorMemKB == 0)
	{
		/**
		 * There are some statements that do not go through the resource queue and these
		 * plans dont get decorated with the operatorMemKB. Someday, we should fix resource queues.
		 */
		result = work_mem;
	}
	else
	{
#if 0
		if (IsA(ps, AggState))
		{
			/*
			 * Retrieve all relinquished memory (quota the other node not
			 * using)
			 */
			result = ps->plan->operatorMemKB + (MemoryAccounting_RequestQuotaIncrease() >> 10);
		}
		else
#endif
			result = ps->plan->operatorMemKB;
	}

	return result;
}

/*
 * POLAR px
 * Walker for search SplitUpdate Node
*/
static bool
SplitSearchWalker(Plan *node, void *ctx)
{
	Assert(ctx);
	if (node == NULL)
		return false;

	if (IsA(node, SplitUpdate))
		return true;
	return plan_tree_walker((Node *) node, SplitSearchWalker, ctx, false);
}

/*
 * POLAR px
 * Walker to set plan->motionNode for every Plan node to its corresponding parent
 * motion node.
 *
 * This function maintains a stack of motion nodes. When we encounter a motion node
 * we push it on to the stack, walk its subtree, and then pop it off the stack.
 * When we encounter any plan node (motion nodes included) we assign its plan->motionNode
 * to the top of the stack.
 *
 * NOTE: Motion nodes will have their motionNode value set to the previous motion node
 * we encountered while walking the subtree.
 */
static bool
MotionAssignerWalker(Plan *node,
					 void *context)
{
	MotionAssignerContext *ctx;

	if (node == NULL)
		return false;

	Assert(context);
	ctx = (MotionAssignerContext *) context;

	if (is_plan_node((Node *) node))
	{
		Plan	   *plan = (Plan *) node;

		/*
		 * TODO: For cached plan we may be assigning multiple times. The
		 * eventual goal is to relocate it to planner. For now, ignore already
		 * assigned nodes.
		 */
		if (NULL != plan->motionNode)
			return true;
		plan->motionNode = ctx->motStack != NIL ? (Plan *) lfirst(list_head(ctx->motStack)) : NULL;
	}

	/*
	 * Subplans get dynamic motion assignment as they can be executed from
	 * arbitrary expressions. So, we don't assign any motion to these nodes.
	 */
	if (IsA(node, SubPlan))
	{
		return false;
	}

	if (IsA(node, Motion))
	{
		ctx->motStack = lcons(node, ctx->motStack);
		plan_tree_walker((Node *) node, MotionAssignerWalker, ctx, true);
		ctx->motStack = list_delete_first(ctx->motStack);

		return false;
	}

	/* Continue walking */
	return plan_tree_walker((Node *) node, MotionAssignerWalker, ctx, true);
}

/*
 * POLAR px
 * Assign every node in plannedstmt->planTree its corresponding
 * parent Motion Node if it has one
 *
 * NOTE: Some plans may not be rooted by a motion on the segment so
 * this function does not guarantee that every node will have a non-NULL
 * motionNode value.
 */
void
AssignParentMotionToPlanNodes(PlannedStmt *plannedstmt)
{
	MotionAssignerContext ctx;

	ctx.base.node = (Node *) plannedstmt;
	ctx.motStack = NIL;

	MotionAssignerWalker(plannedstmt->planTree, &ctx);
	/* The entire motion stack should have been unwounded */
	Assert(ctx.motStack == NIL);
}


/**
 * Provide index of locally executing slice
 */
int
LocallyExecutingSliceIndex(EState *estate)
{
	Assert(estate);
	return (!estate->es_sliceTable ? 0 : estate->es_sliceTable->localSlice);
}

static void
FillSliceGangInfo(ExecSlice *slice, int numsegments, DirectDispatchInfo *dd)
{
	int			k;

	switch (slice->gangType)
	{
		case GANGTYPE_UNALLOCATED:

			/*
			 * It's either the root slice or an InitPlan slice that runs in
			 * the QC process, or really unused slice.
			 */
			slice->planNumSegments = 1;
			break;
		case GANGTYPE_PRIMARY_WRITER:
			/* Write Segments are same as Read Currently */
			slice->planNumSegments = numsegments;

			slice->segments = NIL;

			for (k = 0; k < numsegments; k++)
				slice->segments = lappend_int(slice->segments, k);
			break;
		case GANGTYPE_PRIMARY_READER:
			slice->planNumSegments = numsegments;
			if (dd->isDirectDispatch)
			{
				slice->segments = list_copy(dd->contentIds);
			}
			else
			{
				int			i;

				slice->segments = NIL;
				for (i = 0; i < numsegments; i++)
					slice->segments = lappend_int(slice->segments, i);
			}
			break;
		case GANGTYPE_ENTRYDB_READER:
			slice->planNumSegments = 1;
			slice->segments = list_make1_int(-1);
			break;
		case GANGTYPE_SINGLETON_READER:
			slice->planNumSegments = 1;
			slice->segments = list_make1_int(px_session_id % numsegments);
			break;
		default:
			elog(ERROR, "unexpected gang type");
	}
}

/*
 * POLAR px
 * Create the executor slice table.
 *
 * The planner constructed a slice table, in plannedstmt->slices. Turn that
 * into an "executor slice table", with slightly more information. The gangs
 * to execute the slices will be set up later.
 */
SliceTable *
InitSliceTable(EState *estate, PlannedStmt *plannedstmt)
{
	SliceTable *table;
	int			i;
	int			numSlices;
	MemoryContext oldcontext;

	numSlices = plannedstmt->numSlices;
	Assert(numSlices > 0);

	if (px_max_slices > 0 && numSlices > px_max_slices)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("at most %d slices are allowed in a query, current number: %d",
						px_max_slices, numSlices),
				 errhint("rewrite your query or adjust GUC px_max_slices")));

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	table = makeNode(SliceTable);
	table->instrument_options = 0;
	table->hasMotions = false;

	/*
	 * Initialize the executor slice table.
	 *
	 * We have most of the information in the planner slice table. In addition
	 * to that, we set up the parent-child relationships.
	 */
	table->slices = palloc0(sizeof(ExecSlice) * numSlices);
	for (i = 0; i < numSlices; i++)
	{
		ExecSlice  *currExecSlice = &table->slices[i];
		PlanSlice  *currPlanSlice = &plannedstmt->slices[i];
		int			parentIndex;
		int			rootIndex;

		currExecSlice->sliceIndex = i;
		currExecSlice->planNumSegments = currPlanSlice->numsegments;
		currExecSlice->segments = NIL;
		currExecSlice->primaryGang = NULL;
		currExecSlice->primaryProcesses = NIL;

		parentIndex = currPlanSlice->parentIndex;
		if (parentIndex < -1 || parentIndex >= numSlices)
			elog(ERROR, "invalid parent slice index %d", currPlanSlice->parentIndex);

		if (parentIndex >= 0)
		{
			ExecSlice  *parentExecSlice = &table->slices[parentIndex];
			int			counter;

			/* Sending slice is a child of recv slice */
			parentExecSlice->children = lappend_int(parentExecSlice->children, currPlanSlice->sliceIndex);

			/* Find the root slice */
			rootIndex = i;
			counter = 0;
			while (plannedstmt->slices[rootIndex].parentIndex >= 0)
			{
				rootIndex = plannedstmt->slices[rootIndex].parentIndex;

				if (counter++ > numSlices)
					elog(ERROR, "circular parent-child relationship in slice table");
			}
			table->hasMotions = true;
		}
		else
			rootIndex = i;

		/*
		 * find root of this slice. All the parents should be initialized
		 * already
		 */

		currExecSlice->parentIndex = parentIndex;
		currExecSlice->rootIndex = rootIndex;
		currExecSlice->gangType = currPlanSlice->gangType;

		FillSliceGangInfo(currExecSlice, currPlanSlice->numsegments, &currPlanSlice->directDispatch);
	}
	table->numSlices = numSlices;

	MemoryContextSwitchTo(oldcontext);

	return table;
}

bool
HasMotion(PlannedStmt *plannedstmt)
{
	int			i;

	for (i = 0; i < plannedstmt->numSlices; i++)
	{
		if (plannedstmt->slices[i].parentIndex >= 0)
			return true;
	}
	return false;
}
typedef struct SplitFinderContext
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
} SplitFinderContext;


bool
HasSplit(PlannedStmt *plannedstmt)
{
	SplitFinderContext ctx;

	ctx.base.node = (Node *) plannedstmt;
	if (!plannedstmt || CMD_UPDATE != plannedstmt->commandType || !polar_enable_px || !px_enable_update)
		return false;
	return SplitSearchWalker(plannedstmt->planTree, &ctx);
}

/*
 * POLAR px: A forgiving slice table indexer that returns the indexed Slice* or NULL
 */
ExecSlice *
getCurrentSlice(EState *estate, int sliceIndex)
{
	SliceTable *sliceTable = estate->es_sliceTable;

	if (sliceTable &&
		sliceIndex >= 0 &&
		sliceIndex < sliceTable->numSlices)
		return &sliceTable->slices[sliceIndex];

	return NULL;
}

/*
 * POLAR px
 * Should the slice run on the QC
 * N.B. Not the same as !sliceRunsOnPX(slice), when slice is NULL.
 */
bool
sliceRunsOnQC(ExecSlice *slice)
{
	return (slice != NULL && slice->gangType == GANGTYPE_UNALLOCATED);
}


/*
 * Should the slice run on a PX
 * N.B. Not the same as !sliceRunsOnQC(slice), when slice is NULL.
 */
bool
sliceRunsOnPX(ExecSlice *slice)
{
	return (slice != NULL && slice->gangType != GANGTYPE_UNALLOCATED);
}

/**
 * Calculate the number of sending processes that should in be a slice.
 */
int
sliceCalculateNumSendingProcesses(ExecSlice *slice)
{
	switch (slice->gangType)
	{
		case GANGTYPE_UNALLOCATED:
			return 0;			/* does not send */

		case GANGTYPE_ENTRYDB_READER:
			return 1;			/* on master */

		case GANGTYPE_SINGLETON_READER:
			return 1;			/* on segment */

		case GANGTYPE_PRIMARY_WRITER:
		case GANGTYPE_PRIMARY_READER:
			return list_length(slice->segments);

		default:
			Insist(false);
			return -1;
	}
}

/* Forward declarations */
static void InventorySliceTree(PxDispatcherState *ds, SliceTable *sliceTable, int sliceIndex);

/*
 * AssignWriterGangFirst() - Try to assign writer gang first.
 *
 * For the gang allocation, our current implementation required the first
 * allocated gang must be the writer gang.
 * This has several reasons:
 * - For lock holding, Because of our MPP structure, we assign a LockHolder
 *   for each segment when executing a query. lockHolder is the gang member that
 *   should hold and manage locks for this transaction. On the QEs, it should
 *   normally be the Writer gang member. More details please refer to
 *   lockHolderProcPtr in lock.c.
 * - For SharedSnapshot among session's gang processes on a particular segment.
 *   During initPostgres(), reader QE will try to lookup the shared slot written
 *   by writer QE. More details please reger to sharedsnapshot.c.
 *
 * Normally, the writer slice will be assign writer gang first when iterate the
 * slice table. But this is not true for writable CTE (with only one writer gang).
 * For below statement:
 *
 * WITH updated AS (update tbl set rank = 6 where id = 5 returning rank)
 * select * from tbl where rank in (select rank from updated);
 *                                           QUERY PLAN
 * ----------------------------------------------------------------------------------------------
 *  Gather Motion 3:1  (slice1; segments: 3)
 *    ->  Seq Scan on tbl
 *          Filter: (hashed SubPlan 1)
 *          SubPlan 1
 *            ->  Broadcast Motion 1:3  (slice2; segments: 1)
 *                  ->  Update on tbl
 *                        ->  Seq Scan on tbl
 *                              Filter: (id = 5)
 *  Slice 0: Dispatcher; root 0; parent -1; gang size 0
 *  Slice 1: Reader; root 0; parent 0; gang size 3
 *  Slice 2: Primary Writer; root 0; parent 1; gang size 1
 *
 * If we sill assign writer gang to Slice 1 here, the writer process will execute
 * on reader gang. So, find the writer slice and assign writer gang first.
 */
static bool
AssignWriterGangFirst(PxDispatcherState *ds, SliceTable *sliceTable, int sliceIndex)
{
	ExecSlice  *slice = &sliceTable->slices[sliceIndex];

	if (slice->gangType == GANGTYPE_PRIMARY_WRITER)
	{
		Assert(slice->primaryGang == NULL);
		Assert(slice->segments != NIL);
		slice->primaryGang = AllocateGang(ds, slice->gangType, slice->segments);
		setupPxProcessList(slice);
		return true;
	}
	else
	{
		ListCell   *cell;

		foreach(cell, slice->children)
		{
			int			childIndex = lfirst_int(cell);

			if (AssignWriterGangFirst(ds, sliceTable, childIndex))
				return true;
		}
	}
	return false;
}


/*
 * POLAR px
 * Function AssignGangs runs on the QC and finishes construction of the
 * global slice table for a plan by assigning gangs allocated by the
 * executor factory to the slices of the slice table.
 *
 * On entry, the executor slice table (at queryDesc->estate->es_sliceTable)
 * has been initialized and has correct (by InitSliceTable function)
 *
 * Gang assignment involves taking an inventory of the requirements of
 * each slice tree in the slice table, asking the executor factory to
 * allocate a minimal set of gangs that can satisfy any of the slice trees,
 * and associating the allocated gangs with slices in the slice table.
 *
 * On successful exit, the PXProcess lists (primaryProcesses, mirrorProcesses)
 * and the Gang pointers (primaryGang, mirrorGang) are set correctly in each
 * slice in the slice table.
 */
void
AssignGangs(PxDispatcherState *ds, QueryDesc *queryDesc)
{
	SliceTable *sliceTable;
	EState	   *estate;
	int			rootIdx;
	int			i;

	estate = queryDesc->estate;
	sliceTable = estate->es_sliceTable;
	rootIdx = RootSliceIndex(queryDesc->estate);

	/*
	 * cleanup processMap because initPlan and main Plan share the same slice
	 * table
	 */
	for (i = 0; i < sliceTable->numSlices; i++)
		sliceTable->slices[i].processesMap = NULL;

	/* POLAR px */
	AssignWriterGangFirst(ds, sliceTable, rootIdx);
	/* POLAR end */
	InventorySliceTree(ds, sliceTable, rootIdx);
}

/*
 * Helper for AssignGangs takes a simple inventory of the gangs required
 * by a slice tree.  Recursive.  Closely coupled with AssignGangs.	Not
 * generally useful.
 */
static void
InventorySliceTree(PxDispatcherState *ds, SliceTable *sliceTable, int sliceIndex)
{
	ExecSlice  *slice = &sliceTable->slices[sliceIndex];
	ListCell   *cell;

	if (slice->gangType == GANGTYPE_UNALLOCATED)
	{
		slice->primaryGang = NULL;
		slice->primaryProcesses = getPxProcessesForQC(true);
	}
	else if (!slice->primaryGang)
	{
		Assert(slice->segments != NIL);
		slice->primaryGang = AllocateGang(ds, slice->gangType, slice->segments);
		setupPxProcessList(slice);
	}

	foreach(cell, slice->children)
	{
		int			childIndex = lfirst_int(cell);

		InventorySliceTree(ds, sliceTable, childIndex);
	}
}

/*
 * Choose the execution identity (who does this executor serve?).
 * There are types:
 *
 * 1. No-Op (ignore) -- this occurs when the specified direction is
 *	 NoMovementScanDirection or when px_role is PX_ROLE_QC
 *	 and the current slice belongs to a PX.
 *
 * 2. Executor serves a Root Slice -- this occurs when px_role is
 *   PX_ROLE_UTILITY or the current slice is a root.  It corresponds
 *   to the "normal" path through the executor in that we enter the plan
 *   at the top and count on the motion nodes at the fringe of the top
 *   slice to return without ever calling nodes below them.
 *
 * 3. Executor serves a Non-Root Slice on a PX -- this occurs when
 *   px_role is PX_ROLE_PX and the current slice is not a root
 *   slice. It corresponds to a PX running a slice with a motion node on
 *	 top.  The call, thus, returns no tuples (since they all go out
 *	 on the interconnect to the receiver version of the motion node),
 *	 but it does execute the indicated slice down to any fringe
 *	 motion nodes (as in case 2).
 */
PxExecIdentity
getPxExecIdentity(QueryDesc *queryDesc,
				  ScanDirection direction,
				  EState *estate)
{
	ExecSlice  *currentSlice;

	currentSlice = getCurrentSlice(estate, LocallyExecutingSliceIndex(estate));
	if (currentSlice)
	{
		if (px_role == PX_ROLE_PX ||
			sliceRunsOnQC(currentSlice))
			currentSliceId = currentSlice->sliceIndex;
	}

	/* select the strategy */
	if (direction == NoMovementScanDirection)
	{
		return PX_IGNORE;
	}
	else if (px_role == PX_ROLE_QC && sliceRunsOnPX(currentSlice))
	{
		return PX_IGNORE;
	}
	else if (px_role == PX_ROLE_PX && LocallyExecutingSliceIndex(estate) != RootSliceIndex(estate))
	{
		return PX_NON_ROOT_ON_PX;
	}
	else
	{
		return PX_ROOT_SLICE;
	}
}

/*
 * End the gp-specific part of the executor.
 *
 * In here we collect the dispatch results if there are any, tear
 * down the interconnect if it is set-up.
 */
void
ExecutorFinishup_PX(QueryDesc *queryDesc)
{
	EState	   *estate;

	/* caller must have switched into per-query memory context already */
	estate = queryDesc->estate;

	/* Teardown the Interconnect */
	if (estate->es_interconnect_is_setup)
	{
		TeardownInterconnect(estate->interconnect_context, false);
		estate->interconnect_context = NULL;
		estate->es_interconnect_is_setup = false;
	}

	/*
	 * If QC, wait for PXs to finish and check their results.
	 */
	if (estate->dispatcherState && estate->dispatcherState->primaryResults)
	{
		PxDispatchResults *pr = NULL;
		PxDispatcherState *ds = estate->dispatcherState;
		DispatchWaitMode waitMode = DISPATCH_WAIT_NONE;
		ErrorData  *pxError = NULL;
		int			primaryWriterSliceIndex;

		/*
		 * If we are finishing a query before all the tuples of the query plan
		 * were fetched we must call ExecSquelchNode before checking the
		 * dispatch results in order to tell the nodes below we no longer need
		 * any more tuples.
		 */
		if (!estate->es_got_eos)
		{
			ExecSquelchNode(queryDesc->planstate);
		}

		/*
		 * Wait for completion of all PXs.  We send a "graceful" query finish,
		 * not cancel signal.  Since the query has succeeded, don't confuse
		 * PXs by sending erroneous message.
		 */
		if (estate->cancelUnfinished)
			waitMode = DISPATCH_WAIT_FINISH;

		pxdisp_checkDispatchResult(ds, waitMode);

		pr = pxdisp_getDispatchResults(ds, &pxError);

		if (pxError)
		{
			pxdisp_finishPqThread();
			estate->dispatcherState = NULL;
			FlushErrorState();
			ReThrowError(pxError);
		}

		/* If top slice was delegated to PXs, get num of rows processed. */
		primaryWriterSliceIndex = PrimaryWriterSliceIndex(estate);
		{
			estate->es_processed +=
				pxdisp_sumCmdTuples(pr, primaryWriterSliceIndex);
		}

		/* sum up rejected rows if any (single row error handling only) */
		pxdisp_sumRejectedRows(pr);

		/*
		 * Check and free the results of all gangs. If any PX had an error,
		 * report it and exit to our error handler via PG_THROW. NB: This call
		 * doesn't wait, because we already waited above.
		 */
		pxdisp_finishPqThread();
		estate->dispatcherState = NULL;
		pxdisp_destroyDispatcherState(ds);
	}
}

/*
 * Cleanup the gp-specific parts of the query executor.
 *
 * Will normally be called after an error from within a CATCH block.
 */
void
ExecutorCleanup_PX(QueryDesc *queryDesc)
{
	PxDispatcherState *ds;
	EState	   *estate;

	/* caller must have switched into per-query memory context already */
	estate = queryDesc->estate;
	ds = estate->dispatcherState;

	/* Clean up the interconnect. */
	if (estate->es_interconnect_is_setup)
	{
		TeardownInterconnect(estate->interconnect_context, true);
		estate->es_interconnect_is_setup = false;
	}

	/*
	 * Request any commands still executing on qExecs to stop. Wait for them
	 * to finish and clean up the dispatching structures. Replace current
	 * error info with PX error info if more interesting.
	 */
	if (ds)
	{
		PxDispatchHandleError(ds);

		pxdisp_finishPqThread();
		estate->dispatcherState = NULL;
		pxdisp_destroyDispatcherState(ds);
	}
}

/**
 * Methods to find motionstate object within a planstate tree given a motion id (which is the same as slice index)
 */
typedef struct MotionStateFinderContext
{
	int			motionId;		/* Input */
	MotionState *motionState;	/* Output */
} MotionStateFinderContext;

/**
 * Walker method that finds motion state node within a planstate tree.
 */
static PxVisitOpt
MotionStateFinderWalker(PlanState *node,
						void *context)
{
	MotionStateFinderContext *ctx = (MotionStateFinderContext *) context;

	Assert(context);

	if (IsA(node, MotionState))
	{
		MotionState *ms = (MotionState *) node;
		Motion	   *m = (Motion *) ms->ps.plan;

		if (m->motionID == ctx->motionId)
		{
			Assert(ctx->motionState == NULL);
			ctx->motionState = ms;
			return PxVisit_Skip;	/* don't visit subtree */
		}
	}

	/* Continue walking */
	return PxVisit_Walk;
}

/**
 * Given a slice index, find the motionstate that corresponds to this slice index. This will iterate over the planstate tree
 * to get the right node.
 */
MotionState *
getMotionState(struct PlanState *ps, int sliceIndex)
{
	MotionStateFinderContext ctx;

	Assert(ps);
	Assert(sliceIndex > -1);

	ctx.motionId = sliceIndex;
	ctx.motionState = NULL;
	planstate_walk_node(ps, MotionStateFinderWalker, &ctx);

	if (ctx.motionState == NULL)
		elog(ERROR, "could not find MotionState for slice %d in executor tree", sliceIndex);

	return ctx.motionState;
}

typedef struct MotionFinderContext
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
	int			motionId;		/* Input */
	Motion	   *motion;			/* Output */
} MotionFinderContext;

/*
 * Walker to find a motion node that matches a particular motionID
 */
static bool
MotionFinderWalker(Plan *node,
				   void *context)
{
	MotionFinderContext *ctx = (MotionFinderContext *) context;

	Assert(context);

	if (node == NULL)
		return false;

	if (IsA(node, Motion))
	{
		Motion	   *m = (Motion *) node;

		if (m->motionID == ctx->motionId)
		{
			ctx->motion = m;
			return true;		/* found our node; no more visit */
		}
	}

	/* Continue walking */
	return plan_tree_walker((Node *) node, MotionFinderWalker, ctx, true);
}

/*
 * Given the Plan and a Slice index, find the motion node that is the root of the slice's subtree.
 */
Motion *
findSenderMotion(PlannedStmt *plannedstmt, int sliceIndex)
{
	Plan	   *planTree = plannedstmt->planTree;
	MotionFinderContext ctx;

	Assert(sliceIndex > -1);

	ctx.base.node = (Node *) plannedstmt;
	ctx.motionId = sliceIndex;
	ctx.motion = NULL;
	MotionFinderWalker(planTree, &ctx);
	return ctx.motion;
}

typedef struct ParamExtractorContext
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
	EState	   *estate;
} ParamExtractorContext;

/*
 * Given a subplan determine if it is an initPlan (subplan->is_initplan) then copy its params
 * from estate-> es_param_list_info to estate->es_param_exec_vals.
 */
static void
ExtractSubPlanParam(SubPlan *subplan, EState *estate)
{
	/*
	 * If this plan is un-correlated or undirect correlated one and want to
	 * set params for parent plan then mark parameters as needing evaluation.
	 *
	 * Note that in the case of un-correlated subqueries we don't care about
	 * setting parent->chgParam here: indices take care about it, for others -
	 * it doesn't matter...
	 */
	if (subplan->setParam != NIL)
	{
		ListCell   *lst;

		foreach(lst, subplan->setParam)
		{
			int			paramid = lfirst_int(lst);
			ParamExecData *prmExec = &(estate->es_param_exec_vals[paramid]);

			/**
			 * Has this parameter been already
			 * evaluated as part of preprocess_initplan()? If so,
			 * we shouldn't re-evaluate it. If it has been evaluated,
			 * we will simply substitute the actual value from
			 * the external parameters.
			 */
			if (subplan->is_initplan)
			{
				ParamListInfo paramInfo = estate->es_param_list_info;
				ParamExternData *prmExt = NULL;
				int			extParamIndex = -1;

				Assert(paramInfo);
				Assert(paramInfo->numParams > 0);

				/*
				 * To locate the value of this pre-evaluated parameter, we
				 * need to find its location in the external parameter list.
				 */
				extParamIndex = paramInfo->numParams - estate->es_plannedstmt->nParamExec + paramid;
				prmExt = &paramInfo->params[extParamIndex];

				/* Make sure the types are valid */
				if (!OidIsValid(prmExt->ptype))
				{
					prmExec->execPlan = NULL;
					prmExec->isnull = true;
					prmExec->value = (Datum) 0;
				}
				else
				{
					/** Hurray! Copy value from external parameter and don't bother setting up execPlan. */
					prmExec->execPlan = NULL;
					prmExec->isnull = prmExt->isnull;
					prmExec->value = prmExt->value;
				}
			}
		}
	}
}

/*
 * Walker to extract all the precomputer InitPlan params in a plan tree.
 */
static bool
ParamExtractorWalker(Plan *node,
					 void *context)
{
	ParamExtractorContext *ctx = (ParamExtractorContext *) context;

	Assert(context);

	/* Assuming InitPlan always runs on the master */
	if (node == NULL)
	{
		return false;			/* don't visit subtree */
	}

	if (IsA(node, SubPlan))
	{
		SubPlan    *sub_plan = (SubPlan *) node;

		ExtractSubPlanParam(sub_plan, ctx->estate);
	}

	/* Continue walking */
	return plan_tree_walker((Node *) node, ParamExtractorWalker, ctx, true);
}

/*
 * Find and extract all the InitPlan setParams in a root node's subtree.
 */
void
ExtractParamsFromInitPlans(PlannedStmt *plannedstmt, Plan *root, EState *estate)
{
	ParamExtractorContext ctx;

	ctx.base.node = (Node *) plannedstmt;
	ctx.estate = estate;

	Assert(px_role == PX_ROLE_PX && px_is_executing);

	ParamExtractorWalker(root, &ctx);
}



/**
 * Provide index of slice being executed on the primary writer gang
 */
int
PrimaryWriterSliceIndex(EState *estate)
{
	int			i;

	if (!estate->es_sliceTable)
		return 0;

	for (i = 0; i < estate->es_sliceTable->numSlices; i++)
	{
		ExecSlice  *slice = &estate->es_sliceTable->slices[i];

		if (slice->gangType == GANGTYPE_PRIMARY_WRITER)
			return slice->sliceIndex;
	}

	return 0;
}

/**
 * Provide root slice of locally executing slice.
 */
int
RootSliceIndex(EState *estate)
{
	int			result = 0;

	if (estate->es_sliceTable)
	{
		ExecSlice  *localSlice = &estate->es_sliceTable->slices[LocallyExecutingSliceIndex(estate)];

		result = localSlice->rootIndex;

		Assert(result >= 0 && estate->es_sliceTable->numSlices);
	}

	return result;
}

void
ExecAssignResultType(PlanState *planstate, TupleDesc tupDesc)
{
	TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc);
}

void
ExecAssignResultTypeFromTL(PlanState *planstate)
{
	TupleDesc	tupDesc;

	/*
	 * ExecTypeFromTL needs the parse-time representation of the tlist, not a
	 * list of ExprStates.  This is good because some plan nodes don't bother
	 * to set up planstate->targetlist ...
	 */
	tupDesc = ExecTypeFromTL(planstate->plan->targetlist);
	ExecAssignResultType(planstate, tupDesc);
}


/* -----------------------------------------------------------------------
 *						POLAR px
 *				PlanState Tree Walking Functions
 * -----------------------------------------------------------------------
 *
 * planstate_walk_node
 *	  Calls a 'walker' function for the given PlanState node; or returns
 *	  PxVisit_Walk if 'planstate' is NULL.
 *
 *	  If 'walker' returns PxVisit_Walk, then this function calls
 *	  planstate_walk_kids() to visit the node's children, and returns
 *	  the result.
 *
 *	  If 'walker' returns PxVisit_Skip, then this function immediately
 *	  returns PxVisit_Walk and does not visit the node's children.
 *
 *	  If 'walker' returns PxVisit_Stop or another value, then this function
 *	  immediately returns that value and does not visit the node's children.
 *
 * planstate_walk_array
 *	  Calls planstate_walk_node() for each non-NULL PlanState ptr in
 *	  the given array of pointers to PlanState objects.
 *
 *	  Quits if the result of planstate_walk_node() is PxVisit_Stop or another
 *	  value other than PxVisit_Walk, and returns that result without visiting
 *	  any more nodes.
 *
 *	  Returns PxVisit_Walk if 'planstates' is NULL, or if all of the
 *	  subtrees return PxVisit_Walk.
 *
 *	  Note that this function never returns PxVisit_Skip to its caller.
 *	  Only the caller's 'walker' function can return PxVisit_Skip.
 *
 * planstate_walk_list
 *	  Calls planstate_walk_node() for each PlanState node in the given List.
 *
 *	  Quits if the result of planstate_walk_node() is PxVisit_Stop or another
 *	  value other than PxVisit_Walk, and returns that result without visiting
 *	  any more nodes.
 *
 *	  Returns PxVisit_Walk if all of the subtrees return PxVisit_Walk, or
 *	  if the list is empty.
 *
 *	  Note that this function never returns PxVisit_Skip to its caller.
 *	  Only the caller's 'walker' function can return PxVisit_Skip.
 *
 * planstate_walk_kids
 *	  Calls planstate_walk_node() for each child of the given PlanState node.
 *
 *	  Quits if the result of planstate_walk_node() is PxVisit_Stop or another
 *	  value other than PxVisit_Walk, and returns that result without visiting
 *	  any more nodes.
 *
 *	  Returns PxVisit_Walk if the given planstate node ptr is NULL, or if
 *	  all of the children return PxVisit_Walk, or if there are no children.
 *
 *	  Note that this function never returns PxVisit_Skip to its caller.
 *	  Only the 'walker' can return PxVisit_Skip.
 *
 * NB: All PxVisitOpt values other than PxVisit_Walk or PxVisit_Skip are
 * treated as equivalent to PxVisit_Stop.  Thus the walker can break out
 * of a traversal and at the same time return a smidgen of information to the
 * caller, perhaps to indicate the reason for termination.  For convenience,
 * a couple of alternative stopping codes are predefined for walkers to use at
 * their discretion: PxVisit_Failure and PxVisit_Success.
 *
 * NB: We do not visit the left subtree of a NestLoopState node (NJ) whose
 * 'shared_outer' flag is set.  This occurs when the NJ is the left child of
 * an AdaptiveNestLoopState (AJ); the AJ's right child is a HashJoinState (HJ);
 * and both the NJ and HJ point to the same left subtree.  This way we avoid
 * visiting the common subtree twice when descending through the AJ node.
 * The caller's walker function can handle the NJ as a special case to
 * override this behavior if there is a need to always visit both subtrees.
 *
 * NB: Use PSW_* flags to skip walking certain parts of the planstate tree.
 * -----------------------------------------------------------------------
 */

/**
 * Version of walker that uses no flags.
 */
PxVisitOpt
planstate_walk_node(PlanState *planstate,
					PxVisitOpt (*walker) (PlanState *planstate, void *context),
					void *context)
{
	return planstate_walk_node_extended(planstate, walker, context, 0);
}

/**
 * Workhorse walker that uses flags.
 */
PxVisitOpt
planstate_walk_node_extended(PlanState *planstate,
							 PxVisitOpt (*walker) (PlanState *planstate, void *context),
							 void *context,
							 int flags)
{
	PxVisitOpt	whatnext;

	if (planstate == NULL)
		whatnext = PxVisit_Walk;
	else
	{
		whatnext = walker(planstate, context);
		if (whatnext == PxVisit_Walk)
			whatnext = planstate_walk_kids(planstate, walker, context, flags);
		else if (whatnext == PxVisit_Skip)
			whatnext = PxVisit_Walk;
	}
	Assert(whatnext != PxVisit_Skip);
	return whatnext;
}								/* planstate_walk_node */

PxVisitOpt
planstate_walk_array(PlanState **planstates,
					 int nplanstate,
					 PxVisitOpt (*walker) (PlanState *planstate, void *context),
					 void *context,
					 int flags)
{
	PxVisitOpt	whatnext = PxVisit_Walk;
	int			i;

	if (planstates == NULL)
		return PxVisit_Walk;

	for (i = 0; i < nplanstate && whatnext == PxVisit_Walk; i++)
		whatnext = planstate_walk_node_extended(planstates[i], walker, context, flags);

	return whatnext;
}								/* planstate_walk_array */

PxVisitOpt
planstate_walk_kids(PlanState *planstate,
					PxVisitOpt (*walker) (PlanState *planstate, void *context),
					void *context,
					int flags)
{
	PxVisitOpt	v;

	if (planstate == NULL)
		return PxVisit_Walk;

	switch (nodeTag(planstate))
	{
		case T_NestLoopState:
			{
				v = planstate_walk_node_extended(planstate->lefttree, walker, context, flags);

				/* Right subtree */
				if (v == PxVisit_Walk)
					v = planstate_walk_node_extended(planstate->righttree, walker, context, flags);
				break;
			}

		case T_AppendState:
			{
				AppendState *as = (AppendState *) planstate;

				v = planstate_walk_array(as->appendplans, as->as_nplans, walker, context, flags);
				Assert(!planstate->lefttree && !planstate->righttree);
				break;
			}

		case T_MergeAppendState:
			{
				MergeAppendState *ms = (MergeAppendState *) planstate;

				v = planstate_walk_array(ms->mergeplans, ms->ms_nplans, walker, context, flags);
				Assert(!planstate->lefttree && !planstate->righttree);
				break;
			}

		case T_ModifyTableState:
			{
				elog(ERROR, "pg15_px: adapt plan tree walker to renewed ModifyTable node");
#if 0
				ModifyTableState *mts = (ModifyTableState *) planstate;

				v = planstate_walk_array(mts->mt_plans, mts->mt_nplans, walker, context, flags);
				Assert(!planstate->lefttree && !planstate->righttree);
#endif
				break;
			}

			/* POLAR px */
		case T_SequenceState:
			{
				SequenceState *ss = (SequenceState *) planstate;

				v = planstate_walk_array(ss->subplans, ss->numSubplans, walker, context, flags);
				Assert(!planstate->lefttree && !planstate->righttree);
				break;
			}

		case T_BitmapAndState:
			{
				BitmapAndState *bas = (BitmapAndState *) planstate;

				v = planstate_walk_array(bas->bitmapplans, bas->nplans, walker, context, flags);
				Assert(!planstate->lefttree && !planstate->righttree);
				break;
			}
		case T_BitmapOrState:
			{
				BitmapOrState *bos = (BitmapOrState *) planstate;

				v = planstate_walk_array(bos->bitmapplans, bos->nplans, walker, context, flags);
				Assert(!planstate->lefttree && !planstate->righttree);
				break;
			}

		case T_SubqueryScanState:
			v = planstate_walk_node_extended(((SubqueryScanState *) planstate)->subplan, walker, context, flags);
			Assert(!planstate->lefttree && !planstate->righttree);
			break;

		default:
			/* Left subtree */
			v = planstate_walk_node_extended(planstate->lefttree, walker, context, flags);

			/* Right subtree */
			if (v == PxVisit_Walk)
				v = planstate_walk_node_extended(planstate->righttree, walker, context, flags);
			break;
	}

	/* Init plan subtree */
	if (!(flags & PSW_IGNORE_INITPLAN)
		&& (v == PxVisit_Walk))
	{
		ListCell   *lc = NULL;
		PxVisitOpt	v1 = v;

		foreach(lc, planstate->initPlan)
		{
			SubPlanState *sps = (SubPlanState *) lfirst(lc);
			PlanState  *ips = sps->planstate;

			Assert(ips);
			if (v1 == PxVisit_Walk)
			{
				v1 = planstate_walk_node_extended(ips, walker, context, flags);
			}
		}
	}

	/* Sub plan subtree */
	if (v == PxVisit_Walk)
	{
		ListCell   *lc = NULL;
		PxVisitOpt	v1 = v;

		foreach(lc, planstate->subPlan)
		{
			SubPlanState *sps = (SubPlanState *) lfirst(lc);
			PlanState  *ips = sps->planstate;

			if (!ips)
				elog(ERROR, "subplan has no planstate");
			if (v1 == PxVisit_Walk)
			{
				v1 = planstate_walk_node_extended(ips, walker, context, flags);
			}
		}

	}

	return v;
}								/* planstate_walk_kids */

/* ---------------------- POLAR px -----------------------*/
static bool
querytree_safe_for_px_walker(Node *expr, void *context)
{
	Assert(context == NULL);

	if (!expr)
	{
		/**
		 * Do not end recursion just because we have reached one leaf node.
		 */
		return false;
	}

	switch (nodeTag(expr))
	{
		case T_Query:
			{
				Query	   *q = (Query *) expr;

				/* POLAR px: allow_segment_DML should be guc param */
				bool		allow_segment_DML = false;
				ListCell   *f = NULL;

				if (!allow_segment_DML &&
					(q->commandType != CMD_SELECT
					 || (q->utilityStmt != NULL &&
						 IsA(q->utilityStmt, CreateTableAsStmt))
					 || q->resultRelation > 0))
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("function cannot execute on a PX slice because it issues a non-SELECT statement")));
				}

				foreach(f, q->rtable)
				{
					RangeTblEntry *rte = (RangeTblEntry *) lfirst(f);

					if (rte->rtekind == RTE_RELATION)
					{
						Oid			namespaceId;

						Assert(rte->relid != InvalidOid);

						namespaceId = get_rel_namespace(rte->relid);

						Assert(namespaceId != InvalidOid);

						if (!px_enable_spi_read_all_namespaces &&
							(!(IsCatalogNamespace(namespaceId) || IsToastNamespace(namespaceId)))
							)
						{
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("function cannot execute on a PX slice because it accesses relation \"%s.%s\"",
											quote_identifier(get_namespace_name(namespaceId)),
											quote_identifier(get_rel_name(rte->relid)))));
						}
					}
				}
				query_tree_walker(q, querytree_safe_for_px_walker, context, 0);
				break;
			}
		default:
			break;
	}

	return expression_tree_walker(expr, querytree_safe_for_px_walker, context);
}

/**
 * POLAR px
 * This function determines if the query tree is safe to be planned and
 * executed on a PX. The checks it performs are:
 * 1. The query cannot access any non-catalog relation except it's a replicated table.
 * 2. The query must be select only.
 * In case of a problem, the method spits out an error.
 */
void
querytree_safe_for_px(Node *node)
{
	Assert(node);
	querytree_safe_for_px_walker(node, NULL);
}


/*
 * POLAR px
 * polar_fake_outer_params
 *   helper function to fake the nestloop's nestParams
 *   so that prefetch inner or prefetch joinqual will
 *   not encounter NULL pointer reference issue. It is
 *   only invoked in ExecNestLoop and ExecPrefetchJoinQual
 *   when the join is a nestloop join.
 */
void
polar_fake_outer_params(JoinState *node)
{
	ExprContext *econtext = node->ps.ps_ExprContext;
	PlanState  *inner = innerPlanState(node);
	TupleTableSlot *outerTupleSlot = econtext->ecxt_outertuple;
	NestLoop   *nl = (NestLoop *) (node->ps.plan);
	ListCell   *lc = NULL;

	/* only nestloop contains nestParams */
	Assert(IsA(node->ps.plan, NestLoop));

	/* econtext->ecxt_outertuple must have been set fakely. */
	Assert(outerTupleSlot != NULL);

	/*
	 * fetch the values of any outer Vars that must be passed to the inner
	 * scan, and store them in the appropriate PARAM_EXEC slots.
	 */
	foreach(lc, nl->nestParams)
	{
		NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);
		int			paramno = nlp->paramno;
		ParamExecData *prm;

		prm = &(econtext->ecxt_param_exec_vals[paramno]);
		/* Param value should be an OUTER_VAR var */
		Assert(IsA(nlp->paramval, Var));
		Assert(nlp->paramval->varno == OUTER_VAR);
		Assert(nlp->paramval->varattno > 0);
		prm->value = slot_getattr(outerTupleSlot,
								  nlp->paramval->varattno,
								  &(prm->isnull));
		/* Flag parameter value as changed */
		inner->chgParam = bms_add_member(inner->chgParam,
										 paramno);
	}
}
