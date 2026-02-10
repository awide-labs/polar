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
//		CDXLScalarRowNum.cpp
//
//	@doc:
//		Implementation of DXL scalar rownum operators.
//---------------------------------------------------------------------------

#include "naucrates/dxl/operators/CDXLScalarRowNum.h"

#include "gpopt/mdcache/CMDAccessor.h"
#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/dxl/xml/CXMLSerializer.h"

using namespace gpos;
using namespace gpopt;
using namespace gpdxl;


//---------------------------------------------------------------------------
//	@function:
//		CDXLScalarRowNum::GetDXLOperator
//
//	@doc:
//		Operator type
//
//---------------------------------------------------------------------------
Edxlopid
CDXLScalarRowNum::GetDXLOperator() const
{
	return EdxlopScalarRowNum;
}


//---------------------------------------------------------------------------
//	@function:
//		CDXLScalarRowNum::GetOpNameStr
//
//	@doc:
//		Operator name
//
//---------------------------------------------------------------------------
const CWStringConst *
CDXLScalarRowNum::GetOpNameStr() const
{
	return CDXLTokens::GetDXLTokenStr(EdxltokenScalarRowNum);
}


//---------------------------------------------------------------------------
//	@function:
//		CDXLScalarRowNum::SerializeToDXL
//
//	@doc:
//		Serialize operator in DXL format
//
//---------------------------------------------------------------------------
void
CDXLScalarRowNum::SerializeToDXL(CXMLSerializer *xml_serializer,
								const CDXLNode *node) const
{
	const CWStringConst *element_name = GetOpNameStr();

	xml_serializer->OpenElement(
		CDXLTokens::GetDXLTokenStr(EdxltokenNamespacePrefix), element_name);

	node->SerializeChildrenToDXL(xml_serializer);

	xml_serializer->CloseElement(
		CDXLTokens::GetDXLTokenStr(EdxltokenNamespacePrefix), element_name);
}

#ifdef GPOS_DEBUG
//---------------------------------------------------------------------------
//	@function:
//		CDXLScalarRowNum::AssertValid
//
//	@doc:
//		Checks whether operator node is well-structured
//
//---------------------------------------------------------------------------
void
CDXLScalarRowNum::AssertValid(const CDXLNode *node,
							 BOOL  // validate_children
) const
{
	GPOS_ASSERT(0 == node->Arity());
}
#endif	// GPOS_DEBUG

// EOF
