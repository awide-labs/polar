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
//		CScalarRowNum.cpp
//
//	@doc:
//		Implementation of scalar rownum operator
//---------------------------------------------------------------------------

#include "gpopt/operators/CScalarRowNum.h"

#include "gpos/base.h"

#include "gpopt/base/CColRefSet.h"
#include "gpopt/base/CColRefTable.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/operators/CScalarFunc.h"
#include "naucrates/md/IMDTypeInt8.h"


using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CScalarRowNum::MdidType
//
//	@doc:
//		Expression type
//
//---------------------------------------------------------------------------
IMDId *
CScalarRowNum::MdidType() const
{
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	return md_accessor->PtMDType<IMDTypeInt8>()->MDId();
}


//---------------------------------------------------------------------------
//	@function:
//		CScalarRowNum::Matches
//
//	@doc:
//		Match function on operator level
//
//---------------------------------------------------------------------------
BOOL
CScalarRowNum::Matches(COperator *pop) const
{
	return pop->Eopid() == Eopid();
}

// EOF
