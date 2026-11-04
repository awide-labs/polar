#include <pg_config.h>

#include "postgres.h"

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "common/relpath.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "storage/bufmgr.h"
#include "utils/elog.h"

#include "px/px_mutate.h"
#include "px/px_plan.h"
#include "px/px_hash.h"
#include "px/px_snapshot.h"
#include "catalog/px_policy.h"
/*  #include "px/px_gang.h" */
#include "optimizer/px_walkers.h"

PG_MODULE_MAGIC;

#ifdef FAULT_INJECTOR
typedef struct
{
	plan_tree_base_prefix base; /* Required prefix for
								 * plan_tree_walker/mutator */
	EState	   *estate;
	bool		single_row_insert;
	List	   *cursorPositions;
} pre_dispatch_function_evaluation_context;

typedef enum
{
	PQTRANS_IDLE,				/* connection idle */
	PQTRANS_ACTIVE,				/* command in progress */
	PQTRANS_INTRANS,			/* idle, within transaction block */
	PQTRANS_INERROR,			/* idle, within failed transaction */
	PQTRANS_UNKNOWN				/* cannot determine status */
} PGTransactionStatusType;

extern Node *(*test_pre_dispatch_function_evaluation_mutator) (Node *node,
															   pre_dispatch_function_evaluation_context *context);

static void
test_lock_rows(void)
{
	Node	   *new_node = NULL;
	LockRows   *lockrows = makeNode(LockRows);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) lockrows,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_repeat(void)
{
	Node	   *new_node = NULL;
	Repeat	   *repeat = makeNode(Repeat);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) repeat,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_append(void)
{
	Node	   *new_node = NULL;
	Append	   *append = makeNode(Append);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) append,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_mergeappend(void)
{
	Node	   *new_node = NULL;
	MergeAppend *merge = makeNode(MergeAppend);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) merge,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_recursiveunion(void)
{
	Node	   *new_node = NULL;
	RecursiveUnion *ru = makeNode(RecursiveUnion);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) ru,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_sequnce(void)
{
	Node	   *new_node = NULL;
	Sequence   *sequence = makeNode(Sequence);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) sequence,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_bitmapand(void)
{
	Node	   *new_node = NULL;
	BitmapAnd  *old = makeNode(BitmapAnd);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) old,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_bitmapor(void)
{
	Node	   *new_node = NULL;
	BitmapOr   *old = makeNode(BitmapOr);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) old,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_scan(void)
{
	Node	   *new_node = NULL;
	Scan	   *scan = makeNode(Scan);
	pre_dispatch_function_evaluation_context pcontext;

	/*
	 * We need the try-catch because plan_tree_mutator with T_Scan will
	 * repport an error. We don't want this test case stop.
	 */
	PG_TRY();
	{
		new_node = plan_tree_mutator((Node *) scan,
									 (*test_pre_dispatch_function_evaluation_mutator),
									 (void *) &pcontext,
									 true);
	}
	PG_CATCH();
	{
		elog(WARNING, "Inject error test_scan");
	}
	PG_END_TRY();
	Assert(new_node == NULL);
}

static void
test_samplescan(void)
{
	Node	   *new_node = NULL;
	SampleScan *samplescan = makeNode(SampleScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) samplescan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_customscan(void)
{
	Node	   *new_node = NULL;
	CustomScan *cscan = makeNode(CustomScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) cscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_indexonlyscan(void)
{
	Node	   *new_node = NULL;
	IndexOnlyScan *idxonlyscan = makeNode(IndexOnlyScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) idxonlyscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_bitmapindexscan(void)
{
	Node	   *new_node = NULL;
	BitmapIndexScan *idxscan = makeNode(BitmapIndexScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) idxscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_bitmapheapscan(void)
{
	Node	   *new_node = NULL;
	BitmapHeapScan *bmheapscan = makeNode(BitmapHeapScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) bmheapscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_tidscan(void)
{
	Node	   *new_node = NULL;
	TidScan    *tidscan = makeNode(TidScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) tidscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_subqueryscan(void)
{
	Node	   *new_node = NULL;
	SubqueryScan *sqscan = makeNode(SubqueryScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) sqscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_valuesscan(void)
{
	Node	   *new_node = NULL;
	ValuesScan *scan = makeNode(ValuesScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) scan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_worktablescan(void)
{
	Node	   *new_node = NULL;
	WorkTableScan *wts = makeNode(WorkTableScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) wts,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_join(void)
{
	Node	   *new_node = NULL;
	Join	   *join = makeNode(Join);
	pre_dispatch_function_evaluation_context pcontext;

	/*
	 * We need the try-catch because plan_tree_mutator with T_Join will
	 * repport an error. We don't want this test case stop.
	 */
	PG_TRY();
	{
		new_node = plan_tree_mutator((Node *) join,
									 (*test_pre_dispatch_function_evaluation_mutator),
									 (void *) &pcontext,
									 true);
	}
	PG_CATCH();
	{
		elog(WARNING, "Inject error test_join");
	}
	PG_END_TRY();
	Assert(new_node == NULL);
}

static void
test_tablefuncscan(void)
{
	Node	   *new_node = NULL;
	TableFuncScan *tabfunc = makeNode(TableFuncScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) tabfunc,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_unique(void)
{
	Node	   *new_node = NULL;
	Unique	   *uniq = makeNode(Unique);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) uniq,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_gather(void)
{
	Node	   *new_node = NULL;
	Gather	   *gather = makeNode(Gather);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) gather,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_setop(void)
{
	Node	   *new_node = NULL;
	SetOp	   *setop = makeNode(SetOp);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) setop,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_flow(void)
{
	Node	   *new_node = NULL;
	Flow	   *flow = makeNode(Flow);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) flow,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_query(void)
{
	Node	   *new_node = NULL;
	Query	   *query = makeNode(Query);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) query,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_subplan(void)
{
	Node	   *new_node = NULL;
	SubPlan    *subplan = makeNode(SubPlan);
	pre_dispatch_function_evaluation_context *pcontext = NULL;

	new_node = plan_tree_mutator((Node *) subplan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) pcontext,
								 true);
	Assert(new_node != NULL);
}

static void
test_rangetblentry(void)
{
	Node	   *new_node = NULL;
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	pre_dispatch_function_evaluation_context pcontext;

	rte->ctename = (char *) calloc(10, sizeof(char));
	strcpy(rte->ctename, "test");
	rte->ctelevelsup = 1;
	rte->self_reference = false;

	/* RTE_SUBQUERY */
	rte->rtekind = RTE_SUBQUERY;
	new_node = plan_tree_mutator((Node *) rte,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
	new_node = NULL;

	/* RTE_CTE */
	rte->rtekind = RTE_CTE;
	new_node = plan_tree_mutator((Node *) rte,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
	new_node = NULL;

	/* RTE_JOIN */
	rte->rtekind = RTE_JOIN;
	new_node = plan_tree_mutator((Node *) rte,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
	new_node = NULL;

	/* RTE_FUNCTION */
	rte->rtekind = RTE_FUNCTION;
	new_node = plan_tree_mutator((Node *) rte,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
	new_node = NULL;

	/* RTE_TABLEFUNCTION */
	rte->rtekind = RTE_TABLEFUNCTION;
	new_node = plan_tree_mutator((Node *) rte,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
	new_node = NULL;

	/* RTE_VALUES */
	rte->rtekind = RTE_VALUES;
	new_node = plan_tree_mutator((Node *) rte,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
	free(rte->ctename);
}

static void
test_rangetblfunction(void)
{
	Node	   *new_node = NULL;
	RangeTblFunction *rtfunc = makeNode(RangeTblFunction);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) rtfunc,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node == NULL);
}

static void
test_foreignscan(void)
{
	Node	   *new_node = NULL;
	ForeignScan *fdwscan = makeNode(ForeignScan);
	pre_dispatch_function_evaluation_context pcontext;

	new_node = plan_tree_mutator((Node *) fdwscan,
								 (*test_pre_dispatch_function_evaluation_mutator),
								 (void *) &pcontext,
								 true);
	Assert(new_node != NULL);
}

/* coverage for px_plan.c */
static void
test_plan_tree_mutator(void)
{
	test_lock_rows();
	test_repeat();
	test_append();
	test_mergeappend();
	test_recursiveunion();
	test_sequnce();
	test_bitmapand();
	test_bitmapor();
	test_scan();
	test_samplescan();
	test_customscan();
	test_indexonlyscan();
	test_bitmapindexscan();
	test_bitmapheapscan();
	test_tidscan();
	test_subqueryscan();
	test_valuesscan();
	test_worktablescan();
	test_join();
	test_tablefuncscan();
	test_unique();
	test_gather();
	test_setop();
	test_flow();
	test_query();
	test_subplan();
	test_rangetblentry();
	test_rangetblfunction();
	test_foreignscan();
}

/* coverage for px_cat.c */
static void
test_px_policy(void)
{
	PxPolicy   *policy = NULL;

	policy = makePxPolicy(POLICYTYPE_PARTITIONED, 3, 1);
	Assert(policy != NULL);
}

static void
test_px_hash_init(void)
{
	PxHash	   *h = NULL;

	h = (PxHash *) palloc0(sizeof(PxHash));
	h->is_legacy_hash = true;
	pxhashinit(h);
	pfree(h);
}

static void
test_px_hash_reduce(void)
{
	PxHash	   *h = NULL;
	int			result = 0;

	h = (PxHash *) palloc0(sizeof(PxHash));

	/* case REDUCE_BITMASK */
	h->reducealg = REDUCE_BITMASK;
	h->hash = 1;
	h->numsegs = 3;
	h->natts = 1;
	result = pxhashreduce(h);
	elog(WARNING, "pxhashreduce result %d", result);

	/* case REDUCE_LAZYMOD */
	h->reducealg = REDUCE_LAZYMOD;
	h->hash = 1;
	h->numsegs = 3;
	h->natts = 1;
	result = pxhashreduce(h);
	elog(WARNING, "pxhashreduce result %d", result);
	pfree(h);
}

/* coverage for px_hash.c */
static void
test_px_hash(void)
{
	test_px_hash_init();
	test_px_hash_reduce();
}

static void
test_contains_outer_params(void)
{
	Node	   *new_node = NULL;
	Param	   *param = NULL;
	PlannerInfo *root = NULL;
	PlannerParamItem *param_item = NULL;
	bool		result = false;

	/* test 1 */
	result = contains_outer_params(new_node, NULL);
	Assert(result == false);

	/* test 2 */
	param = makeNode(Param);
	param->paramid = 1;
	param->paramkind = PARAM_EXEC;

	param_item = (PlannerParamItem *) palloc0(sizeof(PlannerParamItem));
	param_item->paramId = 1;

	root = (PlannerInfo *) palloc0(sizeof(PlannerInfo));
	root->parent_root = (PlannerInfo *) palloc0(sizeof(PlannerInfo));
	root->parent_root->plan_params = lappend(
											 root->parent_root->plan_params, (void *) param_item
		);
	result = contains_outer_params((Node *) param, root);
	pfree(param);
	pfree(param_item);
	list_free(root->parent_root->plan_params);
	pfree(root->parent_root);
	pfree(root);

	/* test 3 */
	param = makeNode(Param);
	param->paramkind = PARAM_EXEC;
	root = (PlannerInfo *) palloc0(sizeof(PlannerInfo));
	root->parent_root = NULL;
	result = contains_outer_params((Node *) param, root);
	pfree(param);
	pfree(root);
}

/* coverage for px_mutate.c */
static void
test_px_mutate(void)
{
	test_contains_outer_params();
}


PG_FUNCTION_INFO_V1(test_px);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_px(PG_FUNCTION_ARGS)
{
	if (!polar_enable_shared_storage_mode)
		PG_RETURN_VOID();

	/* TODO: consider to remove it as useless */
	elog(WARNING, "test_plan_tree_mutator start");
	test_plan_tree_mutator();

	elog(WARNING, "test_px_policy");
	test_px_policy();

	elog(WARNING, "test_px_hash");
	test_px_hash();

	elog(WARNING, "test_px_mutate");
	test_px_mutate();

	PG_RETURN_VOID();
}

#else
PG_FUNCTION_INFO_V1(test_px);
Datum
test_px(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}
#endif							/* FAULT_INJECTOR */
