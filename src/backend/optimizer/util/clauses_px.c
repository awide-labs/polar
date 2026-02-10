/*-------------------------------------------------------------------------
 *
 * clauses_px.c
 *	  routines to manipulate qualification clauses
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/clauses_px.c
 *
 *-------------------------------------------------------------------------
 */
#include "optimizer/cost.h"
#include "px/px_vars.h"

/*
 * Default configuration information
 */
static PlannerConfig *
DefaultPlannerConfig(void)
{
	PlannerConfig *c1 = (PlannerConfig *) palloc(sizeof(PlannerConfig));

	c1->enable_sort = enable_sort;
	c1->enable_hashagg = enable_hashagg;
	c1->enable_groupagg = enable_groupagg;
	c1->enable_nestloop = enable_nestloop;
	c1->enable_mergejoin = enable_mergejoin;
	c1->enable_hashjoin = enable_hashjoin;
	c1->px_enable_hashjoin_size_heuristic = px_enable_hashjoin_size_heuristic;
	c1->px_enable_predicate_propagation = px_enable_predicate_propagation;
	c1->constraint_exclusion = constraint_exclusion;

	c1->px_enable_minmax_optimization = px_enable_minmax_optimization;
	c1->px_enable_multiphase_agg = px_enable_multiphase_agg;
	c1->px_enable_preunique = px_enable_preunique;
	c1->px_eager_preunique = px_eager_preunique;
	c1->px_hashagg_streambottom = px_hashagg_streambottom;
	c1->px_enable_agg_distinct = px_enable_agg_distinct;
	c1->px_enable_dqa_pruning = px_enable_dqa_pruning;
	c1->px_eager_dqa_pruning = px_eager_dqa_pruning;
	c1->px_eager_one_phase_agg = px_eager_one_phase_agg;
	c1->px_eager_two_phase_agg = px_eager_two_phase_agg;
	c1->px_enable_sort_distinct = px_enable_sort_distinct;

	c1->px_enable_direct_dispatch = px_enable_direct_dispatch;

	c1->px_cte_sharing = px_cte_sharing;

	c1->honor_order_by = true;

	c1->is_under_subplan = false;

	return c1;
}

/*
 * flatten_join_alias_var_optimizer
 *	  Replace Vars that reference JOIN outputs with references to the original
 *	  relation variables instead.
 */
Query *
flatten_join_alias_var_optimizer(Query *query, int queryLevel)
{
	Query	   *queryNew = (Query *) copyObject(query);
	ListCell   *plc = NULL;
	List	   *targetList = queryNew->targetList;
	List	   *returningList = queryNew->returningList;
	Node	   *havingQual = queryNew->havingQual;
	Node	   *limitOffset = queryNew->limitOffset;
	List	   *windowClause = queryNew->windowClause;
	Node	   *limitCount = queryNew->limitCount;

	/* Create a PlannerInfo data structure for this subquery */
	PlannerInfo *root = makeNode(PlannerInfo);

	root->parse = queryNew;
	root->query_level = queryLevel;

	root->glob = makeNode(PlannerGlobal);
	root->glob->boundParams = NULL;
	root->glob->subplans = NIL;
	root->glob->subroots = NIL;
	root->glob->finalrtable = NIL;
	root->glob->relationOids = NIL;
	root->glob->invalItems = NIL;
	root->glob->nParamExec = 0;
	root->glob->transientPlan = false;
	root->glob->nParamExec = 0;

	root->config = DefaultPlannerConfig();

	root->parent_root = NULL;
	root->planner_cxt = CurrentMemoryContext;
	root->init_plans = NIL;

	root->list_cteplaninfo = NIL;
	root->join_info_list = NIL;
	root->append_rel_list = NIL;

	root->hasJoinRTEs = false;

	foreach(plc, queryNew->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(plc);

		if (rte->rtekind == RTE_JOIN)
		{
			root->hasJoinRTEs = true;
			if (IS_OUTER_JOIN(rte->jointype))
			{
				break;
			}
		}
	}

	/*----------
	 * Flatten join alias for expression in
	 * 1. targetlist
	 * 2. returningList
	 * 3. having qual
	 * 4. scatterClause
	 * 5. limit offset
	 * 6. limit count
	 *
	 * We flatten the above expressions since these entries may be moved
	 * during the query normalization step before algebrization. In contrast,
	 * the planner flattens alias inside quals to allow predicates involving
	 * such vars to be pushed down.
	 *
	 * Here we ignore the flattening of quals due to the following reasons:
	 * 1. we assume that the function will be called before Query->DXL
	 * translation.
	 * 2. the quals never gets moved from old query to the new top-level query
	 * in the query normalization phase before algebrization. In other words,
	 * the quals hang of the same query structure that is now the new derived
	 * table.
	 * 3. the algebrizer can resolve the abiquity of join aliases in quals since
	 * we maintain all combinations of <query level, varno, varattno> to
	 * DXL-ColId during Query->DXL translation.
	 *----------
	 */

	if (NIL != targetList)
	{
		queryNew->targetList = (List *) flatten_join_alias_vars(root->parse, (Node *) targetList);
		pfree(targetList);
	}

	if (NIL != returningList)
	{
		queryNew->returningList = (List *) flatten_join_alias_vars(root->parse, (Node *) returningList);
		pfree(returningList);
	}

	if (NULL != havingQual)
	{
		queryNew->havingQual = flatten_join_alias_vars(root->parse, havingQual);
		pfree(havingQual);
	}

	if (NULL != limitOffset)
	{
		queryNew->limitOffset = flatten_join_alias_vars(root->parse, limitOffset);
		pfree(limitOffset);
	}

	if (NIL != queryNew->windowClause)
	{
		ListCell   *l;

		foreach(l, windowClause)
		{
			WindowClause *wc = (WindowClause *) lfirst(l);

			if (wc == NULL)
				continue;

			if (wc->startOffset)
				wc->startOffset = flatten_join_alias_vars(root->parse, wc->startOffset);

			if (wc->endOffset)
				wc->endOffset = flatten_join_alias_vars(root->parse, wc->endOffset);
		}
	}

	if (NULL != limitCount)
	{
		queryNew->limitCount = flatten_join_alias_vars(root->parse, limitCount);
		pfree(limitCount);
	}

	return queryNew;
}

/*
 * fold_constants
 *
 * Recurses into query tree and folds all constant expressions.
 */
Query *
fold_constants(PlannerInfo *root, Query *q, ParamListInfo boundParams, Size max_size)
{
	eval_const_expressions_context context;

	context.root = root;
	context.boundParams = boundParams;
	context.active_fns = NIL;	/* nothing being recursively simplified */
	context.case_val = NULL;	/* no CASE being examined */
	context.estimate = false;	/* safe transformations only */

	return (Query *) query_or_expression_tree_mutator
		(
		 (Node *) q,
		 eval_const_expressions_mutator,
		 &context,
		 0
		);
}

/*
 * POLAR px
 * Transform a small array constant to an ArrayExpr.
 *
 * This is used by PXOPT, to transform the array argument of a ScalarArrayExpr
 * into an ArrayExpr. If a ScalarArrayExpr has an ArrayExpr argument, PXOPT can
 * perform some optimizations - partition pruning at least - by first expanding
 * the ArrayExpr into its disjunctive normal form and then deriving constraints
 * based on the elements in the ArrayExpr. It doesn't currently know how to
 * extract elements from an Array const, however, so to enable those
 * optimizations in PXOPT, we convert Array Consts into corresponding
 * ArrayExprs.
 *
 * If the argument is not an array constant return the original Const unmodified.
 * We convert an array const of any size to ArrayExpr. PXOPT can use it to derive
 * statistics.
 */
Expr *
transform_array_Const_to_ArrayExpr(Const *c)
{
	Oid			elemtype;
	int16		elemlen;
	bool		elembyval;
	char		elemalign;
	int			nelems;
	Datum	   *elems;
	bool	   *nulls;
	ArrayType  *ac;
	ArrayExpr  *aexpr;
	int			i;

	Assert(IsA(c, Const));

	/* Does it look like the right kind of an array Const? */
	if (c->constisnull)
		return (Expr *) c;		/* NULL const */

	elemtype = get_element_type(c->consttype);
	if (elemtype == InvalidOid)
		return (Expr *) c;		/* not an array */

	ac = DatumGetArrayTypeP(c->constvalue);
	nelems = ArrayGetNItems(ARR_NDIM(ac), ARR_DIMS(ac));

	/* All set, extract the elements, and an ArrayExpr to hold them. */
	get_typlenbyvalalign(elemtype, &elemlen, &elembyval, &elemalign);
	deconstruct_array(ac, elemtype, elemlen, elembyval, elemalign,
					  &elems, &nulls, &nelems);

	aexpr = makeNode(ArrayExpr);
	aexpr->array_typeid = c->consttype;
	aexpr->element_typeid = elemtype;
	aexpr->multidims = false;
	aexpr->location = c->location;

	for (i = 0; i < nelems; i++)
	{
		aexpr->elements = lappend(aexpr->elements,
								  makeConst(elemtype,
											-1,
											c->constcollid,
											elemlen,
											elems[i],
											nulls[i],
											elembyval));
	}

	return (Expr *) aexpr;
}
