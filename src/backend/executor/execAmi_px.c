/*-------------------------------------------------------------------------
 *
 * execAmi_px.c
 *	  Miscellaneous executor access method routines for PX.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/executor/execAmi_px.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeMotion_px.h"
#include "executor/nodeModifyTable.h"
#include "executor/nodeSort.h"

/* TODO: pg15_px - not yet added */
#if 0
#include "executor/nodePartitionSelector.h"
#include "executor/nodeSequence.h"
#include "executor/nodeShareInputScan.h"
#endif

/* nodeWindowAgg */
static void
ExecSquelchWindowAgg(WindowAggState *node)
{
	ExecSquelchNode(outerPlanState(node));
}

/* nodeRecursiveunion */
static void
ExecSquelchRecursiveUnion(RecursiveUnionState *node)
{
	tuplestore_clear(node->working_table);
	tuplestore_clear(node->intermediate_table);

	ExecSquelchNode(outerPlanState(node));
	ExecSquelchNode(innerPlanState(node));
}

/* nodeHashjoin.c */
static void
ExecEagerFreeHashJoin(HashJoinState *node)
{
	if (node->hj_HashTable != NULL)
	{
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;
	}
}

static void
ExecSquelchHashJoin(HashJoinState *node)
{
	ExecEagerFreeHashJoin(node);
	ExecSquelchNode(outerPlanState(node));
	ExecSquelchNode(innerPlanState(node));
}

/* nodeFunctionscan */
static void
ExecEagerFreeFunctionScan(FunctionScanState *node)
{
#if 0
	for (i = 0; i < node->nfuncs; i++)
	{
		FunctionScanPerFuncState *fs =
			&node->funcstates[i];

		if (fs->func_slot)
			ExecClearTuple(fs->func_slot);

		if (fs->tstore != NULL)
		{
			tuplestore_end(node->funcstates[i].tstore);
			fs->tstore = NULL;
		}
	}
#endif
}

static void
ExecSquelchFunctionScan(FunctionScanState *node)
{
	ExecEagerFreeFunctionScan(node);
}


/* nodeBitmapHeapscan */
static void
freeBitmapState(BitmapHeapScanState *scanstate)
{
	/* BitmapIndexScan is the owner of the bitmap memory. Don't free it here */
	scanstate->tbm = NULL;

	/*
	 * Likewise, the tbmres member is owned by the iterator. It'll be freed
	 * during end_iterate.
	 */

	scanstate->tbmres = NULL;
}

static void
ExecEagerFreeBitmapHeapScan(BitmapHeapScanState *node)
{
	/* freeFetchDesc(node); */
	freeBitmapState(node);
}

static void
ExecSquelchBitmapHeapScan(BitmapHeapScanState *node)
{
	ExecEagerFreeBitmapHeapScan(node);
}


/* nodeagg */
static void
ExecEagerFreeAgg(AggState *node)
{
	int			transno;
	int			numGroupingSets = Max(node->maxsets, 1);
	int			setno;

	/* Make sure we have closed any open tuplesorts */
	if (node->sort_in)
	{
		tuplesort_end(node->sort_in);
		node->sort_in = NULL;
	}
	if (node->sort_out)
	{
		tuplesort_end(node->sort_out);
		node->sort_out = NULL;
	}

	for (transno = 0; transno < node->numtrans; transno++)
	{
		for (setno = 0; setno < numGroupingSets; setno++)
		{
			AggStatePerTrans pertrans = &node->pertrans[transno];

			if (pertrans->sortstates[setno])
			{
				tuplesort_end(pertrans->sortstates[setno]);
				pertrans->sortstates[setno] = NULL;
			}
		}
	}

	/*
	 * We don't need to ReScanExprContext the output tuple context here;
	 * ExecReScan already did it. But we do need to reset our per-grouping-set
	 * contexts, which may have transvalues stored in them. (We use rescan
	 * rather than just reset because transfns may have registered callbacks
	 * that need to be run now.)
	 *
	 * Note that with AGG_HASHED, the hash table is allocated in a sub-context
	 * of the aggcontext. This used to be an issue, but now, resetting a
	 * context automatically deletes sub-contexts too.
	 */

	for (setno = 0; setno < numGroupingSets; setno++)
	{
		ReScanExprContext(node->aggcontexts[setno]);
	}

	/* Release first tuple of group, if we have made a copy. */
	if (node->grp_firstTuple != NULL)
	{
		pfree(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}
}

/* nodeAgg */
static void
ExecSquelchAgg(AggState *node)
{
	ExecEagerFreeAgg(node);
	ExecSquelchNode(outerPlanState(node));
}

/* nodeSubplan */
static void
ExecSquelchSubqueryScan(SubqueryScanState *node)
{
	/* Recurse to subquery */
	ExecSquelchNode(node->subplan);
}

/* nodeAppend */
static void
ExecSquelchAppend(AppendState *node)
{
	int			i;

	for (i = 0; i < node->as_nplans; i++)
		ExecSquelchNode(node->appendplans[i]);
}

/*
 * POLAR px
 * ExecSquelchNode
 *
 * When a node decides that it will not consume any more input tuples from a
 * subtree that has not yet returned end-of-data, it must call
 * ExecSquelchNode() on the subtree.
 *
 * This is necessary, to avoid deadlock with Motion nodes. There might be a
 * receiving Motion node in the subtree, and it needs to let the sender side
 * of the Motion know that we will not be reading any more tuples. We might
 * have sibling PX processes in other segments that are still waiting for
 * tuples from the sender Motion, but if the sender's send queue is full, it
 * will never send them. By explicitly telling the sender that we will not be
 * reading any more tuples, it knows to not wait for us, and can skip over,
 * and send tuples to the other PXs that might be waiting.
 *
 * This also gives memory-hungry nodes a chance to release memory earlier, so
 * that other nodes higher up in the plan can make use of it. The Squelch
 * function for many node call a separate node-specific ExecEagerFree*()
 * function to do that.
 *
 * After a node has been squelched, you mustn't try to read more tuples from
 * it. However, ReScanning the node will "un-squelch" it, allowing to read
 * again. Squelching a node is roughly equivalent to fetching and discarding
 * all tuples from it.
 */
void
ExecSquelchNode(PlanState *node)
{
	ListCell   *lc;

	if (!node)
		return;

	if (node->squelched)
		return;

	switch (nodeTag(node))
	{
		case T_AppendState:
			ExecSquelchAppend((AppendState *) node);
			break;

		case T_ModifyTableState:
			ExecSquelchModifyTable((ModifyTableState *) node);
			/* ModifyTable and Reture */
			return;

		case T_SubqueryScanState:
			ExecSquelchSubqueryScan((SubqueryScanState *) node);
			break;

			/*
			 * Node types that need no special handling, just recurse to
			 * children.
			 */
			/* POLAR px */
		case T_AssertOpState:
			/* POLAR end */
		case T_BitmapAndState:
		case T_BitmapOrState:
		case T_LimitState:
		case T_LockRowsState:
		case T_NestLoopState:
		case T_MergeJoinState:
		case T_SetOpState:
		case T_UniqueState:
		case T_HashState:
		case T_WorkTableScanState:
		case T_ResultState:
		case T_ProjectSetState:
			ExecSquelchNode(outerPlanState(node));
			ExecSquelchNode(innerPlanState(node));
			break;

			/*
			 * These node types have nothing to do, and have no children.
			 */
		case T_SeqScanState:
		case T_IndexScanState:
		case T_IndexOnlyScanState:
		case T_BitmapIndexScanState:
		case T_ValuesScanState:
		case T_TidScanState:
		case T_SampleScanState:
			break;

			/*
			 * Node types that consume resources that we want to free eagerly,
			 * as soon as possible.
			 */
		case T_RecursiveUnionState:
			ExecSquelchRecursiveUnion((RecursiveUnionState *) node);
			break;

		case T_BitmapHeapScanState:
			ExecSquelchBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_FunctionScanState:
			ExecSquelchFunctionScan((FunctionScanState *) node);
			break;

		case T_HashJoinState:
			ExecSquelchHashJoin((HashJoinState *) node);
			break;

		case T_MaterialState:
			ExecSquelchMaterial((MaterialState *) node);
			break;

		case T_SortState:
			ExecSquelchSort((SortState *) node);
			break;

		case T_AggState:
			ExecSquelchAgg((AggState *) node);
			break;

		case T_WindowAggState:
			ExecSquelchWindowAgg((WindowAggState *) node);
			break;

			/* POLAR px */
			/* TODO: pg15_px */
#if 0
		case T_SequenceState:
			ExecSquelchSequence((SequenceState *) node);
			break;

		case T_ShareInputScanState:
			ExecSquelchShareInputScan((ShareInputScanState *) node);
			break;
#endif

		case T_MotionState:
			ExecSquelchMotion((MotionState *) node);
			break;
			/* POLAR end */

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}

	/*
	 * Also recurse into subplans, if any. (InitPlans are handled as a
	 * separate step, at executor startup, and don't need squelching.)
	 */
	foreach(lc, node->subPlan)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lc);
		PlanState  *ips = sps->planstate;

		if (!ips)
			elog(ERROR, "subplan has no planstate");
		ExecSquelchNode(ips);
	}

	node->squelched = true;
}
