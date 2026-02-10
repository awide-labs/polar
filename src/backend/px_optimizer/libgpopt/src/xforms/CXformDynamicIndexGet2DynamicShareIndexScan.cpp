//---------------------------------------------------------------------------
//
// PolarDB PX Optimizer
//
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//	@filename:
//		CXformDynamicIndexGet2DynamicShareIndexScan.cpp
//
//	@doc:
//		Implementation of transform
//---------------------------------------------------------------------------

#include "gpopt/xforms/CXformDynamicIndexGet2DynamicShareIndexScan.h"

#include "gpos/base.h"

#include "gpopt/metadata/CPartConstraint.h"
#include "gpopt/metadata/CTableDescriptor.h"
#include "gpopt/operators/CLogicalDynamicIndexGet.h"
#include "gpopt/operators/CPatternLeaf.h"
#include "gpopt/operators/CPhysicalDynamicShareIndexScan.h"

using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CXformDynamicIndexGet2DynamicShareIndexScan::CXformDynamicIndexGet2DynamicShareIndexScan
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CXformDynamicIndexGet2DynamicShareIndexScan::CXformDynamicIndexGet2DynamicShareIndexScan(
	CMemoryPool *mp)
	: CXformImplementation(
		  // pattern
		  GPOS_NEW(mp) CExpression(
			  mp, GPOS_NEW(mp) CLogicalDynamicIndexGet(mp),
			  GPOS_NEW(mp) CExpression(
				  mp, GPOS_NEW(mp) CPatternLeaf(mp))  // index lookup predicate
			  ))
{
}

CXform::EXformPromise
CXformDynamicIndexGet2DynamicShareIndexScan::Exfp(CExpressionHandle &exprhdl) const
{
	if (exprhdl.DeriveHasSubquery(0))
	{
		return CXform::ExfpNone;
	}
	return CXform::ExfpHigh;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformDynamicIndexGet2DynamicShareIndexScan::Transform
//
//	@doc:
//		Actual transformation
//
//---------------------------------------------------------------------------
void
CXformDynamicIndexGet2DynamicShareIndexScan::Transform(
	CXformContext *pxfctxt GPOS_ASSERTS_ONLY, CXformResult *pxfres GPOS_UNUSED,
	CExpression *pexpr GPOS_ASSERTS_ONLY) const
{
	GPOS_ASSERT(nullptr != pxfctxt);
	GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
	GPOS_ASSERT(FCheckPattern(pexpr));

	CLogicalDynamicIndexGet *popIndexGet =
		CLogicalDynamicIndexGet::PopConvert(pexpr->Pop());
	CMemoryPool *mp = pxfctxt->Pmp();

	// create/extract components for alternative
	CName *pname = GPOS_NEW(mp) CName(mp, popIndexGet->Name());
	GPOS_ASSERT(pname != nullptr);

	// extract components
	CExpression *pexprIndexCond = (*pexpr)[0];
	pexprIndexCond->AddRef();

	CTableDescriptor *ptabdesc = popIndexGet->Ptabdesc();
	ptabdesc->AddRef();

	CIndexDescriptor *pindexdesc = popIndexGet->Pindexdesc();
	pindexdesc->AddRef();

	CColRefArray *pdrgpcrOutput = popIndexGet->PdrgpcrOutput();
	GPOS_ASSERT(nullptr != pdrgpcrOutput);
	pdrgpcrOutput->AddRef();

	CColRef2dArray *pdrgpdrgpcrPart = popIndexGet->PdrgpdrgpcrPart();
	pdrgpdrgpcrPart->AddRef();

	COrderSpec *pos = popIndexGet->Pos();
	pos->AddRef();

	popIndexGet->GetPartitionMdids()->AddRef();
	popIndexGet->GetRootColMappingPerPart()->AddRef();

	// create alternative expression
	CExpression *pexprAlt = GPOS_NEW(mp)
		CExpression(mp,
					GPOS_NEW(mp) CPhysicalDynamicShareIndexScan(
						mp, pindexdesc, ptabdesc, pexpr->Pop()->UlOpId(), pname,
						pdrgpcrOutput, popIndexGet->ScanId(), pdrgpdrgpcrPart,
						pos, popIndexGet->GetPartitionMdids(),
						popIndexGet->GetRootColMappingPerPart()),
					pexprIndexCond);
	// add alternative to transformation result
	pxfres->Add(pexprAlt);
}


// EOF
