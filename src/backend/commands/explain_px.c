/*-------------------------------------------------------------------------
 *
 * explain_px.c
 *	  Functions supporting the PolarDB PX extensions to EXPLAIN ANALYZE
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	    src/backend/commands/explain_px.c
 *
 *-------------------------------------------------------------------------
 */

#include "portability/instr_time.h"

#include "libpq-fe.h"
#include "libpq-int.h"
#include "px/px_conn.h"			/* SegmentDatabaseDescriptor */
#include "px/px_disp.h"			/* CheckDispatchResult() */
#include "px/px_dispatchresult.h"	/* PxDispatchResults */
#include "px/px_explain.h"		/* me */
#include "libpq/pqformat.h"		/* pq_beginmessage() etc. */
#include "miscadmin.h"
#include "utils/memutils.h"		/* MemoryContextGetPeakSpace() */

#include "px/px_explain.h"		/* pxexplain_recvExecStats */
#include <math.h>
#define NUM_SORT_METHOD 5

#define TOP_N_HEAP_SORT_STR "top-N heapsort"
#define QUICK_SORT_STR "quicksort"
#define EXTERNAL_SORT_STR "external sort"
#define EXTERNAL_MERGE_STR "external merge"
#define IN_PROGRESS_SORT_STR "sort still in progress"

#define NUM_SORT_SPACE_TYPE 2
#define MEMORY_STR_SORT_SPACE_TYPE "Memory"
#define DISK_STR_SORT_SPACE_TYPE "Disk"

/* Convert bytes into kilobytes */
#define kb(x) (floor((x + 1023.0) / 1024.0))

/*
 * Different sort method in POLAR PX.
 *
 * Make sure to update NUM_SORT_METHOD when this enum changes.
 * This enum value is used an index in the array sortSpaceUsed
 * in struct PxExplain_NodeSummary.
 */
typedef enum
{
	UNINITIALIZED_SORT = 0,
	TOP_N_HEAP_SORT = 1,
	QUICK_SORT = 2,
	EXTERNAL_SORT = 3,
	EXTERNAL_MERGE = 4,
	IN_PROGRESS_SORT = 5
} ExplainSortMethod;

typedef enum
{
	UNINITIALIZED_SORT_SPACE_TYPE = 0,
	MEMORY_SORT_SPACE_TYPE = 1,
	DISK_SORT_SPACE_TYPE = 2
} ExplainSortSpaceType;

/*
 * Convert the above enum `ExplainSortMethod` to printable string for
 * Explain Analyze.
 * Note : No conversion available for `UNINITALIZED_SORT`. Caller has to index
 * this array by subtracting 1 from origin enum value.
 *
 * E.g. sort_method_enum_str[TOP_N_HEAP_SORT-1]
 */
const char *sort_method_enum_str[] = {
	TOP_N_HEAP_SORT_STR,
	QUICK_SORT_STR,
	EXTERNAL_SORT_STR,
	EXTERNAL_MERGE_STR,
	IN_PROGRESS_SORT_STR
};

/* EXPLAIN ANALYZE statistics for one plan node of a slice */
typedef struct PxExplain_StatInst
{
	NodeTag		pstype;			/* PlanState node type */
	instr_time	starttime;		/* Start time of current iteration of node */
	instr_time	counter;		/* Accumulated runtime for this node */
	double		firsttuple;		/* Time for first tuple of this cycle */
	double		startup;		/* Total startup time (in seconds) */
	double		total;			/* Total total time (in seconds) */
	double		ntuples;		/* Total tuples produced */
	double		nloops;			/* # of run cycles for this node */
	double		execmemused;	/* executor memory used (bytes) */
	double		workmemused;	/* work_mem actually used (bytes) */
	double		workmemwanted;	/* work_mem to avoid workfile i/o (bytes) */
	bool		workfileCreated;	/* workfile created in this node */
	instr_time	firststart;		/* Start time of first iteration of node */
	double		peakMemBalance; /* Max mem account balance */
	int			numPartScanned; /* Number of part tables scanned */
	TuplesortInstrumentation sortstats; /* Sort stats, if this is a Sort node */
	HashInstrumentation hashstats;	/* Hash stats, if this is a Hash node */
	int			bnotes;			/* Offset to beginning of node's extra text */
	int			enotes;			/* Offset to end of node's extra text */
} PxExplain_StatInst;


/* EXPLAIN ANALYZE statistics for one process working on one slice */
typedef struct PxExplain_SliceWorker
{
	double		peakmemused;	/* bytes alloc in per-query mem context tree */
	double		vmem_reserved;	/* vmem reserved by a QE */
	double		memory_accounting_global_peak;	/* peak memory observed during
												 * memory accounting */
} PxExplain_SliceWorker;


/* Header of EXPLAIN ANALYZE statistics message sent from qExec to qDisp */
typedef struct PxExplain_StatHdr
{
	NodeTag		type;			/* T_PxExplain_StatHdr */
	int			segindex;		/* segment id */
	int			nInst;			/* num of StatInst entries following StatHdr */
	int			bnotes;			/* offset to extra text area */
	int			enotes;			/* offset to end of extra text area */

	int			memAccountCount;	/* How many mem account we serialized */
	int			memAccountStartOffset;	/* Where in the header our memory
										 * account array is serialized */

	PxExplain_SliceWorker worker;	/* qExec's overall stats for slice */

	/*
	 * During serialization, we use this as a temporary StatInst and save
	 * "one-at-a-time" StatInst into this variable. We then write this
	 * variable into buffer (serialize it) and then "recycle" the same inst
	 * for next plan node's StatInst. During deserialization, an Array
	 * [0..nInst-1] of StatInst entries is appended starting here.
	 */
	PxExplain_StatInst inst[1];

	/* extra text is appended after that */
} PxExplain_StatHdr;


/* Dispatch status summarized over workers in a slice */
typedef struct PxExplain_DispatchSummary
{
	int			nResult;
	int			nOk;
	int			nError;
	int			nCanceled;
	int			nNotDispatched;
	int			nIgnorableError;
} PxExplain_DispatchSummary;


/* One node's EXPLAIN ANALYZE statistics for all the workers of its segworker group */
typedef struct PxExplain_NodeSummary
{
	/* Summary over all the node's workers */
	PxExplain_Agg ntuples;
	PxExplain_Agg execmemused;
	PxExplain_Agg workmemused;
	PxExplain_Agg workmemwanted;
	PxExplain_Agg totalWorkfileCreated;
	PxExplain_Agg peakMemBalance;
	/* Used for DynamicSeqScan, DynamicIndexScan and DynamicBitmapHeapScan */
	PxExplain_Agg totalPartTableScanned;
	/* Summary of space used by sort */
	PxExplain_Agg sortSpaceUsed[NUM_SORT_SPACE_TYPE][NUM_SORT_METHOD];

	/* insts array info */
	int			segindex0;		/* segment id of insts[0] */
	int			ninst;			/* num of StatInst entries in inst array */

	/* Array [0..ninst-1] of StatInst entries is appended starting here */
	PxExplain_StatInst insts[1];	/* variable size - must be last */
} PxExplain_NodeSummary;


/* One slice's statistics for all the workers of its segworker group */
typedef struct PxExplain_SliceSummary
{
	ExecSlice  *slice;

	/* worker array */
	int			nworker;		/* num of SliceWorker slots in worker array */
	int			segindex0;		/* segment id of workers[0] */
	PxExplain_SliceWorker *workers; /* -> array [0..nworker-1] of SliceWorker */

	/*
	 * We use void ** as we don't have access to MemoryAccount struct, which
	 * is private to memory accounting framework
	 */
	void	  **memoryAccounts; /* Array of pointers to serialized memory
								 * accounts array, one array per worker
								 * [0...nworker-1]. */

#if 0
	MemoryAccountIdType *memoryAccountCount;	/* Array of memory account
												 * counts, one per slice */
#endif

	PxExplain_Agg peakmemused;	/* Summary of SliceWorker stats over all of
								 * the slice's workers */

	PxExplain_Agg vmem_reserved;	/* vmem reserved by QEs */

	PxExplain_Agg memory_accounting_global_peak;	/* Peak memory accounting
													 * balance by QEs */

	/* Rollup of per-node stats over all of the slice's workers and nodes */
	double		workmemused_max;
	double		workmemwanted_max;

	/* How many workers were dispatched and returned results? (0 if local) */
	PxExplain_DispatchSummary dispatchSummary;
} PxExplain_SliceSummary;


/* State for pxexplain_showExecStats() */
typedef struct PxExplain_ShowStatCtx
{
	StringInfoData extratextbuf;
	instr_time	querystarttime;

	/* Rollup of per-node stats over the entire query plan */
	double		workmemused_max;
	double		workmemwanted_max;

	bool		stats_gathered;
	/* Per-slice statistics are deposited in this SliceSummary array */
	int			nslice;			/* num of slots in slices array */
	PxExplain_SliceSummary *slices; /* -> array[0..nslice-1] of SliceSummary */
} PxExplain_ShowStatCtx;


/* State for pxexplain_sendStatWalker() and pxexplain_collectStatsFromNode() */
typedef struct PxExplain_SendStatCtx
{
	StringInfoData *notebuf;
	StringInfoData buf;
	PxExplain_StatHdr hdr;
} PxExplain_SendStatCtx;


/* State for pxexplain_recvStatWalker() and pxexplain_depositStatsToNode() */
typedef struct PxExplain_RecvStatCtx
{
	/*
	 * iStatInst is the current StatInst serial during the depositing process
	 * for a slice. We walk the plan tree, and for each node we deposit stat
	 * from all the QEs of the segworker group for current slice. After we
	 * finish one node, we increase iStatInst, which means we are done with
	 * one plan node's stat across all segments and now moving forward to the
	 * next one. Once we are done processing all the plan node of a PARTICULAR
	 * slice, then we switch to the next slice, read the messages from all the
	 * QEs of the next slice (another segworker group) store them in the
	 * msgptrs, reset the iStatInst and then start parsing these messages and
	 * depositing them in the nodes of the new slice.
	 */
	int			iStatInst;

	/*
	 * nStatInst is the total number of StatInst for current slice. Typically
	 * this is the number of plan nodes in the current slice.
	 */
	int			nStatInst;

	/*
	 * segIndexMin is the min of segment index from which we collected message
	 * (i.e., saved msgptrs)
	 */
	int			segindexMin;

	/*
	 * segIndexMax is the max of segment index from which we collected message
	 * (i.e., saved msgptrs)
	 */
	int			segindexMax;

	/*
	 * We deposit stat for one slice at a time. sliceIndex saves the current
	 * slice
	 */
	int			sliceIndex;

	/*
	 * The number of msgptrs that we have saved for current slice. This is
	 * typically the number of QE processes
	 */
	int			nmsgptr;
	/* The actual messages. Contains an array of StatInst too */
	PxExplain_StatHdr **msgptrs;
	PxDispatchResults *dispatchResults;
	StringInfoData *extratextbuf;
	PxExplain_ShowStatCtx *showstatctx;

	/* Rollup of per-node stats over all of the slice's workers and nodes */
	double		workmemused_max;
	double		workmemwanted_max;
} PxExplain_RecvStatCtx;


/* State for pxexplain_localStatWalker() */
typedef struct PxExplain_LocalStatCtx
{
	PxExplain_SendStatCtx send;
	PxExplain_RecvStatCtx recv;
	PxExplain_StatHdr *msgptrs[1];
} PxExplain_LocalStatCtx;

static void pxexplain_showExecStatsEnd(struct PlannedStmt *stmt,
									   struct PxExplain_ShowStatCtx *showstatctx,
									   struct EState *estate,
									   ExplainState *es);
static void pxexplain_showExecStats(struct PlanState *planstate,
									ExplainState *es);

static PxVisitOpt pxexplain_localStatWalker(PlanState *planstate,
											void *context);
static PxVisitOpt pxexplain_sendStatWalker(PlanState *planstate,
										   void *context);
static PxVisitOpt pxexplain_recvStatWalker(PlanState *planstate,
										   void *context);
static void pxexplain_collectSliceStats(PlanState *planstate,
										PxExplain_SliceWorker *out_worker);
static void pxexplain_depositSliceStats(PxExplain_StatHdr *hdr,
										PxExplain_RecvStatCtx *recvstatctx);
static void pxexplain_collectStatsFromNode(PlanState *planstate,
										   PxExplain_SendStatCtx *ctx);
static void pxexplain_depositStatsToNode(PlanState *planstate,
										 PxExplain_RecvStatCtx *ctx);
static int	pxexplain_collectExtraText(PlanState *planstate,
									   StringInfo notebuf);
static void pxexplain_formatSlicesOutput(struct PxExplain_ShowStatCtx *showstatctx,
										 struct EState *estate,
										 ExplainState *es);


/*
 * PxExplain_DepStatAcc
 *	  Segment statistic accumulator used by pxexplain_depositStatsToNode().
 */
typedef struct PxExplain_DepStatAcc
{
	/* vmax, vsum, vcnt, segmax */
	PxExplain_Agg agg;
	/* max's received StatHdr */
	PxExplain_StatHdr *rshmax;
	/* max's received inst in StatHdr */
	PxExplain_StatInst *rsimax;
	/* max's inst in NodeSummary */
	PxExplain_StatInst *nsimax;
	/* max run-time of all the segments */
	double		max_total;
	/* start time of the first iteration for node with maximum runtime */
	instr_time	firststart_of_max_total;
} PxExplain_DepStatAcc;

static void
pxexplain_depStatAcc_init0(PxExplain_DepStatAcc *acc)
{
	pxexplain_agg_init0(&acc->agg);
	acc->rshmax = NULL;
	acc->rsimax = NULL;
	acc->nsimax = NULL;
	acc->max_total = 0;
	INSTR_TIME_SET_ZERO(acc->firststart_of_max_total);
}								/* pxexplain_depStatAcc_init0 */

static inline void
pxexplain_depStatAcc_upd(PxExplain_DepStatAcc *acc,
						 double v,
						 PxExplain_StatHdr *rsh,
						 PxExplain_StatInst *rsi,
						 PxExplain_StatInst *nsi)
{
	if (pxexplain_agg_upd(&acc->agg, v, rsh->segindex))
	{
		acc->rshmax = rsh;
		acc->rsimax = rsi;
		acc->nsimax = nsi;
	}

	if (acc->max_total < nsi->total)
	{
		acc->max_total = nsi->total;
		INSTR_TIME_ASSIGN(acc->firststart_of_max_total, nsi->firststart);
	}
}								/* pxexplain_depStatAcc_upd */

static void
pxexplain_depStatAcc_saveText(PxExplain_DepStatAcc *acc,
							  StringInfoData *extratextbuf,
							  bool *saved_inout)
{
	PxExplain_StatHdr *rsh = acc->rshmax;
	PxExplain_StatInst *rsi = acc->rsimax;
	PxExplain_StatInst *nsi = acc->nsimax;

	if (acc->agg.vcnt > 0 &&
		nsi->bnotes == nsi->enotes &&
		rsi->bnotes < rsi->enotes)
	{
		/* Locate extra message text in dispatch result buffer. */
		int			notelen = rsi->enotes - rsi->bnotes;
		const char *notes = (const char *) rsh + rsh->bnotes + rsi->bnotes;

		Insist(rsh->bnotes + rsi->enotes < rsh->enotes &&
			   notes[notelen] == '\0');

		/* Append to extratextbuf. */
		nsi->bnotes = extratextbuf->len;
		appendBinaryStringInfo(extratextbuf, notes, notelen);
		nsi->enotes = extratextbuf->len;

		/* Tell caller that some extra text has been saved. */
		if (saved_inout)
			*saved_inout = true;
	}
}								/* pxexplain_depStatAcc_saveText */

/*
 * pxexplain_localExecStats
 *	  Called by qDisp to build NodeSummary and SliceSummary blocks
 *	  containing EXPLAIN ANALYZE statistics for a root slice that
 *	  has been executed locally in the qDisp process.  Attaches these
 *	  structures to the PlanState nodes' Instrumentation objects for
 *	  later use by pxexplain_showExecStats().
 *
 * 'planstate' is the top PlanState node of the slice.
 * 'showstatctx' is a PxExplain_ShowStatCtx object which was created by
 *		calling pxexplain_showExecStatsBegin().
 */
void
pxexplain_localExecStats(struct PlanState *planstate,
						 struct PxExplain_ShowStatCtx *showstatctx)
{
	PxExplain_LocalStatCtx ctx;

	Assert(px_role != PX_ROLE_PX);

	Insist(planstate && planstate->instrument && showstatctx);

	memset(&ctx, 0, sizeof(ctx));

	/* Set up send context area. */
	ctx.send.notebuf = &showstatctx->extratextbuf;

	/* Set up a temporary StatHdr for both collecting and depositing stats. */
	ctx.msgptrs[0] = &ctx.send.hdr;
	ctx.send.hdr.segindex = PxIdentity.workerid;
	ctx.send.hdr.nInst = 1;

	/* Set up receive context area referencing our temp StatHdr. */
	ctx.recv.nStatInst = ctx.send.hdr.nInst;
	ctx.recv.segindexMin = ctx.recv.segindexMax = ctx.send.hdr.segindex;

	ctx.recv.sliceIndex = LocallyExecutingSliceIndex(planstate->state);
	ctx.recv.msgptrs = ctx.msgptrs;
	ctx.recv.nmsgptr = 1;
	ctx.recv.dispatchResults = NULL;
	ctx.recv.extratextbuf = NULL;
	ctx.recv.showstatctx = showstatctx;

	/*
	 * Collect and redeposit statistics from each PlanState node in this
	 * slice. Any extra message text will be appended directly to
	 * extratextbuf.
	 */
	planstate_walk_node(planstate, pxexplain_localStatWalker, &ctx);

	/* Obtain per-slice stats and put them in SliceSummary. */
	pxexplain_collectSliceStats(planstate, &ctx.send.hdr.worker);
	pxexplain_depositSliceStats(&ctx.send.hdr, &ctx.recv);
}								/* pxexplain_localExecStats */

/*
 * pxexplain_localStatWalker
 */
static PxVisitOpt
pxexplain_localStatWalker(PlanState *planstate, void *context)
{
	PxExplain_LocalStatCtx *ctx = (PxExplain_LocalStatCtx *) context;

	/* Collect stats into our temporary StatInst and caller's extratextbuf. */
	pxexplain_collectStatsFromNode(planstate, &ctx->send);

	/* Redeposit stats back into Instrumentation, and attach a NodeSummary. */
	pxexplain_depositStatsToNode(planstate, &ctx->recv);

	/* Don't descend across a slice boundary. */
	if (IsA(planstate, MotionState))
		return PxVisit_Skip;

	return PxVisit_Walk;
}								/* pxexplain_localStatWalker */

/*
 * pxexplain_sendExecStats
 *	  Called by qExec process to send EXPLAIN ANALYZE statistics to qDisp.
 *	  On the qDisp, libpq will recognize our special message type ('Y') and
 *	  attach the message to the current command's PGresult object.
 */
void
pxexplain_sendExecStats(QueryDesc *queryDesc)
{
	EState	   *estate;
	PlanState  *planstate;
	PxExplain_SendStatCtx ctx;
	StringInfoData notebuf;

	/* Header offset (where header begins in the message buffer) */
	int			hoff;

	Assert(px_role == PX_ROLE_PX);

	if (!queryDesc ||
		!queryDesc->estate)
		return;

	/* If executing a root slice (UPD/DEL/INS), start at top of plan tree. */
	estate = queryDesc->estate;
	if (LocallyExecutingSliceIndex(estate) == RootSliceIndex(estate))
		planstate = queryDesc->planstate;

	/* Non-root slice: Start at child of our sending Motion node. */
	else
	{
		planstate = &(getMotionState(queryDesc->planstate, LocallyExecutingSliceIndex(estate))->ps);
		Assert(planstate &&
			   IsA(planstate, MotionState) &&
			   planstate->lefttree);
		planstate = planstate->lefttree;
	}

	if (planstate == NULL)
		return;

	/* Start building the message header in our context area. */
	memset(&ctx, 0, sizeof(ctx));
	ctx.hdr.type = T_PxExplain_StatHdr;
	ctx.hdr.segindex = PxIdentity.workerid;
	ctx.hdr.nInst = 0;

	/* Allocate a separate buffer where nodes can append extra message text. */
	initStringInfo(&notebuf);
	ctx.notebuf = &notebuf;

	/* Reserve buffer space for the message header (excluding 'inst' array). */
	pq_beginmessage(&ctx.buf, 'Y');

	/* Where the actual StatHdr begins */
	hoff = ctx.buf.len;

	/*
	 * Write everything until inst member including "PxExplain_SliceWorker
	 * worker"
	 */
	appendBinaryStringInfo(&ctx.buf, (char *) &ctx.hdr, sizeof(ctx.hdr) - sizeof(ctx.hdr.inst));

	/* Append statistics from each PlanState node in this slice. */
	planstate_walk_node(planstate, pxexplain_sendStatWalker, &ctx);

	/* Obtain per-slice stats and put them in StatHdr. */
	pxexplain_collectSliceStats(planstate, &ctx.hdr.worker);

	/* Append the extra message text. */
	ctx.hdr.bnotes = ctx.buf.len - hoff;
	appendBinaryStringInfo(&ctx.buf, notebuf.data, notebuf.len);
	ctx.hdr.enotes = ctx.buf.len - hoff;
	pfree(notebuf.data);

	/*
	 * Move the message header into the buffer. Rewrite the updated header
	 * (with bnotes, enotes, nInst etc.) Note: this is the second time we are
	 * writing the header. The first write merely reserves space for the
	 * header
	 */
	memcpy(ctx.buf.data + hoff, (char *) &ctx.hdr, sizeof(ctx.hdr) - sizeof(ctx.hdr.inst));

	/* Send message to qDisp process. */
	pq_endmessage(&ctx.buf);
}								/* pxexplain_sendExecStats */


/*
 * pxexplain_sendStatWalker
 */
static PxVisitOpt
pxexplain_sendStatWalker(PlanState *planstate, void *context)
{
	PxExplain_SendStatCtx *ctx = (PxExplain_SendStatCtx *) context;
	PxExplain_StatInst *si = &ctx->hdr.inst[0];

	/* Stuff stats into our temporary StatInst.  Add extra text to notebuf. */
	pxexplain_collectStatsFromNode(planstate, ctx);

	/* Append StatInst instance to message. */
	appendBinaryStringInfo(&ctx->buf, (char *) si, sizeof(*si));
	ctx->hdr.nInst++;

	/* Don't descend across a slice boundary. */
	if (IsA(planstate, MotionState))
		return PxVisit_Skip;

	return PxVisit_Walk;
}								/* pxexplain_sendStatWalker */

/*
 * pxexplain_recvExecStats
 *	  Called by qDisp to transfer a slice's EXPLAIN ANALYZE statistics
 *	  from the PxDispatchResults structures to the PlanState tree.
 *	  Recursively does the same for slices that are descendants of the
 *	  one specified.
 *
 * 'showstatctx' is a PxExplain_ShowStatCtx object which was created by
 *		calling pxexplain_showExecStatsBegin().
 */
void
pxexplain_recvExecStats(struct PlanState *planstate,
						struct PxDispatchResults *dispatchResults,
						int sliceIndex,
						struct PxExplain_ShowStatCtx *showstatctx)
{
	PxDispatchResult *dispatchResultBeg;
	PxDispatchResult *dispatchResultEnd;
	PxExplain_RecvStatCtx ctx;
	PxExplain_DispatchSummary ds;

	/* TODO */
	/* int			gpsegmentCount = getgpsegmentCount(); */
	int			iDispatch;
	int			nDispatch;
	int			imsgptr;

	if (!planstate ||
		!planstate->instrument ||
		!showstatctx)
		return;

	/*
	 * Note that the caller may free the PxDispatchResults upon return, maybe
	 * before EXPLAIN ANALYZE examines the PlanState tree.  Consequently we
	 * must not return ptrs into the dispatch result buffers, but must copy
	 * any needed information into a sufficiently long-lived memory context.
	 */

	/* Initialize treewalk context. */
	memset(&ctx, 0, sizeof(ctx));
	ctx.dispatchResults = dispatchResults;
	ctx.extratextbuf = &showstatctx->extratextbuf;
	ctx.showstatctx = showstatctx;
	ctx.sliceIndex = sliceIndex;

	/* Find the slice's PxDispatchResult objects. */
	dispatchResultBeg = pxdisp_resultBegin(dispatchResults, sliceIndex);
	dispatchResultEnd = pxdisp_resultEnd(dispatchResults, sliceIndex);
	nDispatch = dispatchResultEnd - dispatchResultBeg;

	/* Initialize worker counts. */
	memset(&ds, 0, sizeof(ds));
	ds.nResult = nDispatch;

	/* Find and validate the statistics returned from each qExec. */
	if (nDispatch > 0)
		ctx.msgptrs = (PxExplain_StatHdr **) palloc0(nDispatch * sizeof(ctx.msgptrs[0]));
	for (iDispatch = 0; iDispatch < nDispatch; iDispatch++)
	{
		PxDispatchResult *dispatchResult = &dispatchResultBeg[iDispatch];
		PGresult   *pgresult;
		PxExplain_StatHdr *hdr;
		pgPxStatCell *statcell;

		/* Update worker counts. */
		if (!dispatchResult->hasDispatched)
			ds.nNotDispatched++;
		else if (dispatchResult->wasCanceled)
			ds.nCanceled++;
		else if (dispatchResult->errcode)
			ds.nError++;
		else if (dispatchResult->okindex >= 0)
			ds.nOk++;			/* qExec returned successful completion */
		else
			ds.nIgnorableError++;	/* qExec returned an error that's likely a
									 * side-effect of another qExec's failure,
									 * e.g. an interconnect error */

		/* Find this qExec's last PGresult.  If none, skip to next qExec. */
		pgresult = pxdisp_getPGresult(dispatchResult, -1);
		if (!pgresult)
			continue;

		/* Find our statistics in list of response messages.  If none, skip. */
		for (statcell = pgresult->pxstats; statcell; statcell = statcell->next)
		{
			if (IsA((Node *) statcell->data, PxExplain_StatHdr))
				break;
		}
		if (!statcell)
			continue;

		/* Validate the message header. */
		hdr = (PxExplain_StatHdr *) statcell->data;

		/* Slice should have same number of plan nodes on every qExec. */
		if (iDispatch == 0)
			ctx.nStatInst = hdr->nInst;
		else
		{
			/* MPP-2140: what causes this ? */
			if (ctx.nStatInst != hdr->nInst)
				ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
								errmsg("Invalid execution statistics "
									   "received stats node-count mismatch: pxexplain_recvExecStats() ctx.nStatInst %d hdr->nInst %d", ctx.nStatInst, hdr->nInst),
								errhint("Please verify that all instances are using "
										"the correct %s software version.",
										PACKAGE_NAME)));

			Insist(ctx.nStatInst == hdr->nInst);
		}

		/* Save lowest and highest segment id for which we have stats. */
		if (iDispatch == 0)
			ctx.segindexMin = ctx.segindexMax = hdr->segindex;
		else if (ctx.segindexMax < hdr->segindex)
			ctx.segindexMax = hdr->segindex;
		else if (ctx.segindexMin > hdr->segindex)
			ctx.segindexMin = hdr->segindex;

		/* Save message ptr for easy reference. */
		ctx.msgptrs[ctx.nmsgptr] = hdr;
		ctx.nmsgptr++;
	}

	/* Attach NodeSummary to each PlanState node's Instrumentation node. */
	planstate_walk_node(planstate, pxexplain_recvStatWalker, &ctx);

	/* Make sure we visited the right number of PlanState nodes. */
	Insist(ctx.iStatInst == ctx.nStatInst);

	/* Transfer per-slice stats from message headers to the SliceSummary. */
	for (imsgptr = 0; imsgptr < ctx.nmsgptr; imsgptr++)
		pxexplain_depositSliceStats(ctx.msgptrs[imsgptr], &ctx);

	/* Transfer worker counts to SliceSummary. */
	showstatctx->slices[sliceIndex].dispatchSummary = ds;

	/*
	 * Signal that we've gathered all the statistics For some query, which has
	 * initplan on top of the plan, its `ANALYZE EXPLAIN` invoke
	 * `pxexplain_recvExecStats` multi-times in different recursive routine to
	 * collect metrics on both initplan and plan. Thus, this variable should
	 * only assign on slice 0 after gather result done to promise all slices
	 * information have been collected.
	 */
	if (sliceIndex == 0)
		showstatctx->stats_gathered = true;

	/* Clean up. */
	if (ctx.msgptrs)
		pfree(ctx.msgptrs);
}								/* pxexplain_recvExecStats */


/*
 * pxexplain_recvStatWalker
 *	  Update the given PlanState node's Instrument node with statistics
 *	  received from qExecs.  Attach a PxExplain_NodeSummary block to
 *	  the Instrument node.  At a MotionState node, descend to child slice.
 */
static PxVisitOpt
pxexplain_recvStatWalker(PlanState *planstate, void *context)
{
	PxExplain_RecvStatCtx *ctx = (PxExplain_RecvStatCtx *) context;

	/* If slice was dispatched to qExecs, and stats came back, grab 'em. */
	if (ctx->nmsgptr > 0)
	{
		/* Transfer received stats to Instrumentation, NodeSummary, etc. */
		pxexplain_depositStatsToNode(planstate, ctx);

		/* Advance to next node's entry in all of the StatInst arrays. */
		ctx->iStatInst++;
	}

	/* Motion operator?  Descend to next slice. */
	if (IsA(planstate, MotionState))
	{
		pxexplain_recvExecStats(planstate->lefttree,
								ctx->dispatchResults,
								((Motion *) planstate->plan)->motionID,
								ctx->showstatctx);
		return PxVisit_Skip;
	}

	return PxVisit_Walk;
}								/* pxexplain_recvStatWalker */


/*
 * pxexplain_collectSliceStats
 *	  Obtain per-slice statistical observations from the current slice
 *	  (which has just completed execution in the current process) and
 *	  store the information in the given SliceWorker struct.
 *
 * 'planstate' is the top PlanState node of the current slice.
 */
static void
pxexplain_collectSliceStats(PlanState *planstate,
							PxExplain_SliceWorker *out_worker)
{
	EState	   *estate = planstate->state;

	/* Max bytes malloc'ed under executor's per-query memory context. */
	out_worker->peakmemused = (double) MemoryContextGetPeakSpace(estate->es_query_cxt);

#if 0
	out_worker->vmem_reserved = (double) VmemTracker_GetMaxReservedVmemBytes();
#endif
}

/*
 * pxexplain_depositStatsToNode
 *
 * Called by recvStatWalker and localStatWalker to update the given
 * PlanState node's Instrument node with statistics received from
 * workers or collected locally.  Attaches a PxExplain_NodeSummary
 * block to the Instrument node.  If top node of slice, per-slice
 * statistics are transferred from the StatHdr to the SliceSummary.
 */
static void
pxexplain_depositStatsToNode(PlanState *planstate, PxExplain_RecvStatCtx *ctx)
{
	Instrumentation *instr = planstate->instrument;
	PxExplain_StatHdr *rsh;		/* The header (which includes StatInst) */
	PxExplain_StatInst *rsi;	/* The current StatInst */

	/*
	 * Points to the insts array of node summary (PxExplain_NodeSummary). Used
	 * for saving every rsi in the node summary (in addition to saving the
	 * max/avg).
	 */
	PxExplain_StatInst *nsi;

	/*
	 * ns is the node summary across all QEs of the segworker group. It also
	 * contains detailed "unsummarized" raw stat for a node across all QEs in
	 * current segworker group (in the insts array)
	 */
	PxExplain_NodeSummary *ns;
	PxExplain_DepStatAcc ntuples;
	PxExplain_DepStatAcc execmemused;
	PxExplain_DepStatAcc workmemused;
	PxExplain_DepStatAcc workmemwanted;
	PxExplain_DepStatAcc totalWorkfileCreated;
	PxExplain_DepStatAcc peakmemused;
	PxExplain_DepStatAcc vmem_reserved;
	PxExplain_DepStatAcc memory_accounting_global_peak;
	PxExplain_DepStatAcc peakMemBalance;
	PxExplain_DepStatAcc totalPartTableScanned;
	PxExplain_DepStatAcc sortSpaceUsed[NUM_SORT_SPACE_TYPE][NUM_SORT_METHOD];
	int			imsgptr;
	int			nInst;
	int			i;
	int			j;

	Insist(instr &&
		   ctx->iStatInst < ctx->nStatInst);

	/* Allocate NodeSummary block. */
	nInst = ctx->segindexMax + 1 - ctx->segindexMin;
	ns = (PxExplain_NodeSummary *) palloc0(sizeof(*ns) - sizeof(ns->insts) +
										   nInst * sizeof(ns->insts[0]));
	ns->segindex0 = ctx->segindexMin;
	ns->ninst = nInst;

	/* Attach our new NodeSummary to the Instrumentation node. */
	instr->pxNodeSummary = ns;

	/* Initialize per-node accumulators. */
	pxexplain_depStatAcc_init0(&ntuples);
	pxexplain_depStatAcc_init0(&execmemused);
	pxexplain_depStatAcc_init0(&workmemused);
	pxexplain_depStatAcc_init0(&workmemwanted);
	pxexplain_depStatAcc_init0(&totalWorkfileCreated);
	pxexplain_depStatAcc_init0(&peakMemBalance);
	pxexplain_depStatAcc_init0(&totalPartTableScanned);
	for (i = 0; i < NUM_SORT_METHOD; i++)
	{
		for (j = 0; j < NUM_SORT_SPACE_TYPE; j++)
		{
			pxexplain_depStatAcc_init0(&sortSpaceUsed[j][i]);
		}
	}

	/* Initialize per-slice accumulators. */
	pxexplain_depStatAcc_init0(&peakmemused);
	pxexplain_depStatAcc_init0(&vmem_reserved);
	pxexplain_depStatAcc_init0(&memory_accounting_global_peak);

	/* Examine the statistics from each qExec. */
	for (imsgptr = 0; imsgptr < ctx->nmsgptr; imsgptr++)
	{
		/* Locate PlanState node's StatInst received from this qExec. */
		rsh = ctx->msgptrs[imsgptr];
		rsi = &rsh->inst[ctx->iStatInst];

		Insist(rsi->pstype == planstate->type &&
			   ns->segindex0 <= rsh->segindex &&
			   rsh->segindex < ns->segindex0 + ns->ninst);

		/* Locate this qExec's StatInst slot in node's NodeSummary block. */
		nsi = &ns->insts[rsh->segindex - ns->segindex0];

		/* Copy the StatInst to NodeSummary from dispatch result buffer. */
		*nsi = *rsi;

		/*
		 * Drop qExec's extra text.  We rescue it below if qExec is a winner.
		 * For local qDisp slice, ctx->extratextbuf is NULL, which tells us to
		 * leave the extra text undisturbed in its existing buffer.
		 */
		if (ctx->extratextbuf)
			nsi->bnotes = nsi->enotes = 0;

		/* Update per-node accumulators. */
		pxexplain_depStatAcc_upd(&ntuples, rsi->ntuples, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&execmemused, rsi->execmemused, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&workmemused, rsi->workmemused, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&workmemwanted, rsi->workmemwanted, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&totalWorkfileCreated, (rsi->workfileCreated ? 1 : 0), rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&peakMemBalance, rsi->peakMemBalance, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&totalPartTableScanned, rsi->numPartScanned, rsh, rsi, nsi);

		if (rsi->sortstats.sortMethod != SORT_TYPE_STILL_IN_PROGRESS)
		{
			pxexplain_depStatAcc_upd(&sortSpaceUsed[rsi->sortstats.spaceType][rsi->sortstats.sortMethod],
									 (double) rsi->sortstats.spaceUsed, rsh, rsi, nsi);
		}
		/* FIXME: different from orca, we only pxexplain_depStatAcc_upd once! */

		/* Update per-slice accumulators. */
		pxexplain_depStatAcc_upd(&peakmemused, rsh->worker.peakmemused, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&vmem_reserved, rsh->worker.vmem_reserved, rsh, rsi, nsi);
		pxexplain_depStatAcc_upd(&memory_accounting_global_peak, rsh->worker.memory_accounting_global_peak, rsh, rsi, nsi);
	}

	/* Save per-node accumulated stats in NodeSummary. */
	ns->ntuples = ntuples.agg;
	ns->execmemused = execmemused.agg;
	ns->workmemused = workmemused.agg;
	ns->workmemwanted = workmemwanted.agg;
	ns->totalWorkfileCreated = totalWorkfileCreated.agg;
	ns->peakMemBalance = peakMemBalance.agg;
	ns->totalPartTableScanned = totalPartTableScanned.agg;
	for (i = 0; i < NUM_SORT_METHOD; i++)
	{
		for (int j = 0; j < NUM_SORT_SPACE_TYPE; j++)
		{
			ns->sortSpaceUsed[j][i] = sortSpaceUsed[j][i].agg;
		}
	}

	/* Roll up summary over all nodes of slice into RecvStatCtx. */
	ctx->workmemused_max = Max(ctx->workmemused_max, workmemused.agg.vmax);
	ctx->workmemwanted_max = Max(ctx->workmemwanted_max, workmemwanted.agg.vmax);

	instr->total = ntuples.max_total;
	INSTR_TIME_ASSIGN(instr->firststart, ntuples.firststart_of_max_total);

	/* Put winner's stats into qDisp PlanState's Instrument node. */
	if (ntuples.agg.vcnt > 0)
	{
		instr->starttime = ntuples.nsimax->starttime;
		instr->counter = ntuples.nsimax->counter;
		instr->firsttuple = ntuples.nsimax->firsttuple;
		instr->startup = ntuples.nsimax->startup;
		instr->total = ntuples.nsimax->total;
		instr->ntuples = ntuples.nsimax->ntuples;
		instr->nloops = ntuples.nsimax->nloops;
		instr->execmemused = ntuples.nsimax->execmemused;
		instr->workmemused = ntuples.nsimax->workmemused;
		instr->workmemwanted = ntuples.nsimax->workmemwanted;
		instr->workfileCreated = ntuples.nsimax->workfileCreated;
		instr->firststart = ntuples.nsimax->firststart;
	}

	/* Save extra message text for the most interesting winning qExecs. */
	if (ctx->extratextbuf)
	{
		bool		saved = false;

		/* One worker which used or wanted the most work_mem */
		if (workmemwanted.agg.vmax >= workmemused.agg.vmax)
			pxexplain_depStatAcc_saveText(&workmemwanted, ctx->extratextbuf, &saved);
		else if (workmemused.agg.vmax > 1.05 * pxexplain_agg_avg(&workmemused.agg))
			pxexplain_depStatAcc_saveText(&workmemused, ctx->extratextbuf, &saved);

		/* Worker which used the most executor memory (this node's usage) */
		if (execmemused.agg.vmax > 1.05 * pxexplain_agg_avg(&execmemused.agg))
			pxexplain_depStatAcc_saveText(&execmemused, ctx->extratextbuf, &saved);

		/*
		 * For the worker which had the highest peak executor memory usage
		 * overall across the whole slice, we'll report the extra message text
		 * from all of the nodes in the slice.  But only if that worker stands
		 * out more than 5% above the average.
		 */
		if (peakmemused.agg.vmax > 1.05 * pxexplain_agg_avg(&peakmemused.agg))
			pxexplain_depStatAcc_saveText(&peakmemused, ctx->extratextbuf, &saved);

		/*
		 * One worker which produced the greatest number of output rows.
		 * (Always give at least one node a chance to have its extra message
		 * text seen.  In case no node stood out above the others, make a
		 * repeatable choice based on the number of output rows.)
		 */
		if (!saved ||
			ntuples.agg.vmax > 1.05 * pxexplain_agg_avg(&ntuples.agg))
			pxexplain_depStatAcc_saveText(&ntuples, ctx->extratextbuf, &saved);
	}

	/*
	 * If this is a HashState, construct a SharedHashInfo with the stats from
	 * all the QEs. In PostgreSQL, SharedHashInfo is used to show stats of all
	 * the worker processes, we use it to show stats from all the QEs instead.
	 *
	 * GPDB_12_MERGE_FIXME: Should we do the same for Sort stats nowadays?
	 */
	if (IsA(planstate, HashState))
	{
		/* GPDB: Collect the results from all QE processes */
		HashState  *hashstate = (HashState *) planstate;
		SharedHashInfo *shared_state;

		size_t		size;

		size = offsetof(SharedHashInfo, hinstrument) +
			ctx->nmsgptr * sizeof(HashInstrumentation);
		shared_state = palloc0(size);
		shared_state->num_workers = ctx->nmsgptr;

		/* Examine the statistics from each qExec. */
		for (imsgptr = 0; imsgptr < ctx->nmsgptr; imsgptr++)
		{
			/* Locate PlanState node's StatInst received from this qExec. */
			rsh = ctx->msgptrs[imsgptr];
			rsi = &rsh->inst[ctx->iStatInst];

			memcpy(&shared_state->hinstrument[imsgptr], &rsi->hashstats, sizeof(HashInstrumentation));
		}

		hashstate->shared_info = shared_state;
	}
}								/* pxexplain_depositStatsToNode */


/*
 * pxexplain_depositSliceStats
 *	  Transfer a worker's per-slice stats contribution from StatHdr into the
 *	  SliceSummary array in the ShowStatCtx.  Transfer the rollup of per-node
 *	  stats from the RecvStatCtx into the SliceSummary.
 *
 * Kludge: In a non-parallel plan, slice numbers haven't been assigned, so we
 * may be called more than once with sliceIndex == 0: once for the outermost
 * query and once for each InitPlan subquery.  In this case we dynamically
 * expand the SliceSummary array.  POLAR PX TODO: Always assign proper root slice
 * ids (in qDispSliceId field of SubPlan node); then remove this kludge.
 */
static void
pxexplain_depositSliceStats(PxExplain_StatHdr *hdr,
							PxExplain_RecvStatCtx *recvstatctx)
{
	int			sliceIndex = recvstatctx->sliceIndex;
	PxExplain_ShowStatCtx *showstatctx = recvstatctx->showstatctx;
	PxExplain_SliceSummary *ss = &showstatctx->slices[sliceIndex];
	PxExplain_SliceWorker *ssw;
	int			iworker;

#if 0
	const char *originalSerializedMemoryAccountingStartAddress = ((const char *) hdr) + hdr->memAccountStartOffset;
#endif

	Insist(sliceIndex >= 0 &&
		   sliceIndex < showstatctx->nslice);

	/* Kludge:	QD can have more than one 'Slice 0' if plan is non-parallel. */
	if (sliceIndex == 0 &&
		recvstatctx->dispatchResults == NULL &&
		ss->workers)
	{
		Assert(ss->nworker == 1 &&
			   recvstatctx->segindexMin == hdr->segindex &&
			   recvstatctx->segindexMax == hdr->segindex);

		/* Expand the SliceSummary array to make room for InitPlan subquery. */
		sliceIndex = showstatctx->nslice++;
		showstatctx->slices = (PxExplain_SliceSummary *)
			repalloc(showstatctx->slices, showstatctx->nslice * sizeof(showstatctx->slices[0]));
		ss = &showstatctx->slices[sliceIndex];
		memset(ss, 0, sizeof(*ss));
	}

	/* Slice's first worker? */
	if (!ss->workers)
	{
		/* Allocate SliceWorker array and attach it to the SliceSummary. */
		ss->segindex0 = recvstatctx->segindexMin;
		ss->nworker = recvstatctx->segindexMax + 1 - ss->segindex0;
		ss->workers = (PxExplain_SliceWorker *) palloc0(ss->nworker * sizeof(ss->workers[0]));
		ss->memoryAccounts = (void **) palloc0(ss->nworker * sizeof(ss->memoryAccounts[0]));
	}

	/* Save a copy of this SliceWorker instance in the worker array. */
	iworker = hdr->segindex - ss->segindex0;
	ssw = &ss->workers[iworker];
	Insist(iworker >= 0 && iworker < ss->nworker);
	Insist(ssw->peakmemused == 0);	/* each worker should be seen just once */
	*ssw = hdr->worker;

	/* Rollup of per-worker stats into SliceSummary */
	pxexplain_agg_upd(&ss->peakmemused, hdr->worker.peakmemused, hdr->segindex);
	pxexplain_agg_upd(&ss->vmem_reserved, hdr->worker.vmem_reserved, hdr->segindex);
	pxexplain_agg_upd(&ss->memory_accounting_global_peak, hdr->worker.memory_accounting_global_peak, hdr->segindex);

	/* Rollup of per-node stats over all nodes of the slice into SliceSummary */
	ss->workmemused_max = recvstatctx->workmemused_max;
	ss->workmemwanted_max = recvstatctx->workmemwanted_max;

	/* Rollup of per-node stats over the whole query into ShowStatCtx. */
	showstatctx->workmemused_max = Max(showstatctx->workmemused_max, recvstatctx->workmemused_max);
	showstatctx->workmemwanted_max = Max(showstatctx->workmemwanted_max, recvstatctx->workmemwanted_max);
}								/* pxexplain_depositSliceStats */

/*
 * pxexplain_collectStatsFromNode
 *
 * Called by sendStatWalker and localStatWalker to obtain a node's statistics
 * and transfer them into the temporary StatHdr and StatInst in the SendStatCtx.
 * Also obtains the node's extra message text, which it appends to the caller's
 * cxt->nodebuf.
 */
static void
pxexplain_collectStatsFromNode(PlanState *planstate, PxExplain_SendStatCtx *ctx)
{
	PxExplain_StatInst *si = &ctx->hdr.inst[0];
	Instrumentation *instr = planstate->instrument;

	Insist(instr);

	/* We have to finalize statistics, since ExecutorEnd hasn't been called. */
	InstrEndLoop(instr);

	/* Initialize the StatInst slot in the temporary StatHdr. */
	memset(si, 0, sizeof(*si));
	si->pstype = planstate->type;

	/* Add this node's extra message text to notebuf.  Store final stats. */
	si->bnotes = pxexplain_collectExtraText(planstate, ctx->notebuf);
	si->enotes = ctx->notebuf->len;

	/* Make sure there is a '\0' between this node's message and the next. */
	if (si->bnotes < si->enotes)
		appendStringInfoChar(ctx->notebuf, '\0');

	if (planstate->node_context)
		si->execmemused = (double) MemoryContextGetPeakSpace(planstate->node_context);

	/* Transfer this node's statistics from Instrumentation into StatInst. */
	si->starttime = instr->starttime;
	si->counter = instr->counter;
	si->firsttuple = instr->firsttuple;
	si->startup = instr->startup;
	si->total = instr->total;
	si->ntuples = instr->ntuples;
	si->nloops = instr->nloops;
	si->workmemused = instr->workmemused;
	si->workmemwanted = instr->workmemwanted;
	si->workfileCreated = instr->workfileCreated;
	si->firststart = instr->firststart;
	si->numPartScanned = instr->numPartScanned;

	if (IsA(planstate, SortState))
	{
		SortState  *sortstate = (SortState *) planstate;

		si->sortstats = sortstate->sortstats;
	}
	if (IsA(planstate, HashState))
	{
		HashState  *hashstate = (HashState *) planstate;

		if (hashstate->hashtable)
			ExecHashAccumInstrumentation(&si->hashstats, hashstate->hashtable);
	}
}								/* pxexplain_collectStatsFromNode */

/*
 * pxexplain_collectExtraText
 *	  Allow a node to supply additional text for its EXPLAIN ANALYZE report.
 *
 * Returns the starting offset of the extra message text from notebuf->data.
 * The caller can compute the length as notebuf->len minus the starting offset.
 * If the node did not provide any extra message text, the length will be 0.
 */
static int
pxexplain_collectExtraText(PlanState *planstate, StringInfo notebuf)
{
	int			bnotes = notebuf->len;

	/*
	 * Invoke node's callback.  It may append to our notebuf and/or its own
	 * pxexplainbuf; and store final statistics in its Instrumentation node.
	 */
	if (planstate->pxexplainfun)
		planstate->pxexplainfun(planstate, notebuf);

	/*
	 * Append contents of node's extra message buffer.  This allows nodes to
	 * contribute EXPLAIN ANALYZE info without having to set up a callback.
	 */
	if (planstate->pxexplainbuf && planstate->pxexplainbuf->len > 0)
	{
		/* If callback added to notebuf, make sure text ends with a newline. */
		if (bnotes < notebuf->len &&
			notebuf->data[notebuf->len - 1] != '\n')
			appendStringInfoChar(notebuf, '\n');

		appendBinaryStringInfo(notebuf, planstate->pxexplainbuf->data,
							   planstate->pxexplainbuf->len);

		resetStringInfo(planstate->pxexplainbuf);
	}

	return bnotes;
}

/*
 * pxexplain_formatExtraText
 *	  Format extra message text into the EXPLAIN output buffer.
 */
static void
pxexplain_formatExtraText(StringInfo str,
						  int indent,
						  int segindex,
						  const char *notes,
						  int notelen)
{
	const char *cp = notes;
	const char *ep = notes + notelen;

	/* Could be more than one line... */
	while (cp < ep)
	{
		const char *nlp = memchr(cp, '\n', ep - cp);
		const char *dp = nlp ? nlp : ep;

		/* Strip trailing whitespace. */
		while (cp < dp &&
			   isspace(dp[-1]))
			dp--;

		/* Add to output buffer. */
		if (cp < dp)
		{
			appendStringInfoSpaces(str, indent * 2);
			if (segindex >= 0)
			{
				appendStringInfo(str, "(seg%d) ", segindex);
				if (segindex < 10)
					appendStringInfoChar(str, ' ');
				if (segindex < 100)
					appendStringInfoChar(str, ' ');
			}
			appendBinaryStringInfo(str, cp, dp - cp);
			if (nlp)
				appendStringInfoChar(str, '\n');
		}

		if (!nlp)
			break;
		cp = nlp + 1;
	}
}								/* pxexplain_formatExtraText */



/*
 * pxexplain_formatMemory
 *	  Convert memory size to string from (double) bytes.
 *
 *		outbuf:  [output] pointer to a char buffer to be filled
 *		bufsize: [input] maximum number of characters to write to outbuf (must be set by the caller)
 *		bytes:	 [input] a value representing memory size in bytes to be written to outbuf
 */
static void
pxexplain_formatMemory(char *outbuf, int bufsize, double bytes)
{
#ifdef USE_ASSERT_CHECKING
	int			nchars_written;
#endif

	Assert(outbuf != NULL && "PXEXPLAIN: char buffer is null");
	Assert(bufsize > 0 && "PXEXPLAIN: size of char buffer is zero");
	/* check if truncation occurs */
#ifdef USE_ASSERT_CHECKING
	nchars_written =
#endif
		snprintf(outbuf, bufsize, "%.0fK bytes", kb(bytes));
	Assert(nchars_written < bufsize &&
		   "PXEXPLAIN:  size of char buffer is smaller than the required number of chars");
}								/* pxexplain_formatMemory */



/*
 * pxexplain_formatSeconds
 *	  Convert time in seconds to readable string
 *
 *		outbuf:  [output] pointer to a char buffer to be filled
 *		bufsize: [input] maximum number of characters to write to outbuf (must be set by the caller)
 *		seconds: [input] a value representing no. of seconds to be written to outbuf
 */
static void
pxexplain_formatSeconds(char *outbuf, int bufsize, double seconds, bool unit)
{
#ifdef USE_ASSERT_CHECKING
	int			nchars_written;
#endif
	double		ms = seconds * 1000.0;

	Assert(outbuf != NULL && "PXEXPLAIN: char buffer is null");
	Assert(bufsize > 0 && "PXEXPLAIN: size of char buffer is zero");

#ifdef USE_ASSERT_CHECKING
	nchars_written =
#endif
		snprintf(outbuf, bufsize, "%.*f%s",
				 (ms < 10.0 && ms != 0.0 && ms > -10.0) ? 3 : 0,
				 ms, (unit ? "(ms)" : ""));

	Assert(nchars_written < bufsize &&
		   "PXEXPLAIN:  size of char buffer is smaller than the required number of chars");
}								/* pxexplain_formatSeconds */


/*
 * pxexplain_formatSeg
 *	  Convert segment id to string.
 *
 *		outbuf:  [output] pointer to a char buffer to be filled
 *		bufsize: [input] maximum number of characters to write to outbuf (must be set by the caller)
 *		segindex:[input] a value representing segment index to be written to outbuf
 *		nInst:	 [input] no. of stat instances
 */
static void
pxexplain_formatSeg(char *outbuf, int bufsize, int segindex, int nInst)
{
	Assert(outbuf != NULL && "PXEXPLAIN: char buffer is null");
	Assert(bufsize > 0 && "PXEXPLAIN: size of char buffer is zero");

	if (nInst > 1 && segindex >= 0)
	{
		/* check if truncation occurs */
#ifdef USE_ASSERT_CHECKING
		int			nchars_written =
#endif							/* USE_ASSERT_CHECKING */
			snprintf(outbuf, bufsize, " (seg%d)", segindex);

		Assert(nchars_written < bufsize &&
			   "PXEXPLAIN:  size of char buffer is smaller than the required number of chars");
	}
	else
	{
		outbuf[0] = '\0';
	}
}								/* pxexplain_formatSeg */


/*
 * pxexplain_showExecStatsBegin
 *	  Called by qDisp process to create a PxExplain_ShowStatCtx structure
 *	  in which to accumulate overall statistics for a query.
 *
 * 'querystarttime' is the timestamp of the start of the query, in a
 *		platform-dependent format.
 *
 * Note this function is called before ExecutorStart(), so there is no EState
 * or SliceTable yet.
 */
struct PxExplain_ShowStatCtx *
pxexplain_showExecStatsBegin(struct QueryDesc *queryDesc,
							 instr_time querystarttime)
{
	PxExplain_ShowStatCtx *ctx;
	int			nslice;

	Assert(px_role != PX_ROLE_PX);

	/* Allocate and zero the ShowStatCtx */
	ctx = (PxExplain_ShowStatCtx *) palloc0(sizeof(*ctx));

	ctx->querystarttime = querystarttime;

	/* Determine number of slices.  (SliceTable hasn't been built yet.) */
	nslice = queryDesc->plannedstmt->numSlices;

	/* Allocate and zero the SliceSummary array. */
	ctx->nslice = nslice;
	ctx->slices = (PxExplain_SliceSummary *) palloc0(nslice * sizeof(ctx->slices[0]));

	/* Allocate a buffer in which we can collect any extra message text. */
	initStringInfoOfSize(&ctx->extratextbuf, 4000);

	return ctx;
}								/* pxexplain_showExecStatsBegin */

/*
 * nodeSupportWorkfileCaching
 *	 Return true if a given node supports workfile caching.
 */
static bool
nodeSupportWorkfileCaching(PlanState *planstate)
{
	return (IsA(planstate, SortState) ||
			IsA(planstate, HashJoinState) ||
			(IsA(planstate, AggState) && ((Agg *) planstate->plan)->aggstrategy == AGG_HASHED) ||
			IsA(planstate, MaterialState));
}

/*
 * pxexplain_showExecStats
 *	  Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 *
 * 'planstate' is the node whose statistics are to be displayed.
 * 'str' is the output buffer.
 * 'indent' is the root indentation for all the text generated for explain output
 * 'ctx' is a PxExplain_ShowStatCtx object which was created by a call to
 *		pxexplain_showExecStatsBegin().
 */
static void
pxexplain_showExecStats(struct PlanState *planstate, ExplainState *es)
{
	struct PxExplain_ShowStatCtx *ctx = es->showstatctx;
	Instrumentation *instr = planstate->instrument;
	PxExplain_NodeSummary *ns = instr->pxNodeSummary;
	instr_time	timediff;
	int			i;

	char		totalbuf[50];
	char		avgbuf[50];
	char		maxbuf[50];
	char		segbuf[50];
	char		startbuf[50];
	bool		haveExtraText = false;
	StringInfoData extraData;

	/* Might not have received stats from qExecs if they hit errors. */
	if (!ns)
		return;

	Assert(instr != NULL);

	/*
	 * Executor memory used by this individual node, if it allocates from a
	 * memory context of its own instead of sharing the per-query context.
	 */
	if (es->analyze && ns->execmemused.vcnt > 0)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "Executor Memory: %ldkB  Workers: %d  Max: %ldkB (worker %d)\n",
							 (long) kb(ns->execmemused.vsum),
							 ns->execmemused.vcnt,
							 (long) kb(ns->execmemused.vmax),
							 ns->execmemused.imax);
		}
		else
		{
			ExplainPropertyInteger("Executor Memory", "kB", kb(ns->execmemused.vsum), es);
			ExplainPropertyInteger("Executor Memory Workers", NULL, ns->execmemused.vcnt, es);
			ExplainPropertyInteger("Executor Max Memory", "kB", kb(ns->execmemused.vmax), es);
			ExplainPropertyInteger("Executor Max Memory Worker", NULL, ns->execmemused.imax, es);
		}
	}

	/*
	 * Actual work_mem used and wanted
	 */
	if (es->analyze && es->verbose && ns->workmemused.vcnt > 0)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "work_mem: %ldkB  Workers: %d  Max: %ldkB (worker %d)",
							 (long) kb(ns->workmemused.vsum),
							 ns->workmemused.vcnt,
							 (long) kb(ns->workmemused.vmax),
							 ns->workmemused.imax);

			/*
			 * Total number of segments in which this node reuses cached or
			 * creates workfiles.
			 */
			if (nodeSupportWorkfileCaching(planstate))
				appendStringInfo(es->str, "  Workfile: (%d spilling)",
								 ns->totalWorkfileCreated.vcnt);

			appendStringInfo(es->str, "\n");

			if (ns->workmemwanted.vcnt > 0)
			{
				appendStringInfoSpaces(es->str, es->indent * 2);
				pxexplain_formatMemory(maxbuf, sizeof(maxbuf), ns->workmemwanted.vmax);
				if (ns->ninst == 1)
				{
					appendStringInfo(es->str,
									 "Work_mem wanted: %s to lessen workfile I/O.",
									 maxbuf);
				}
				else
				{
					pxexplain_formatMemory(avgbuf, sizeof(avgbuf), pxexplain_agg_avg(&ns->workmemwanted));
					pxexplain_formatSeg(segbuf, sizeof(segbuf), ns->workmemwanted.imax, ns->ninst);
					appendStringInfo(es->str,
									 "Work_mem wanted: %s avg, %s max%s"
									 " to lessen workfile I/O affecting %d workers.",
									 avgbuf, maxbuf, segbuf, ns->workmemwanted.vcnt);
				}

				appendStringInfo(es->str, "\n");
			}
		}
		else
		{
			ExplainOpenGroup("work_mem", "work_mem", true, es);
			ExplainPropertyInteger("Used", "kB", kb(ns->workmemused.vsum), es);
			ExplainPropertyInteger("Workers", NULL, ns->workmemused.vcnt, es);
			ExplainPropertyInteger("Max Memory", "kB", kb(ns->workmemused.vmax), es);
			ExplainPropertyInteger("Max Memory Workers", NULL, ns->workmemused.imax, es);

			/*
			 * Total number of segments in which this node reuses cached or
			 * creates workfiles.
			 */
			if (nodeSupportWorkfileCaching(planstate))
				ExplainPropertyInteger("Workfile Spilling", NULL, ns->totalWorkfileCreated.vcnt, es);

			if (ns->workmemwanted.vcnt > 0)
			{
				ExplainPropertyInteger("Max Memory Wanted", "kB", kb(ns->workmemwanted.vmax), es);

				if (ns->ninst > 1)
				{
					ExplainPropertyInteger("Max Memory Wanted Segment", NULL, ns->workmemwanted.imax, es);
					ExplainPropertyInteger("Avg Memory Wanted", "kB", kb(pxexplain_agg_avg(&ns->workmemwanted)), es);
					ExplainPropertyInteger("Workers Affected", NULL, ns->ninst, es);
				}
			}

			ExplainCloseGroup("work_mem", "work_mem", true, es);
		}
	}

	initStringInfo(&extraData);

	for (i = 0; i < ns->ninst; i++)
	{
		PxExplain_StatInst *nsi = &ns->insts[i];

		if (nsi->bnotes < nsi->enotes)
		{
			if (!haveExtraText)
			{
				ExplainOpenGroup("Extra Text", "Extra Text", false, es);
				ExplainOpenGroup("Segment", NULL, true, es);
				haveExtraText = true;
			}

			resetStringInfo(&extraData);

			pxexplain_formatExtraText(&extraData,
									  0,
									  (ns->ninst == 1) ? -1
									  : ns->segindex0 + i,
									  ctx->extratextbuf.data + nsi->bnotes,
									  nsi->enotes - nsi->bnotes);
			ExplainPropertyText("Extra Text", extraData.data, es);
		}
	}

	if (haveExtraText)
	{
		ExplainCloseGroup("Segment", NULL, true, es);
		ExplainCloseGroup("Extra Text", "Extra Text", false, es);
	}
	pfree(extraData.data);

	/*
	 * Dump stats for all workers.
	 */
	if (px_enable_explain_all_stat && ns->segindex0 >= 0 && ns->ninst > 0)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			/*
			 * create a header for all stats: separate each individual stat by
			 * an underscore, separate the grouped stats for each node by a
			 * slash
			 */
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfoString(es->str,
								   "allstat:");
		}
		else
			ExplainOpenGroup("Allstat", "Allstat", true, es);
		appendStringInfo(es->str, "\n");
		for (i = 0; i < ns->ninst; i++)
		{
			PxExplain_StatInst *nsi = &ns->insts[i];

			if (INSTR_TIME_IS_ZERO(nsi->firststart))
				continue;

			/* Time from start of query on qDisp to worker's first result row */
			INSTR_TIME_SET_ZERO(timediff);
			INSTR_TIME_ACCUM_DIFF(timediff, nsi->firststart, ctx->querystarttime);

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				pxexplain_formatSeconds(startbuf, sizeof(startbuf),
										INSTR_TIME_GET_DOUBLE(timediff), true);
				pxexplain_formatSeconds(totalbuf, sizeof(totalbuf),
										nsi->total, true);
				appendStringInfoSpaces(es->str, es->indent * 3);
				appendStringInfo(es->str,
								 "worker:%d, fisrt_time:%s, total_time:%s, total_num:%.0f\n",
								 ns->segindex0 + i,
								 startbuf,
								 totalbuf,
								 nsi->ntuples);
			}
			else
			{
				pxexplain_formatSeconds(startbuf, sizeof(startbuf),
										INSTR_TIME_GET_DOUBLE(timediff), false);
				pxexplain_formatSeconds(totalbuf, sizeof(totalbuf),
										nsi->total, false);

				ExplainOpenGroup("Worker", NULL, false, es);
				ExplainPropertyInteger("Woker index", NULL, ns->segindex0 + i, es);
				ExplainPropertyText("Time To First Result", startbuf, es);
				ExplainPropertyText("Time To Total Result", totalbuf, es);
				ExplainPropertyFloat("Tuples", NULL, nsi->ntuples, 1, es);
				ExplainCloseGroup("Worker", NULL, false, es);
			}
		}
	}
}								/* pxexplain_showExecStats */

/*
 *	ExplainPrintExecStatsEnd
 *			External API wrapper for pxexplain_showExecStatsEnd
 *
 * This is an externally exposed wrapper for pxexplain_showExecStatsEnd such
 * that extensions, such as auto_explain, can leverage the Greenplum specific
 * parts of the EXPLAIN machinery.
 */
void
ExplainPrintExecStatsEnd(ExplainState *es, QueryDesc *queryDesc)
{
	pxexplain_showExecStatsEnd(queryDesc->plannedstmt,
							   queryDesc->showstatctx,
							   queryDesc->estate, es);
}

/*
 * pxexplain_showExecStatsEnd
 *	  Called by qDisp process to format the overall statistics for a query
 *	  into the caller's buffer.
 *
 * 'ctx' is the PxExplain_ShowStatCtx object which was created by a call to
 *		pxexplain_showExecStatsBegin() and contains statistics which have
 *		been accumulated over a series of calls to pxexplain_showExecStats().
 *		Invalid on return (it is freed).
 *
 * This doesn't free the PxExplain_ShowStatCtx object or buffers, because
 * they will be free'd shortly by the end of statement anyway.
 */
static void
pxexplain_showExecStatsEnd(struct PlannedStmt *stmt,
						   struct PxExplain_ShowStatCtx *showstatctx,
						   struct EState *estate,
						   ExplainState *es)
{
	if (!es->summary)
		return;

	pxexplain_formatSlicesOutput(showstatctx, estate, es);

	/* TODO add statement statistics */
}

/*
 * Given a statistics context search for all the slice statistics
 * and format them to the correct layout
 */
static void
pxexplain_formatSlicesOutput(struct PxExplain_ShowStatCtx *showstatctx,
							 struct EState *estate,
							 ExplainState *es)
{
	/* TODO: enable it after per-slice memory consumption tracking support */
#if 0
	ExecSlice  *slice;
	int			sliceIndex;
	int			flag;
	double		total_memory_across_slices = 0;

	char		avgbuf[50];
	char		maxbuf[50];
	char		segbuf[50];

	if (showstatctx->nslice > 0)
		ExplainOpenGroup("Slice statistics", "Slice statistics", false, es);

	for (sliceIndex = 0; sliceIndex < showstatctx->nslice; sliceIndex++)
	{
		PxExplain_SliceSummary *ss = &showstatctx->slices[sliceIndex];
		PxExplain_DispatchSummary *ds = &ss->dispatchSummary;

		flag = es->str->len;
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{

			appendStringInfo(es->str, "  (slice%d) ", sliceIndex);
			if (sliceIndex < 10)
				appendStringInfoChar(es->str, ' ');

			appendStringInfoString(es->str, "  ");
		}
		else
		{
			ExplainOpenGroup("Slice", NULL, true, es);
			ExplainPropertyInteger("Slice", NULL, sliceIndex, es);
		}

		/* Worker counts */
		slice = getCurrentSlice(estate, sliceIndex);
		if (slice &&
			list_length(slice->segments) > 0 &&
			list_length(slice->segments) != ss->dispatchSummary.nOk)
		{
			int			nNotDispatched;
			StringInfoData workersInformationText;

			nNotDispatched = list_length(slice->segments) - ds->nResult + ds->nNotDispatched;

			es->str->data[flag] = (ss->dispatchSummary.nError > 0) ? 'X' : '_';

			initStringInfo(&workersInformationText);
			appendStringInfo(&workersInformationText, "Workers:");

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				if (ds->nError == 1)
				{
					appendStringInfo(&workersInformationText,
									 " %d error;",
									 ds->nError);
				}
				else if (ds->nError > 1)
				{
					appendStringInfo(&workersInformationText,
									 " %d errors;",
									 ds->nError);
				}
			}
			else
			{
				ExplainOpenGroup("Workers", "Workers", true, es);
				if (ds->nError > 0)
					ExplainPropertyInteger("Errors", NULL, ds->nError, es);
			}

			if (ds->nCanceled > 0)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					appendStringInfo(&workersInformationText,
									 " %d canceled;",
									 ds->nCanceled);
				}
				else
				{
					ExplainPropertyInteger("Canceled", NULL, ds->nCanceled, es);
				}
			}

			if (nNotDispatched > 0)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					appendStringInfo(&workersInformationText,
									 " %d not dispatched;",
									 nNotDispatched);
				}
				else
				{
					ExplainPropertyInteger("Not Dispatched", NULL, nNotDispatched, es);
				}
			}

			if (ds->nIgnorableError > 0)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					appendStringInfo(&workersInformationText,
									 " %d aborted;",
									 ds->nIgnorableError);
				}
				else
				{
					ExplainPropertyInteger("Aborted", NULL, ds->nIgnorableError, es);
				}
			}

			if (ds->nOk > 0)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					appendStringInfo(&workersInformationText,
									 " %d ok;",
									 ds->nOk);
				}
				else
				{
					ExplainPropertyInteger("Ok", NULL, ds->nOk, es);
				}
			}

			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				workersInformationText.len--;
				appendStringInfo(&workersInformationText, ".  ");
				ExplainPropertyText("Workers", workersInformationText.data, es);
			}
			else
			{
				ExplainCloseGroup("Workers", "Workers", true, es);
			}
		}

		/* Executor memory high-water mark */
		pxexplain_formatMemory(maxbuf, sizeof(maxbuf), ss->peakmemused.vmax);
		if (ss->peakmemused.vcnt == 1)
		{
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				const char *seg = segbuf;

				if (ss->peakmemused.imax >= 0)
				{
					pxexplain_formatSeg(segbuf, sizeof(segbuf), ss->peakmemused.imax, 999);
				}
				else if (slice && list_length(slice->segments) > 0)
				{
					seg = " (entry db)";
				}
				else
				{
					seg = "";
				}
				appendStringInfo(es->str,
								 "Executor memory: %s%s.",
								 maxbuf,
								 seg);
			}
			else
			{
				ExplainPropertyInteger("Executor Memory", "kB", ss->peakmemused.vmax, es);
			}
		}
		else if (ss->peakmemused.vcnt > 1)
		{
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				pxexplain_formatMemory(avgbuf, sizeof(avgbuf), pxexplain_agg_avg(&ss->peakmemused));
				pxexplain_formatSeg(segbuf, sizeof(segbuf), ss->peakmemused.imax, ss->nworker);
				appendStringInfo(es->str,
								 "Executor memory: %s avg x %d workers, %s max%s.",
								 avgbuf,
								 ss->peakmemused.vcnt,
								 maxbuf,
								 segbuf);
			}
			else
			{
				ExplainOpenGroup("Executor Memory", "Executor Memory", true, es);
				ExplainPropertyInteger("Average", "kB", pxexplain_agg_avg(&ss->peakmemused), es);
				ExplainPropertyInteger("Workers", NULL, ss->peakmemused.vcnt, es);
				ExplainPropertyInteger("Maximum Memory Used", "kB", ss->peakmemused.vmax, es);
				ExplainCloseGroup("Executor Memory", "Executor Memory", true, es);
			}
		}

		if (EXPLAIN_MEMORY_VERBOSITY_SUPPRESS < px_explain_memory_verbosity)
		{
			/* Vmem reserved by QEs */
			pxexplain_formatMemory(maxbuf, sizeof(maxbuf), ss->vmem_reserved.vmax);
			if (ss->vmem_reserved.vcnt == 1)
			{

				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					const char *seg = segbuf;

					if (ss->vmem_reserved.imax >= 0)
					{
						pxexplain_formatSeg(segbuf, sizeof(segbuf), ss->vmem_reserved.imax, 999);
					}
					else if (slice && list_length(slice->segments) > 0)
					{
						seg = " (entry db)";
					}
					else
					{
						seg = "";
					}
					appendStringInfo(es->str,
									 "  Vmem reserved: %s%s.",
									 maxbuf,
									 seg);
				}
				else
				{
					ExplainPropertyInteger("Virtual Memory", "kB", ss->vmem_reserved.vmax, es);
				}
			}
			else if (ss->vmem_reserved.vcnt > 1)
			{
				if (es->format == EXPLAIN_FORMAT_TEXT)
				{
					pxexplain_formatMemory(avgbuf, sizeof(avgbuf), pxexplain_agg_avg(&ss->vmem_reserved));
					pxexplain_formatSeg(segbuf, sizeof(segbuf), ss->vmem_reserved.imax, ss->nworker);
					appendStringInfo(es->str,
									 "  Vmem reserved: %s avg x %d workers, %s max%s.",
									 avgbuf,
									 ss->vmem_reserved.vcnt,
									 maxbuf,
									 segbuf);
				}
				else
				{
					ExplainOpenGroup("Virtual Memory", "Virtual Memory", true, es);
					ExplainPropertyInteger("Average", "kB", pxexplain_agg_avg(&ss->vmem_reserved), es);
					ExplainPropertyInteger("Workers", NULL, ss->vmem_reserved.vcnt, es);
					ExplainPropertyInteger("Maximum Memory Used", "kB", ss->vmem_reserved.vmax, es);
					ExplainCloseGroup("Virtual Memory", "Virtual Memory", true, es);
				}

			}
		}

		/* Work_mem used/wanted (max over all nodes and workers of slice) */
		if (ss->workmemused_max + ss->workmemwanted_max > 0)
		{
			if (es->format == EXPLAIN_FORMAT_TEXT)
			{
				pxexplain_formatMemory(maxbuf, sizeof(maxbuf), ss->workmemused_max);
				appendStringInfo(es->str, "  Work_mem: %s max", maxbuf);
				if (ss->workmemwanted_max > 0)
				{
					es->str->data[flag] = '*';	/* draw attention to this
												 * slice */
					pxexplain_formatMemory(maxbuf, sizeof(maxbuf), ss->workmemwanted_max);
					appendStringInfo(es->str, ", %s wanted", maxbuf);
				}
				appendStringInfoChar(es->str, '.');
			}
			else
			{
				ExplainPropertyInteger("Work Maximum Memory", "kB", ss->workmemused_max, es);
			}
		}

		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoChar(es->str, '\n');

		ExplainCloseGroup("Slice", NULL, true, es);
	}

	if (showstatctx->nslice > 0)
		ExplainCloseGroup("Slice statistics", "Slice statistics", false, es);

	if (total_memory_across_slices > 0)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfo(es->str, "Total memory used across slices: %.0fK bytes \n", total_memory_across_slices);
		}
		else
		{
			ExplainPropertyInteger("Total memory used across slices", "bytes", total_memory_across_slices, es);
		}
	}
#endif
}
