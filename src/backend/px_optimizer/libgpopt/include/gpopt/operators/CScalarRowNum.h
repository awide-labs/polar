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
//		CScalarRowNum.h
//
//	@doc:
//		Scalar column rownum
//---------------------------------------------------------------------------
#ifndef GPOPT_CScalarRowNum_H
#define GPOPT_CScalarRowNum_H

#include "gpos/base.h"

#include "gpopt/base/CDrvdProp.h"
#include "gpopt/operators/CScalar.h"

namespace gpopt
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CScalarRowNum
//
//	@doc:
//		scalar rownum operator
//
//---------------------------------------------------------------------------
class CScalarRowNum : public CScalar
{
public:
	CScalarRowNum(const CScalarRowNum &) = delete;

	// ctor
	CScalarRowNum(CMemoryPool *mp)
		: CScalar(mp)
	{
	}

	// dtor
	~CScalarRowNum() override
	{
	}

	// ident accessors
	EOperatorId
	Eopid() const override
	{
		return EOperatorId::EopScalarRowNum;
	}

	// return a string for operator name
	const CHAR *
	SzId() const override
	{
		return "CScalarRowNum";
	}

	// type of expression's result
	IMDId *
	MdidType() const override;

	// match function
	BOOL Matches(COperator *pop) const override;

	// sensitivity to order of inputs
	BOOL FInputOrderSensitive() const override
	{
		return true;
	}

	// return a copy of the operator with remapped columns
	COperator *PopCopyWithRemappedColumns(CMemoryPool *mp GPOS_UNUSED,
										  UlongToColRefMap *colref_mapping GPOS_UNUSED,
										  BOOL must_exist GPOS_UNUSED) override
	{
		return PopCopyDefault();
	}

	// derive function properties
	CFunctionProp *
	DeriveFunctionProperties(CMemoryPool *mp,
							 CExpressionHandle &exprhdl) const override
	{
		return PfpDeriveFromChildren(mp, exprhdl,
									 IMDFunction::EfsVolatile,	 // efdaDefault
									 true,	 // fHasVolatileFunctionScan
									 false	 // fScan
		);
	}

	// conversion function
	static CScalarRowNum *
	PopConvert(COperator *pop)
	{
		GPOS_ASSERT(nullptr != pop);
		GPOS_ASSERT(EOperatorId::EopScalarRowNum == pop->Eopid());

		return dynamic_cast<CScalarRowNum *>(pop);
	}
};	// class CScalarRowNum

}  // namespace gpopt


#endif	// !GPOPT_CScalarRowNum_H

// EOF
