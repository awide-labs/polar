/*-------------------------------------------------------------------------
 *
 * px_opt.h
 *	  prototypes for the ORCA query planner
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 2010-Present, Pivotal Inc
 * Portions Copyright (c) 2005-2009, Greenplum inc
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/include/optimizer/px_opt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PX_OPT_H
#define PX_OPT_H

#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "pg_config.h"

#ifdef USE_PX

extern PlannedStmt *px_optimize_query(Query *parse, ParamListInfo boundParams);

#else

/* Keep compilers quiet in case the build is without PX engine */
static PlannedStmt *
px_optimize_query(Query *parse, ParamListInfo boundParams)
{
	elog(DEBUG1, "PolarDB Parallel Execution Is Disable");
	return NULL;
}

#endif

#endif							/* PX_OPT_H */
