/*-------------------------------------------------------------------------
 *
 * px_mutate.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Portions Copyright (c) 2004-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/px_mutate.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "catalog/px_policy.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"	/* for rt_fetch() */
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"		/* RelationGetPartitioningKey() */
#include "utils/syscache.h"

#include "px/px_hash.h"
#include "px/px_llize.h"
#include "px/px_mutate.h"
#include "px/px_plan.h"
#include "px/px_util.h"
#include "px/px_vars.h"

/*
 * ApplyMotionState holds state for the recursive apply_motion_mutator().
 */
typedef struct ApplyMotionSubPlanState
{
	int			sliceDepth;
	Flow	   *parentFlow;
	Flow	   *outerQueryFlow;
	bool		is_initplan;
	bool		useHashTable;
	bool		processed;

} ApplyMotionSubPlanState;

typedef struct ApplyMotionState
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
	int			nextMotionID;
	int			sliceDepth;

	ApplyMotionSubPlanState *subplans;

	List	   *subplan_workingQueue;

	Flow	   *currentPlanFlow;
	Flow	   *outer_query_flow;
} ApplyMotionState;

typedef struct
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
	EState	   *estate;
	bool		single_row_insert;
	List	   *cursorPositions;
} pre_dispatch_function_evaluation_context;

/*
 * Forward Declarations
 */
static Node *pre_dispatch_function_evaluation_mutator(Node *node,
													  pre_dispatch_function_evaluation_context *context);

static bool replace_shareinput_targetlists_walker(Node *node, PlannerInfo *root, bool fPop);

#ifdef FAULT_INJECTOR
Node	   *(*test_pre_dispatch_function_evaluation_mutator) (Node *node,
															  pre_dispatch_function_evaluation_context *context) = pre_dispatch_function_evaluation_mutator;
#endif

/* --------------------------------------------------------------------
 *
 *	Static Helper routines
 * --------------------------------------------------------------------
 */

/*
 * Code that mutate the tree for share input
 *
 * After the planner, the plan is really a DAG.  SISCs will have valid
 * pointer to the underlying share.  However, other code (setrefs etc)
 * depends on the fact that the plan is a tree.  We first mutate the DAG
 * to a tree.
 *
 * Next, we will need to decide if the share is cross slices.  If the share
 * is not cross slice, we do not need the syncrhonization, and it is possible to
 * keep the Material/Sort in memory to save a sort.
 *
 * It is essential that we walk the tree in the same order as the ExecProcNode start
 * execution, otherwise, deadlock may rise.
 */

/* Walk the tree for shareinput.
 * Shareinput fix shared_as_id and underlying_share_id of nodes in place.  We do not want to use
 * the ordinary tree walker as it is unnecessary to make copies etc.
 */
typedef bool (*SHAREINPUT_MUTATOR) (Node *node, PlannerInfo *root, bool fPop);
static void
shareinput_walker(SHAREINPUT_MUTATOR f, Node *node, PlannerInfo *root)
{
	Plan	   *plan = NULL;
	bool		recursive_down;

	if (node == NULL)
		return;

	if (IsA(node, List))
	{
		List	   *l = (List *) node;
		ListCell   *lc;

		foreach(lc, l)
		{
			Node	   *n = lfirst(lc);

			shareinput_walker(f, n, root);
		}
		return;
	}

	if (!is_plan_node(node))
		return;

	plan = (Plan *) node;
	recursive_down = (*f) (node, root, false);

	if (recursive_down)
	{
		if (IsA(node, Append))
		{
			ListCell   *cell;
			Append	   *app = (Append *) node;

			foreach(cell, app->appendplans)
				shareinput_walker(f, (Node *) lfirst(cell), root);
		}
		else if (IsA(node, ModifyTable))
		{
			elog(ERROR, "pg15_px: adapt new ModifyTable struct for shareinput walker");
#if 0
			ListCell   *cell;
			ModifyTable *mt = (ModifyTable *) node;

			foreach(cell, mt->plans)
				shareinput_walker(f, (Node *) lfirst(cell), root);
#endif
		}
		else if (IsA(node, SubqueryScan))
		{
			SubqueryScan *subqscan = (SubqueryScan *) node;
			PlannerGlobal *glob = root->glob;
			PlannerInfo *subroot;
			List	   *save_rtable;
			RelOptInfo *rel;

			/*
			 * If glob->finalrtable is not NULL, rtables have been flatten,
			 * thus we should use glob->finalrtable instead.
			 */
			save_rtable = glob->share.curr_rtable;
			if (root->glob->finalrtable == NULL)
			{
				rel = find_base_rel(root, subqscan->scan.scanrelid);

				/*
				 * The Assert() on RelOptInfo's subplan being same as the
				 * subqueryscan's subplan, is valid in Upstream but for not
				 * for PX, since we create a new copy of the subplan if two
				 */
				subroot = rel->subroot;
				glob->share.curr_rtable = subroot->parse->rtable;
			}
			else
			{
				subroot = root;
				glob->share.curr_rtable = glob->finalrtable;
			}
			shareinput_walker(f, (Node *) subqscan->subplan, subroot);
			glob->share.curr_rtable = save_rtable;
		}
		else if (IsA(node, BitmapAnd))
		{
			ListCell   *cell;
			BitmapAnd  *ba = (BitmapAnd *) node;

			foreach(cell, ba->bitmapplans)
				shareinput_walker(f, (Node *) lfirst(cell), root);
		}
		else if (IsA(node, BitmapOr))
		{
			ListCell   *cell;
			BitmapOr   *bo = (BitmapOr *) node;

			foreach(cell, bo->bitmapplans)
				shareinput_walker(f, (Node *) lfirst(cell), root);
		}
		else if (IsA(node, NestLoop))
		{
			/*
			 * Nest loop join is strange.  Exec order depends on
			 * prefetch_inner
			 */
			NestLoop   *nl = (NestLoop *) node;

			if (nl->join.prefetch_inner)
			{
				shareinput_walker(f, (Node *) plan->righttree, root);
				shareinput_walker(f, (Node *) plan->lefttree, root);
			}
			else
			{
				shareinput_walker(f, (Node *) plan->lefttree, root);
				shareinput_walker(f, (Node *) plan->righttree, root);
			}
		}
		else if (IsA(node, HashJoin))
		{
			/* Hash join the hash table is at inner */
			shareinput_walker(f, (Node *) plan->righttree, root);
			shareinput_walker(f, (Node *) plan->lefttree, root);
		}
		else if (IsA(node, MergeJoin))
		{
			MergeJoin  *mj = (MergeJoin *) node;

			if (mj->unique_outer)
			{
				shareinput_walker(f, (Node *) plan->lefttree, root);
				shareinput_walker(f, (Node *) plan->righttree, root);
			}
			else
			{
				shareinput_walker(f, (Node *) plan->righttree, root);
				shareinput_walker(f, (Node *) plan->lefttree, root);
			}
		}
		else if (IsA(node, Sequence))
		{
			ListCell   *cell = NULL;
			Sequence   *sequence = (Sequence *) node;

			foreach(cell, sequence->subplans)
			{
				shareinput_walker(f, (Node *) lfirst(cell), root);
			}
		}
		else
		{
			shareinput_walker(f, (Node *) plan->lefttree, root);
			shareinput_walker(f, (Node *) plan->righttree, root);
			shareinput_walker(f, (Node *) plan->initPlan, root);
		}
	}

	(*f) (node, root, true);
}

/*
 * Create a fake CTE range table entry that reflects the target list of a
 * shared input.
 */
static RangeTblEntry *
create_shareinput_producer_rte(ApplyShareInputContext * ctxt, int share_id,
							   int refno)
{
	int			attno = 1;
	ListCell   *lc;
	Plan	   *subplan;
	char		buf[100];
	RangeTblEntry *rte;
	List	   *colnames = NIL;
	List	   *coltypes = NIL;
	List	   *coltypmods = NIL;
	List	   *colcollations = NIL;

	Assert(ctxt->shared_plans);
	Assert(ctxt->shared_input_count > share_id);
	subplan = ctxt->shared_plans[share_id];

	foreach(lc, subplan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Oid			vartype;
		int32		vartypmod;
		Oid			varcollid;
		char	   *resname;

		vartype = exprType((Node *) tle->expr);
		vartypmod = exprTypmod((Node *) tle->expr);
		varcollid = exprCollation((Node *) tle->expr);

		/*
		 * We should've filled in tle->resname in shareinput_save_producer().
		 * Note that it's too late to call get_tle_name() here, because this
		 * runs after all the varnos in Vars have already been changed to
		 * INNER_VAR/OUTER_VAR.
		 */
		resname = tle->resname;
		if (!resname)
			resname = pstrdup("unnamed_attr");

		colnames = lappend(colnames, makeString(resname));
		coltypes = lappend_oid(coltypes, vartype);
		coltypmods = lappend_int(coltypmods, vartypmod);
		colcollations = lappend_oid(colcollations, varcollid);
		attno++;
	}

	/*
	 * Create a new RTE. Note that we use a different RTE for each reference,
	 * because we want to give each reference a different name.
	 */
	snprintf(buf, sizeof(buf), "share%d_ref%d", share_id, refno);

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_CTE;
	rte->ctename = pstrdup(buf);
	rte->ctelevelsup = 0;
	rte->self_reference = false;
	rte->alias = NULL;

	rte->eref = makeAlias(rte->ctename, colnames);
	rte->coltypes = coltypes;
	rte->coltypmods = coltypmods;
	rte->colcollations = colcollations;

	rte->inh = false;
	rte->inFromCl = false;

	rte->requiredPerms = 0;
	rte->checkAsUser = InvalidOid;

	return rte;
}

/* POLAR px begin */

/*
 * Memorize the shared plan of a shared input in an array, one per share_id.
 */
static void
shareinput_save_producer(ShareInputScan *plan, ApplyShareInputContext * ctxt)
{
	int			share_id = plan->share_id;
	int			new_shared_input_count = (share_id + 1);

	Assert(plan->share_id >= 0);

	if (ctxt->shared_plans == NULL)
	{
		ctxt->shared_plans = palloc0(sizeof(Plan *) * new_shared_input_count);
		ctxt->shared_input_count = new_shared_input_count;
	}
	else if (ctxt->shared_input_count < new_shared_input_count)
	{
		ctxt->shared_plans = repalloc(ctxt->shared_plans, new_shared_input_count * sizeof(Plan *));
		memset(&ctxt->shared_plans[ctxt->shared_input_count], 0, (new_shared_input_count - ctxt->shared_input_count) * sizeof(Plan *));
		ctxt->shared_input_count = new_shared_input_count;
	}

	Assert(ctxt->shared_plans[share_id] == NULL);
	ctxt->shared_plans[share_id] = plan->scan.plan.lefttree;
}

/*
 * Collect all the producer ShareInput nodes into an array, for later use by
 * replace_shareinput_targetlists().
 *
 * This is a stripped-down version of apply_shareinput_dag_to_tree(), for use
 * on PXOPT-produced plans. PXOPT assigns share_ids to all ShareInputScan nodes,
 * and only producer nodes have a subtree, so we don't need to do the DAG to
 * tree conversion or assign share_ids here.
 */
static bool
collect_shareinput_producers_walker(Node *node, PlannerInfo *root, bool fPop)
{
	PlannerGlobal *glob = root->glob;
	ApplyShareInputContext *ctxt = &glob->share;

	if (fPop)
		return true;

	if (IsA(node, ShareInputScan))
	{
		ShareInputScan *siscan = (ShareInputScan *) node;
		Plan	   *subplan = siscan->scan.plan.lefttree;

		Assert(siscan->share_id >= 0);

		if (subplan)
			shareinput_save_producer(siscan, ctxt);
	}

	return true;
}

void
collect_shareinput_producers(PlannerInfo *root, Plan *plan)
{
	PlannerGlobal *glob = root->glob;

	glob->share.curr_rtable = glob->finalrtable;
	shareinput_walker(collect_shareinput_producers_walker, (Node *) plan, root);
}

/* Some helper: implements a stack using List. */
static void
shareinput_pushmot(ApplyShareInputContext * ctxt, int motid)
{
	ctxt->motStack = lcons_int(motid, ctxt->motStack);
}
static void
shareinput_popmot(ApplyShareInputContext * ctxt)
{
	ctxt->motStack = list_delete_first(ctxt->motStack);
}
static int
shareinput_peekmot(ApplyShareInputContext * ctxt)
{
	return linitial_int(ctxt->motStack);
}

/* POLAR px end */

/*
 * Replace the target list of ShareInputScan nodes, with references
 * to CTEs that we build on the fly.
 *
 * Only one of the ShareInputScan nodes in a plan tree contains the real
 * child plan, while others contain just a "share id" that binds all the
 * ShareInputScan nodes sharing the same input together. The missing
 * child plan is a problem for EXPLAIN, as any OUTER Vars in the
 * ShareInputScan's target list cannot be resolved without the child
 * plan.
 *
 * To work around that issue, create a CTE for each shared input node, with
 * columns that match the target list of the SharedInputScan's subplan,
 * and replace the target list entries of the SharedInputScan with
 * Vars that point to the CTE instead of the child plan.
 */
Plan *
replace_shareinput_targetlists(PlannerInfo *root, Plan *plan)
{
	shareinput_walker(replace_shareinput_targetlists_walker, (Node *) plan, root);
	return plan;
}

static bool
replace_shareinput_targetlists_walker(Node *node, PlannerInfo *root, bool fPop)
{
	PlannerGlobal *glob = root->glob;
	ApplyShareInputContext *ctxt = &glob->share;

	if (fPop)
		return true;

	if (IsA(node, ShareInputScan))
	{
		ShareInputScan *sisc = (ShareInputScan *) node;
		int			share_id = sisc->share_id;
		ListCell   *lc;
		int			attno;
		List	   *newtargetlist;
		RangeTblEntry *rte;

		/*
		 * Note that even though the planner assigns sequential share_ids for
		 * each shared node, so that share_id is always below
		 * list_length(ctxt->sharedNodes), PXOPT has a different assignment
		 * scheme. So we have to be prepared for any share_id, at least when
		 * PXOPT is in use.
		 */
		if (ctxt->share_refcounts == NULL)
		{
			int			new_sz = share_id + 1;

			ctxt->share_refcounts = palloc0(new_sz * sizeof(int));
			ctxt->share_refcounts_sz = new_sz;
		}
		else if (share_id >= ctxt->share_refcounts_sz)
		{
			int			old_sz = ctxt->share_refcounts_sz;
			int			new_sz = share_id + 1;

			ctxt->share_refcounts = repalloc(ctxt->share_refcounts, new_sz * sizeof(int));
			memset(&ctxt->share_refcounts[old_sz], 0, (new_sz - old_sz) * sizeof(int));
			ctxt->share_refcounts_sz = new_sz;
		}

		ctxt->share_refcounts[share_id]++;

		/*
		 * Create a new RTE. Note that we use a different RTE for each
		 * reference, because we want to give each reference a different name.
		 */
		rte = create_shareinput_producer_rte(ctxt, share_id,
											 ctxt->share_refcounts[share_id]);

		glob->finalrtable = lappend(glob->finalrtable, rte);
		sisc->scan.scanrelid = list_length(glob->finalrtable);

		/*
		 * Replace all the target list entries.
		 *
		 * SharedInputScan nodes are not projection-capable, so the target
		 * list of the SharedInputScan matches the subplan's target list.
		 */
		newtargetlist = NIL;
		attno = 1;
		foreach(lc, sisc->scan.plan.targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			TargetEntry *newtle = flatCopyTargetEntry(tle);

			newtle->expr = (Expr *) makeVar(sisc->scan.scanrelid, attno,
											exprType((Node *) tle->expr),
											exprTypmod((Node *) tle->expr),
											exprCollation((Node *) tle->expr),
											0);
			newtargetlist = lappend(newtargetlist, newtle);
			attno++;
		}
		sisc->scan.plan.targetlist = newtargetlist;
	}

	return true;
}

/* POLAR px begin */

/*
 * First walk on shareinput xslice. Collect information about the producer
 * and consumer slice IDs for each share. It also builds a list of shares
 * that should run in the QD.
 */
static bool
shareinput_mutator_xslice_1(Node *node, PlannerInfo *root, bool fPop)
{
	PlannerGlobal *glob = root->glob;
	ApplyShareInputContext *ctxt = &glob->share;
	Plan	   *plan = (Plan *) node;

	if (fPop)
	{
		if (IsA(plan, Motion))
			shareinput_popmot(ctxt);
		return false;
	}

	if (IsA(plan, Motion))
	{
		Motion	   *motion = (Motion *) plan;

		shareinput_pushmot(ctxt, motion->motionID);
		return true;
	}

	if (IsA(plan, ShareInputScan))
	{
		ShareInputScan *sisc = (ShareInputScan *) plan;
		int			motId = shareinput_peekmot(ctxt);
		Plan	   *shared = plan->lefttree;
		PlanSlice  *currentSlice;
		ApplyShareInputContextPerShare *share_info;

		share_info = &ctxt->shared_inputs[sisc->share_id];

		currentSlice = &ctxt->slices[motId];
		if (currentSlice->gangType == GANGTYPE_UNALLOCATED ||
			currentSlice->gangType == GANGTYPE_ENTRYDB_READER)
		{
			ctxt->qdShares = bms_add_member(ctxt->qdShares, sisc->share_id);
		}

		/* Remember information about the slice that this instance appears in. */
		if (shared)
			ctxt->shared_inputs[sisc->share_id].producer_slice_id = motId;
		share_info->participant_slices = bms_add_member(share_info->participant_slices, motId);

		sisc->this_slice_id = motId;
	}

	return true;
}

/*
 * Second pass:
 * 1. Mark shareinput scans with multiple consumer slices as cross-slice.
 * 2. Fill 'share_type' and 'share_id' fields in the shared Material/Sort nodes.
 */
static bool
shareinput_mutator_xslice_2(Node *node, PlannerInfo *root, bool fPop)
{
	PlannerGlobal *glob = root->glob;
	ApplyShareInputContext *ctxt = &glob->share;
	Plan	   *plan = (Plan *) node;

	if (fPop)
	{
		if (IsA(plan, Motion))
			shareinput_popmot(ctxt);
		return false;
	}

	if (IsA(plan, Motion))
	{
		Motion	   *motion = (Motion *) plan;

		shareinput_pushmot(ctxt, motion->motionID);
		return true;
	}

	if (IsA(plan, ShareInputScan))
	{
		ShareInputScan *sisc = (ShareInputScan *) plan;
		int			motId = shareinput_peekmot(ctxt);
		ApplyShareInputContextPerShare *pershare;

		pershare = &ctxt->shared_inputs[sisc->share_id];

		if (bms_num_members(pershare->participant_slices) > 1)
		{
			Assert(!sisc->cross_slice);
			sisc->cross_slice = true;
		}

		sisc->producer_slice_id = pershare->producer_slice_id;
		sisc->nconsumers = bms_num_members(pershare->participant_slices) - 1;

		/*
		 * If this share needs to run in the QD, mark the slice accordingly.
		 */
		if (bms_is_member(sisc->share_id, ctxt->qdShares))
		{
			PlanSlice  *currentSlice = &ctxt->slices[motId];

			switch (currentSlice->gangType)
			{
				case GANGTYPE_UNALLOCATED:
				case GANGTYPE_ENTRYDB_READER:
					break;
				case GANGTYPE_SINGLETON_READER:
#if 0
					currentSlice->gangType = GANGTYPE_ENTRYDB_READER;
					break;
#endif
				case GANGTYPE_PRIMARY_READER:
				case GANGTYPE_PRIMARY_WRITER:
					elog(ERROR, "cannot share ShareInputScan between QD and primary reader/write gang");
					break;
			}
		}
	}
	return true;
}

/*
 * Scan through the plan tree and make note of which Share Input Scans
 * are cross-slice.
 */
Plan *
apply_shareinput_xslice(Plan *plan, PlannerInfo *root)
{
	PlannerGlobal *glob = root->glob;
	ApplyShareInputContext *ctxt = &glob->share;
	ListCell   *lp,
			   *lr;
	int			subplan_id;

	/*
	 * If the plan tree has only one slice, there cannot be any cross-slice
	 * Share Input Scans. They were all marked as cross_slice=false when they
	 * were created. Note that we won't set slice_ids on them correctly; the
	 * executor knows not to expect that when numSlices == 1.
	 */
	if (root->glob->numSlices == 1)
		return plan;

	ctxt->motStack = NULL;
	ctxt->qdShares = NULL;
	ctxt->slices = root->glob->slices;

	ctxt->shared_inputs = palloc0(ctxt->shared_input_count * sizeof(ApplyShareInputContextPerShare));

	shareinput_pushmot(ctxt, 0);

	/*
	 * Walk the tree.  See comment for each pass for what each pass will do.
	 * The context is used to carry information from one pass to another, as
	 * well as within a pass.
	 */

	/*
	 * A subplan might have a SharedScan consumer while the SharedScan
	 * producer is in the main plan, or vice versa. So in the first pass, we
	 * walk through all plans and collect all producer subplans into the
	 * context, before processing the consumers.
	 */
	subplan_id = 0;
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = (PlannerInfo *) lfirst(lr);
		int			slice_id = glob->subplan_sliceIds[subplan_id];

		shareinput_pushmot(ctxt, slice_id);
		shareinput_walker(shareinput_mutator_xslice_1, (Node *) subplan, subroot);
		shareinput_popmot(ctxt);
		subplan_id++;
	}
	shareinput_walker(shareinput_mutator_xslice_1, (Node *) plan, root);

	/* Now walk the tree again, and process all the consumers. */
	subplan_id = 0;
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = (PlannerInfo *) lfirst(lr);
		int			slice_id = glob->subplan_sliceIds[subplan_id];

		shareinput_pushmot(ctxt, slice_id);
		shareinput_walker(shareinput_mutator_xslice_2, (Node *) subplan, subroot);
		shareinput_popmot(ctxt);
		subplan_id++;
	}
	shareinput_walker(shareinput_mutator_xslice_2, (Node *) plan, root);

	return plan;
}

/* POLAR px end */

/*
 * Hash a list of const values with GPDB's hash function
 */
int32
pxhash_const_list(List *plConsts, int iSegments, Oid *hashfuncs)
{
	ListCell   *lc;
	PxHash	   *ppxhash;
	int			i;

	Assert(0 < list_length(plConsts));

	ppxhash = makePxHash(iSegments, list_length(plConsts), hashfuncs);

	pxhashinit(ppxhash);

	Assert(0 < list_length(plConsts));

	i = 0;
	foreach(lc, plConsts)
	{
		Const	   *pconst = (Const *) lfirst(lc);

		pxhash(ppxhash, i + 1, pconst->constvalue, pconst->constisnull);
		i++;
	}

	return pxhashreduce(ppxhash);
}

/*
 * Construct an expression that checks whether the current worker is
 * 'workerid'.
 */
Node *
makePXWorkerIndexFilterExpr(int workerid)
{
	/*
	 * get px_workerid_funcid if it is InvalidOid.
	 */
	if (px_workerid_funcid == InvalidOid)
		update_px_workerid_funcid();

	/* Build an expression: polar_px_workerid() = <workerid> */
	return (Node *)
		make_opclause(Int4EqualOperator,
					  BOOLOID,
					  false,	/* opretset */
					  (Expr *) makeFuncExpr(px_workerid_funcid,
											INT4OID,
											NIL,	/* args */
											InvalidOid,
											InvalidOid,
											COERCE_EXPLICIT_CALL),
					  (Expr *) makeConst(INT4OID,
										 -1,	/* consttypmod */
										 InvalidOid,	/* constcollid */
										 sizeof(int32),
										 Int32GetDatum(workerid),
										 false, /* constisnull */
										 true), /* constbyval */
					  InvalidOid,	/* opcollid */
					  InvalidOid	/* inputcollid */
		);
}

/*
 * Evaluate functions to constants.
 */
Node *
exec_make_plan_constant(struct PlannedStmt *stmt, EState *estate, bool is_SRI,
						List **cursorPositions)
{
	pre_dispatch_function_evaluation_context pcontext;
	Node	   *result;

	Assert(stmt);
	exec_init_plan_tree_base(&pcontext.base, stmt);
	pcontext.single_row_insert = is_SRI;
	pcontext.cursorPositions = NIL;
	pcontext.estate = estate;

	result = pre_dispatch_function_evaluation_mutator((Node *) stmt->planTree, &pcontext);

	*cursorPositions = pcontext.cursorPositions;
	return result;
}

/*
 * Remove subquery field in RTE's with subquery kind.
 */
void
remove_subquery_in_RTEs(Node *node)
{
	if (node == NULL)
	{
		return;
	}

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (RTE_SUBQUERY == rte->rtekind && NULL != rte->subquery)
		{
			/*
			 * Replace subquery with a dummy subquery.
			 *
			 * XXX: We could save a lot more memory by deep-freeing the many
			 * fields in the Query too. But I'm not sure which of them might
			 * be shared by other objects in the tree.
			 */
			pfree(rte->subquery);
			rte->subquery = NULL;
		}

		return;
	}

	if (IsA(node, List))
	{
		List	   *list = (List *) node;
		ListCell   *lc = NULL;

		foreach(lc, list)
		{
			remove_subquery_in_RTEs((Node *) lfirst(lc));
		}
	}
}

/*
 * Let's evaluate all STABLE functions that have constant args before
 * dispatch, so we get a consistent view across PXs
 *
 * Also, if this is a single_row insert, let's evaluate nextval() and
 * currval() before dispatching
 */
static Node *
pre_dispatch_function_evaluation_mutator(Node *node,
										 pre_dispatch_function_evaluation_context *context)
{
	Node	   *new_node = 0;

	if (node == NULL)
		return NULL;

	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		/* Not replaceable, so just copy the Param (no need to recurse) */
		return (Node *) copyObject(param);
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;
		List	   *args;
		ListCell   *arg;
		Expr	   *simple;
		FuncExpr   *newexpr;
		bool		has_nonconst_input;

		Form_pg_proc funcform;
		EState	   *estate;
		ExprState  *exprstate;
		MemoryContext oldcontext;
		Datum		const_val;
		bool		const_is_null;
		int16		resultTypLen;
		bool		resultTypByVal;

		Oid			funcid;
		HeapTuple	func_tuple;

		/*
		 * Reduce constants in the FuncExpr's arguments. We know args is
		 * either NIL or a List node, so we can call expression_tree_mutator
		 * directly rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
												pre_dispatch_function_evaluation_mutator,
												(void *) context);

		funcid = expr->funcid;

		newexpr = makeNode(FuncExpr);
		newexpr->funcid = expr->funcid;
		newexpr->funcresulttype = expr->funcresulttype;
		newexpr->funcretset = expr->funcretset;
		newexpr->funcvariadic = expr->funcvariadic;
		newexpr->funcformat = expr->funcformat;
		newexpr->funccollid = expr->funccollid;
		newexpr->inputcollid = expr->inputcollid;
		newexpr->args = args;
		newexpr->location = expr->location;
		newexpr->isGlobalFunc = expr->isGlobalFunc;

		/* newexpr->is_tablefunc = expr->is_tablefunc; */

		/*
		 * Check for constant inputs
		 */
		has_nonconst_input = false;

		foreach(arg, args)
		{
			if (!IsA(lfirst(arg), Const))
			{
				has_nonconst_input = true;
				break;
			}
		}

		if (!has_nonconst_input)
		{
			bool		is_seq_func = false;
			bool		tup_or_set;

			func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(func_tuple))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			funcform = (Form_pg_proc) GETSTRUCT(func_tuple);

			/* can't handle set returning or row returning functions */
			tup_or_set = (funcform->proretset || type_is_rowtype(funcform->prorettype));

			ReleaseSysCache(func_tuple);

			/* can't handle it */
			if (tup_or_set)
			{
				/*
				 * We haven't mutated this node, but we still return the
				 * mutated arguments.
				 *
				 * If we don't do this, we'll miss out on transforming
				 * function arguments which are themselves functions we need
				 * to mutated. For example, select foo(now()).
				 */
				return (Node *) newexpr;
			}

			/*
			 * Here we want to mark any statement that is going to use a
			 * sequence as dirty.  Doing this means that the QC will flush the
			 * xlog which will also flush any xlog writes that the sequence
			 * server might do.
			 */
			if (funcid == F_NEXTVAL || funcid == F_CURRVAL ||
				funcid == F_SETVAL_REGCLASS_INT8)
			{
				/* ExecutorMarkTransactionUsesSequences(); */
				is_seq_func = true;
			}

			if (funcform->provolatile == PROVOLATILE_IMMUTABLE)
				 /* okay */ ;
			else if (funcform->provolatile == PROVOLATILE_STABLE)
				 /* okay */ ;
			else if (context->single_row_insert && is_seq_func)
				;				/* Volatile, but special sequence function */
			else
				return (Node *) newexpr;

			/*
			 * Ok, we have a function that is STABLE (or IMMUTABLE), with
			 * constant args. Let's try to evaluate it.
			 */

			/*
			 * To use the executor, we need an EState.
			 */
			estate = CreateExecutorState();

			/* We can use the estate's working context to avoid memory leaks. */
			oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

			/*
			 * Prepare expr for execution.
			 */
			exprstate = ExecPrepareExpr((Expr *) newexpr, estate);

			/*
			 * And evaluate it.
			 *
			 * It is OK to use a default econtext because none of the
			 * ExecEvalExpr() code used in this situation will use econtext.
			 * That might seem fortuitous, but it's not so unreasonable --- a
			 * constant expression does not depend on context, by definition,
			 * n'est-ce pas?
			 */
			const_val = ExecEvalExprSwitchContext(exprstate,
												  GetPerTupleExprContext(estate),
												  &const_is_null);

			/* Get info needed about result datatype */
			get_typlenbyval(expr->funcresulttype, &resultTypLen, &resultTypByVal);

			/* Get back to outer memory context */
			MemoryContextSwitchTo(oldcontext);

			/* Must copy result out of sub-context used by expression eval */
			if (!const_is_null)
				const_val = datumCopy(const_val, resultTypByVal, resultTypLen);

			/* Release all the junk we just created */
			FreeExecutorState(estate);

			/*
			 * Make the constant result node.
			 */
			simple = (Expr *) makeConst(expr->funcresulttype,
										-1,
										expr->funccollid,
										resultTypLen,
										const_val, const_is_null,
										resultTypByVal);

			/* successfully simplified it */
			if (simple)
				return (Node *) simple;
		}

		/*
		 * The expression cannot be simplified any further, so build and
		 * return a replacement FuncExpr node using the possibly-simplified
		 * arguments.
		 */
		return (Node *) newexpr;
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;
		List	   *args;

		OpExpr	   *newexpr;

		/*
		 * Reduce constants in the OpExpr's arguments.  We know args is either
		 * NIL or a List node, so we can call expression_tree_mutator directly
		 * rather than recursing to self.
		 */
		args = (List *) expression_tree_mutator((Node *) expr->args,
												pre_dispatch_function_evaluation_mutator,
												(void *) context);

		/*
		 * Need to get OID of underlying function.	Okay to scribble on input
		 * to this extent.
		 */
		set_opfuncid(expr);

		newexpr = makeNode(OpExpr);
		newexpr->opno = expr->opno;
		newexpr->opfuncid = expr->opfuncid;
		newexpr->opresulttype = expr->opresulttype;
		newexpr->opretset = expr->opretset;
		newexpr->opcollid = expr->opcollid;
		newexpr->inputcollid = expr->inputcollid;
		newexpr->args = args;
		newexpr->location = expr->location;

		return (Node *) newexpr;
	}
	else if (IsA(node, CurrentOfExpr))
	{
		/*
		 * updatable cursors
		 *
		 * During constant folding, we collect the current position of each
		 * cursor mentioned in the plan into a list, and dispatch them to the
		 * PXs.
		 *
		 * We should not get here if called from planner_make_plan_constant().
		 * That is only used for simple Result plans, which should not contain
		 * CURRENT OF expressions.
		 */
		if (context->estate)
		{
			CursorPosInfo *cpos;

			cpos = makeNode(CursorPosInfo);

			/* TODO */
			/* getCurrentOf(expr, */
			/* GetPerTupleExprContext(context->estate), */
			/* expr->target_relid, */
			/* &cpos->ctid, */
			/* &cpos->_px_worker_id, */
			/* &cpos->table_oid, */
			/* &cpos->cursor_name); */

			context->cursorPositions = lappend(context->cursorPositions, cpos);
		}
	}

	/*
	 * For any node type not handled above, we recurse using
	 * plan_tree_mutator, which will copy the node unchanged but try to
	 * simplify its arguments (if any) using this routine.
	 */
	new_node = plan_tree_mutator(node,
								 pre_dispatch_function_evaluation_mutator,
								 (void *) context,
								 true);

	return new_node;
}

/*
 * Does the given expression contain Params that are passed down from
 * outer query?
 */
bool
contains_outer_params(Node *node, void *context)
{
	PlannerInfo *root = (PlannerInfo *) context;

	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXEC)
		{
			/* Does this Param refer to a value that an outer query provides? */
			PlannerInfo *parent = root->parent_root;

			while (parent)
			{
				ListCell   *lc;

				foreach(lc, parent->plan_params)
				{
					PlannerParamItem *ppi = (PlannerParamItem *) lfirst(lc);

					if (ppi->paramId == param->paramid)
						return true;	/* abort the tree traversal and return
										 * true */
				}

				parent = parent->parent_root;
			}
		}
	}
	return expression_tree_walker(node, contains_outer_params, context);
}
