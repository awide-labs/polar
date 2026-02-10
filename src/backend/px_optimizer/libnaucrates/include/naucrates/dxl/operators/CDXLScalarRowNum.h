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
//		CDXLScalarRowNum.h
//
//	@doc:
//		Class for representing DXL scalar rownum operators.
//---------------------------------------------------------------------------

#ifndef GPDXL_CDXLScalarRowNum_H
#define GPDXL_CDXLScalarRowNum_H

#include "gpos/base.h"

#include "naucrates/dxl/operators/CDXLColRef.h"
#include "naucrates/dxl/operators/CDXLScalar.h"
#include "naucrates/md/IMDId.h"

namespace gpdxl
{
using namespace gpmd;

//---------------------------------------------------------------------------
//	@class:
//		CDXLScalarRowNum
//
//	@doc:
//		Class for representing DXL scalar rownum operators.
//
//---------------------------------------------------------------------------
class CDXLScalarRowNum : public CDXLScalar
{
public:
	CDXLScalarRowNum(CDXLScalarRowNum &) = delete;

	// ctor/dtor
	CDXLScalarRowNum(CMemoryPool *mp) : CDXLScalar(mp) {}

	~CDXLScalarRowNum() override {};

	// ident accessors
	Edxlopid GetDXLOperator() const override;

	// name of the operator
	const CWStringConst *GetOpNameStr() const override;

	// serialize operator in DXL format
	void SerializeToDXL(CXMLSerializer *xml_serializer,
						const CDXLNode *node) const override;

	// conversion function
	static CDXLScalarRowNum *
	Cast(CDXLOperator *dxl_op)
	{
		GPOS_ASSERT(nullptr != dxl_op);
		GPOS_ASSERT(Edxlopid::EdxlopScalarRowNum == dxl_op->GetDXLOperator());

		return dynamic_cast<CDXLScalarRowNum *>(dxl_op);
	}

	// does the operator return a boolean result
	BOOL HasBoolResult(CMDAccessor *md_accessor GPOS_UNUSED) const override
	{
		return false;
	}

#ifdef GPOS_DEBUG
	// checks whether the operator has valid structure
	void AssertValid(const CDXLNode *node,
					 BOOL validate_children) const override;
#endif	// GPOS_DEBUG
};
}  // namespace gpdxl



#endif	// !GPDXL_CDXLScalarRowNum_H

// EOF
