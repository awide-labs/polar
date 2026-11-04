/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2012 Greenplum, Inc.
*	Copyright (C) 2021, Alibaba Group Holding Limited
*
*	@filename:
*		CGPOptimizer.cpp
*
*	@doc:
*		Entry point to GP optimizer
*
*	@test:
*
*-------------------------------------------------------------------------
*/

#include "px_optimizer_util/CGPOptimizer.h"
#include "px_optimizer_util/utils/COptTasks.h"
#include "px_optimizer_util/utils/CMemoryPoolPalloc.h"
#include "px_optimizer_util/utils/CMemoryPoolPallocManager.h"

// the following headers are needed to reference optimizer library initializers
#include "naucrates/init.h"
#include "gpopt/init.h"
#include "gpos/_api.h"
#include "px_optimizer_util/px_wrappers.h"
#include "gpos/memory/CMemoryPoolManager.h"

#include "naucrates/exception.h"
#include "utils/guc.h"
#include "utils/memutils.h"

extern MemoryContext MessageContext;

/*-------------------------------------------------------------------------
*	@function:
*		CGPOptimizer::PlstmtOptimize
*
*	@doc:
*		Optimize given query using GP optimizer
*
*-------------------------------------------------------------------------
*/
PlannedStmt *
CGPOptimizer::PXOPTOptimizedPlan
	(
	Query *query,
	bool *had_unexpected_failure // output : set to true if optimizer unexpectedly failed to produce plan
	)
{
	SOptContext gpopt_context;
	PlannedStmt* plStmt = NULL;

	*had_unexpected_failure = false;

	GPOS_TRY
	{
		plStmt = COptTasks::PXOPTOptimizedPlan(query, &gpopt_context);
		// clean up context
		gpopt_context.Free(gpopt_context.epinQuery, gpopt_context.epinPlStmt);
	}
	GPOS_CATCH_EX(ex)
	{
		// clone the error message before context free.
		CHAR* serialized_error_msg = gpopt_context.CloneErrorMsg(MessageContext);
		// clean up context
		gpopt_context.Free(gpopt_context.epinQuery, gpopt_context.epinPlStmt);

		// Special handler for a few common user-facing errors. In particular,
		// we want to use the correct error code for these, in case an application
		// tries to do something smart with them. Also, ERRCODE_INTERNAL_ERROR
		// is handled specially in elog.c, and we don't want that for "normal"
		// application errors.
		if (GPOS_MATCH_EX(ex, gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLNotNullViolation))
		{
			errstart(ERROR, TEXTDOMAIN);
			errcode(ERRCODE_NOT_NULL_VIOLATION);
			errmsg("%s", serialized_error_msg);
			errfinish(ex.Filename(), ex.Line(), NULL);
		}

		else if (GPOS_MATCH_EX(ex, gpdxl::ExmaDXL, gpdxl::ExmiOptimizerError) ||
			 gpopt_context.m_should_error_out)
		{
			Assert(NULL != serialized_error_msg);
			errstart(ERROR, TEXTDOMAIN);
			errcode(ERRCODE_INTERNAL_ERROR);
			errmsg("%s", serialized_error_msg);
			errfinish(ex.Filename(), ex.Line(), NULL);
		}
		else if (GPOS_MATCH_EX(ex, gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError))
		{
			PG_RE_THROW();
		}
		else if (GPOS_MATCH_EX(ex, gpdxl::ExmaDXL, gpdxl::ExmiNoAvailableMemory))
		{
			errstart(ERROR, TEXTDOMAIN);
			errcode(ERRCODE_INTERNAL_ERROR);
			errmsg("no available memory to allocate string buffer");
			errfinish(ex.Filename(), ex.Line(), NULL);
		}
		else if (GPOS_MATCH_EX(ex, gpdxl::ExmaDXL, gpdxl::ExmiInvalidComparisonTypeCode))
		{
			errstart(ERROR, TEXTDOMAIN);
			errcode(ERRCODE_INTERNAL_ERROR);
			errmsg("invalid comparison type code. Valid values are Eq, NEq, LT, LEq, GT, GEq.");
			errfinish(ex.Filename(), ex.Line(), NULL);
		}

		// Failed to produce a plan, but it wasn't an error that should
		// be propagated to the user. Log the failure if needed, and
		// return without a plan. The caller should fall back to the
		// Postgres planner.

		if (px_optimizer_trace_fallback)
		{
			errstart(INFO, TEXTDOMAIN);
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			errmsg("PXOPT failed to produce a plan, falling back to planner");
			if (serialized_error_msg)
				errdetail("%s", serialized_error_msg);
			errfinish(ex.Filename(), ex.Line(), NULL);
		}

		*had_unexpected_failure = gpopt_context.m_is_unexpected_failure;

		if (serialized_error_msg)
			pfree(serialized_error_msg);
	}
	GPOS_CATCH_END;
	return plStmt;
}


/*-------------------------------------------------------------------------
*	@function:
*		CGPOptimizer::SerializeDXLPlan
*
*	@doc:
*		Serialize planned statement into DXL
*
*-------------------------------------------------------------------------
*/
char *
CGPOptimizer::SerializeDXLPlan
	(
	Query *query
	)
{
	GPOS_TRY;
	{
		return COptTasks::Optimize(query);
	}
	GPOS_CATCH_EX(ex);
	{
		errstart(ERROR, TEXTDOMAIN);
		errcode(ERRCODE_INTERNAL_ERROR);
		errmsg("optimizer failed to produce plan");
		errfinish(ex.Filename(), ex.Line(), NULL);
	}
	GPOS_CATCH_END;
	return NULL;
}

/*-------------------------------------------------------------------------
*	@function:
*		InitPXOPT()
*
*	@doc:
*		Initialize GPTOPT and dependent libraries
*
*-------------------------------------------------------------------------
*/
void
CGPOptimizer::InitPXOPT ()
{

	if (px_optimizer_use_px_allocators)
	{
		CMemoryPoolPallocManager::Init();
	}

	struct gpos_init_params params = {px::IsAbortRequested};

	gpos_init(&params);
	gpdxl_init();
	gpopt_init();
}

/*-------------------------------------------------------------------------
*	@function:
*		TerminatePXOPT()
*
*	@doc:
*		Terminate PXOPT and dependent libraries
*
*-------------------------------------------------------------------------
*/
void
CGPOptimizer::TerminatePXOPT ()
{
	gpopt_terminate();
	gpdxl_terminate();
	gpos_terminate();
}

/*-------------------------------------------------------------------------
*	@function:
*		PXOPTOptimizedPlan
*
*	@doc:
*		Expose GP optimizer API to C files
*
*-------------------------------------------------------------------------
*/
extern "C"
{
PlannedStmt *PXOPTOptimizedPlan
	(
	Query *query,
	bool *had_unexpected_failure
	)
{
	return CGPOptimizer::PXOPTOptimizedPlan(query, had_unexpected_failure);
}
}

/*-------------------------------------------------------------------------
*	@function:
*		SerializeDXLPlan
*
*	@doc:
*		Serialize planned statement to DXL
*
*-------------------------------------------------------------------------
*/
extern "C"
{
char *SerializeDXLPlan
	(
	Query *query
	)
{
	return CGPOptimizer::SerializeDXLPlan(query);
}
}

/*-------------------------------------------------------------------------
*	@function:
*		InitPXOPT()
*
*	@doc:
*		Initialize GPTOPT and dependent libraries
*
*-------------------------------------------------------------------------
*/
extern "C"
{
void InitPXOPT ()
{
	GPOS_TRY
	{
		return CGPOptimizer::InitPXOPT();
	}
	GPOS_CATCH_EX(ex)
	{
		if (GPOS_MATCH_EX(ex, gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError))
		{
			PG_RE_THROW();
		}
	}
	GPOS_CATCH_END;
}
}

/*-------------------------------------------------------------------------
*	@function:
*		TerminatePXOPT()
*
*	@doc:
*		Terminate PXOPT and dependent libraries
*
*-------------------------------------------------------------------------
*/
extern "C"
{
void TerminatePXOPT ()
{
	GPOS_TRY
	{
		return CGPOptimizer::TerminatePXOPT();
	}
	GPOS_CATCH_EX(ex)
	{
		if (GPOS_MATCH_EX(ex, gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError))
		{
			PG_RE_THROW();
		}
	}
	GPOS_CATCH_END;
}
}

// EOF
