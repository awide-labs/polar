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
//		CXformDynamicIndexGet2DynamicShareIndexScan.h
//
//	@doc:
//		Transform DynamicIndexGet to DynamicShareIndexScan
//---------------------------------------------------------------------------
#ifndef GPOPT_CXformDynamicIndexGet2DynamicShareIndexScan_H
#define GPOPT_CXformDynamicIndexGet2DynamicShareIndexScan_H

#include "gpos/base.h"

#include "gpopt/xforms/CXformImplementation.h"

namespace gpopt
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CXformDynamicIndexGet2DynamicShareIndexScan
//
//	@doc:
//		Transform DynamicIndexGet to DynamicShareIndexScan
//
//---------------------------------------------------------------------------
class CXformDynamicIndexGet2DynamicShareIndexScan : public CXformImplementation
{
private:
public:
	CXformDynamicIndexGet2DynamicShareIndexScan(
		const CXformDynamicIndexGet2DynamicShareIndexScan &) = delete;

	// ctor
	explicit CXformDynamicIndexGet2DynamicShareIndexScan(CMemoryPool *mp);

	// dtor
	~CXformDynamicIndexGet2DynamicShareIndexScan() override = default;

	// ident accessors
	EXformId
	Exfid() const override
	{
		return ExfDynamicIndexGet2DynamicShareIndexScan;
	}

	// return a string for xform name
	const CHAR *
	SzId() const override
	{
		return "CXformDynamicIndexGet2DynamicShareIndexScan";
	}

	// compute xform promise for a given expression handle
	EXformPromise Exfp(CExpressionHandle &exprhdl) const override;

	// actual transform
	void Transform(CXformContext *pxfctxt, CXformResult *pxfres,
				   CExpression *pexpr) const override;

};	// class CXformDynamicIndexGet2DynamicShareIndexScan

}  // namespace gpopt


#endif	// !GPOPT_CXformDynamicIndexGet2DynamicShareIndexScan_H

// EOF
