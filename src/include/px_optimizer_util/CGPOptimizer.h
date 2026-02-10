//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 Greenplum, Inc.
//  Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CGPOptimizer.h
//
//	@doc:
//		Entry point to GP optimizer
//
//	@test:
//
//
//---------------------------------------------------------------------------
#ifndef CGPOptimizer_H
#define CGPOptimizer_H

extern "C" {
#include "postgres.h"
}

#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"

class CGPOptimizer
{
	public:

		// optimize given query using GP optimizer
		static
		PlannedStmt *PXOPTOptimizedPlan
			(
			Query *query,
			bool *had_unexpected_failure // output : set to true if optimizer unexpectedly failed to produce plan
			);

		// serialize planned statement into DXL
		static
		char *SerializeDXLPlan(Query *query);

    // gpopt initialize and terminate
    static
    void InitPXOPT();

    static
    void TerminatePXOPT();
};

extern "C"
{

extern PlannedStmt *PXOPTOptimizedPlan(Query *query, bool *had_unexpected_failure);
extern char *SerializeDXLPlan(Query *query);
extern void InitPXOPT ();
extern void TerminatePXOPT ();

}

#endif // CGPOptimizer_H
