/*-------------------------------------------------------------------------
 *
 * nodeAssertOp_px.h
 *	  Prototypes for nodeAssertOp.
 *
 * Portions Copyright (c) 2012, EMC Corp.
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 *
 *
 * IDENTIFICATION
 *	    src/include/executor/nodeAssertOp_px.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef NODEASSERTOP_H
#define NODEASSERTOP_H

#include "nodes/execnodes.h"

extern void ExecAssertOpExplainEnd(PlanState *planstate, struct StringInfoData *buf);
extern TupleTableSlot *ExecAssertOp(AssertOpState * node);
extern AssertOpState * ExecInitAssertOp(AssertOp *node, EState *estate, int eflags);
extern void ExecEndAssertOp(AssertOpState * node);
extern void ExecReScanAssertOp(AssertOpState * node);

#endif							/* NODEASSERTOP_H */
