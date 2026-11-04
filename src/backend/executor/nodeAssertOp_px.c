/*-------------------------------------------------------------------------
 *
 * nodeAssertOp_px.c
 *	  Implementation of nodeAssertOp_px.
 *
 * Portions Copyright (c) 2012, EMC Corp.
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/executor/nodeAssertOp_px.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeAssertOp_px.h"

/* memory used by node.*/
#define ASSERTOP_MEM 	1

/*
 * Estimated Memory Usage of AssertOp Node.
 **/
void
ExecAssertOpExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
	planstate->instrument->execmemused += ASSERTOP_MEM;
}

/*
 * Check for assert violations and error out, if any.
 */
static void
CheckForAssertViolations(AssertOpState * node, TupleTableSlot *slot)
{
	AssertOp   *plannode;
	ExprContext *econtext;
	ExprState  *state;
	int			violationCount = 0;
	bool		isNull = false;
	MemoryContext oldContext;
	Datum		expr_value;
	StringInfoData errorString;

	plannode = (AssertOp *) node->ps.plan;
	state = node->ps.qual;
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_outertuple = slot;

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* verify that expression was compiled using ExecInitQual */
	Assert(state->flags & EEO_FLAG_IS_QUAL);

	initStringInfo(&errorString);

	expr_value = ExecEvalExprSwitchContext(state, econtext, &isNull);

	if (!isNull && !DatumGetBool(expr_value))
	{
		/*
		 * FIXME: how to get the real assertion failure item index? Currently
		 * show the first error message ([0]) directly.
		 */
		String	   *valErrorMessage = (String *) list_nth(plannode->errmessage, 0);

		Assert(NULL != valErrorMessage && IsA(valErrorMessage, String) &&
			   0 < strlen(strVal(valErrorMessage)));

		appendStringInfo(&errorString, "%s\n", strVal(valErrorMessage));
		violationCount++;
	}

	if (0 < violationCount)
	{
		ereport(ERROR,
				(errcode(plannode->errcode),
				 errmsg("one or more assertions failed"),
				 errdetail("%s", errorString.data)));
	}
	pfree(errorString.data);
	MemoryContextSwitchTo(oldContext);
	ResetExprContext(econtext);
}

/*
 * Evaluate Constraints (in node->ps.qual) and project output TupleTableSlot.
 * */
TupleTableSlot *
ExecAssertOp(AssertOpState * node)
{
	PlanState  *outerNode = outerPlanState(node);
	TupleTableSlot *slot = ExecProcNode(outerNode);

	if (TupIsNull(slot))
	{
		return NULL;
	}

	CheckForAssertViolations(node, slot);

	return ExecProject(node->ps.ps_ProjInfo);
}

/**
 * Init AssertOp, which sets the ProjectInfo and
 * the Constraints to evaluate.
 * */
AssertOpState *
ExecInitAssertOp(AssertOp *node, EState *estate, int eflags)
{
	AssertOpState *assertOpState;
	Plan	   *outerPlan;

	/* Check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
	Assert(outerPlan(node) != NULL);

	assertOpState = makeNode(AssertOpState);
	assertOpState->ps.plan = (Plan *) node;
	assertOpState->ps.state = estate;
	assertOpState->ps.ExecProcNode = (ExecProcNodeMtd) ExecAssertOp;

	ExecInitResultTupleSlotTL(&assertOpState->ps, &TTSOpsVirtual);

	/* Create expression evaluation context */
	ExecAssignExprContext(estate, &assertOpState->ps);

	/*
	 * Initialize outer plan
	 */
	outerPlan = outerPlan(node);
	outerPlanState(assertOpState) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * Initialize result type and projection.
	 */
	ExecAssignProjectionInfo(&assertOpState->ps, NULL);

	/*
	 * Initialize qual ExprState.
	 */
	assertOpState->ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) assertOpState);

	if (estate->es_instrument &&
		(estate->es_instrument & INSTRUMENT_PX /* INSTRUMENT_OPERATION */ ))
	{
		assertOpState->ps.pxexplainbuf = makeStringInfo();

		/* Request a callback at end of query. */
		assertOpState->ps.pxexplainfun = ExecAssertOpExplainEnd;
	}

	return assertOpState;
}

/* Rescan AssertOp */
void
ExecReScanAssertOp(AssertOpState * node)
{
	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlanState(node) &&
		outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

/* Release Resources Requested by AssertOp node. */
void
ExecEndAssertOp(AssertOpState * node)
{
	ExecFreeExprContext(&node->ps);
	ExecEndNode(outerPlanState(node));
}
