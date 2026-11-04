/*-------------------------------------------------------------------------
 *
 * px_llize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/px_llize.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "commands/defrem.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/print.h"
#include "optimizer/pathnode.h"
#include "utils/lsyscache.h"

#include "px/px_hash.h"
#include "px/px_llize.h"
#include "px/px_mutate.h"
#include "px/px_plan.h"
#include "px/px_vars.h"


/*
 * A PlanProfile holds state for pxparallelize() and its prescan stage.
 *
 * PlanProfileSubPlanInfo holds extra information about every subplan in
 * the plan tree. There is one PlanProfileSubPlanInfo for each entry in
 * glob->subplans.
 */
typedef struct PlanProfileSubPlanInfo
{
	/*
	 * plan_id is used to identify this subplan in the overall plan tree. Same
	 * as SubPlan->plan_id.
	 */
	int			plan_id;

	bool		seen;			/* have we seen a SubPlan reference to this
								 * yet? */
	bool		initPlanParallel;	/* T = this is an Init Plan that needs to
									 * be dispatched, i.e. it contains Motions */

	/* Fields copied from SubPlan */
	bool		is_initplan;
	bool		is_multirow;
	bool		hasParParam;
	SubLinkType subLinkType;

	/* The context where we saw the SubPlan reference(s) for this. */
	Flow	   *parentFlow;
} PlanProfileSubPlanInfo;

typedef struct PlanProfile
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */

	PlannerInfo *root;			/* pxparallelize argument, root of top plan. */

	/* main plan is parallelled */
	bool		dispatchParallel;

	/* Any of the init plans is parallelled */
	bool		anyInitPlanParallel;

	/* array is indexed by plan_id (plan_id is 1-based, so 0 is unused) */
	PlanProfileSubPlanInfo *subplan_info;

	/*
	 * Working queue in prescan stage. Contains plan_ids of subplans that have
	 * been seen in SubPlan expressions, but haven't been parallelized yet.
	 */
	List	   *unvisited_subplans;

	/* working state for prescan_walker() */
	Flow	   *currentPlanFlow;	/* what is the flow of the current plan
									 * node */
} PlanProfile;

/*
 * Is the node a "subclass" of Plan?
 */
bool
is_plan_node(Node *node)
{
	NodeTag		t;

	if (node == NULL)
		return false;

	t = nodeTag(node);
	if ((t >= T_Plan_Start && t <= T_Plan_End)
		|| (t >= T_PX_Plan_Start && t <= T_PX_Plan_End))
		return true;
	return false;
}

#define SANITY_MOTION 0x1
