/*-------------------------------------------------------------------------
 *
 * execUtils_px.h
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	    src/include/executor/execUtils_px.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _EXECUTILS_PX_H_
#define _EXECUTILS_PX_H_

#include "executor/execdesc.h"

struct EState;
struct QueryDesc;
struct PxDispatcherState;

extern SliceTable *InitSliceTable(struct EState *estate, PlannedStmt *plannedstmt);
extern ExecSlice *getCurrentSlice(struct EState *estate, int sliceIndex);
extern bool sliceRunsOnQC(ExecSlice *slice);
extern bool sliceRunsOnPX(ExecSlice *slice);
extern int	sliceCalculateNumSendingProcesses(ExecSlice *slice);

extern void AssignGangs(struct PxDispatcherState *ds, QueryDesc *queryDesc);

extern Motion *findSenderMotion(PlannedStmt *plannedstmt, int sliceIndex);
extern void ExtractParamsFromInitPlans(PlannedStmt *plannedstmt, Plan *root, EState *estate);
extern void AssignParentMotionToPlanNodes(PlannedStmt *plannedstmt);
extern bool HasMotion(PlannedStmt *plannedstmt);
extern bool HasSplit(PlannedStmt *plannedstmt);

extern void polar_fake_outer_params(JoinState *node);

#ifdef USE_ASSERT_CHECKING
struct PlannedStmt;
extern void AssertSliceTableIsValid(SliceTable *st);
#endif

#endif
