//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CXformGet2TableShareScan.cpp
//
//	@doc:
//		Implementation of transform
//---------------------------------------------------------------------------

#include "gpos/base.h"
#include "gpopt/xforms/CXformGet2TableShareScan.h"

#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CLogicalGet.h"
#include "gpopt/operators/CPhysicalTableShareScan.h"
#include "gpopt/metadata/CTableDescriptor.h"

using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CXformGet2TableShareScan::CXformGet2TableShareScan
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CXformGet2TableShareScan::CXformGet2TableShareScan
	(
	CMemoryPool *mp
	)
	:
	CXformImplementation
		(
		 // pattern
		GPOS_NEW(mp) CExpression
				(
				mp,
				GPOS_NEW(mp) CLogicalGet(mp)
				)
		)
{}

//---------------------------------------------------------------------------
//	@function:
//		CXformGet2TableShareScan::Exfp
//
//	@doc:
//		Compute promise of xform
//
//---------------------------------------------------------------------------
CXform::EXformPromise 
CXformGet2TableShareScan::Exfp
	(
	CExpressionHandle &exprhdl
	)
	const
{
	CLogicalGet *popGet = CLogicalGet::PopConvert(exprhdl.Pop());
	
	CTableDescriptor *ptabdesc = popGet->Ptabdesc();
	if (ptabdesc->IsPartitioned())
	{
		return CXform::ExfpNone;
	}
	
	return CXform::ExfpHigh;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformGet2TableShareScan::Transform
//
//	@doc:
//		Actual transformation
//
//---------------------------------------------------------------------------
void
CXformGet2TableShareScan::Transform
	(
	CXformContext *pxfctxt,
	CXformResult *pxfres,
	CExpression *pexpr
	)
	const
{
	GPOS_ASSERT(NULL != pxfctxt);
	GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
	GPOS_ASSERT(FCheckPattern(pexpr));

	CLogicalGet *popGet = CLogicalGet::PopConvert(pexpr->Pop());
	CMemoryPool *mp = pxfctxt->Pmp();

	// create/extract components for alternative
	CName *pname = GPOS_NEW(mp) CName(mp, popGet->Name());
	
	CTableDescriptor *ptabdesc = popGet->Ptabdesc();
	ptabdesc->AddRef();
	
	CColRefArray *pdrgpcrOutput = popGet->PdrgpcrOutput();
	GPOS_ASSERT(NULL != pdrgpcrOutput);

	pdrgpcrOutput->AddRef();
	
	// create alternative expression
	CExpression *pexprAlt = 
		GPOS_NEW(mp) CExpression
			(
			mp,
			GPOS_NEW(mp) CPhysicalTableShareScan(mp, pname, ptabdesc, pdrgpcrOutput)
			);
	// add alternative to transformation result
	pxfres->Add(pexprAlt);
}


// EOF

