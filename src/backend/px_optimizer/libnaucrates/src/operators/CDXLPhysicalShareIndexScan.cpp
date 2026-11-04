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
//		CDXLPhysicalShareIndexScan.cpp
//
//	@doc:
//		Implementation of DXL physical index scan operators
//---------------------------------------------------------------------------

#include "naucrates/dxl/operators/CDXLPhysicalShareIndexScan.h"

#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/dxl/xml/CXMLSerializer.h"

using namespace gpos;
using namespace gpdxl;

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::CDXLPhysicalShareIndexScan
//
//	@doc:
//		Construct an index scan node given its table descriptor,
//		index descriptor and filter conditions on the index
//
//---------------------------------------------------------------------------
CDXLPhysicalShareIndexScan::CDXLPhysicalShareIndexScan(
	CMemoryPool *mp, CDXLTableDescr *table_descr,
	CDXLIndexDescr *dxl_index_descr, EdxlIndexScanDirection idx_scan_direction)
	: CDXLPhysical(mp),
	  m_dxl_table_descr(table_descr),
	  m_dxl_index_descr(dxl_index_descr),
	  m_index_scan_dir(idx_scan_direction)
{
	GPOS_ASSERT(nullptr != m_dxl_table_descr);
	GPOS_ASSERT(nullptr != m_dxl_index_descr);
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::~CDXLPhysicalShareIndexScan
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CDXLPhysicalShareIndexScan::~CDXLPhysicalShareIndexScan()
{
	m_dxl_index_descr->Release();
	m_dxl_table_descr->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::GetDXLOperator
//
//	@doc:
//		Operator type
//
//---------------------------------------------------------------------------
Edxlopid
CDXLPhysicalShareIndexScan::GetDXLOperator() const
{
	return EdxlopPhysicalShareIndexScan;
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::GetOpNameStr
//
//	@doc:
//		Operator name
//
//---------------------------------------------------------------------------
const CWStringConst *
CDXLPhysicalShareIndexScan::GetOpNameStr() const
{
	return CDXLTokens::GetDXLTokenStr(EdxltokenPhysicalShareIndexScan);
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::MakeDXLIndexDescr
//
//	@doc:
//		Index descriptor for the index scan
//
//---------------------------------------------------------------------------
const CDXLIndexDescr *
CDXLPhysicalShareIndexScan::GetDXLIndexDescr() const
{
	return m_dxl_index_descr;
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::GetIndexScanDir
//
//	@doc:
//		Return the scan direction of the index
//
//---------------------------------------------------------------------------
EdxlIndexScanDirection
CDXLPhysicalShareIndexScan::GetIndexScanDir() const
{
	return m_index_scan_dir;
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::GetDXLTableDescr
//
//	@doc:
//		Return the associated table descriptor
//
//---------------------------------------------------------------------------
const CDXLTableDescr *
CDXLPhysicalShareIndexScan::GetDXLTableDescr() const
{
	return m_dxl_table_descr;
}

//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::SerializeToDXL
//
//	@doc:
//		Serialize operator in DXL format
//
//---------------------------------------------------------------------------
void
CDXLPhysicalShareIndexScan::SerializeToDXL(CXMLSerializer *xml_serializer,
									  const CDXLNode *node) const
{
	const CWStringConst *element_name = GetOpNameStr();

	xml_serializer->OpenElement(
		CDXLTokens::GetDXLTokenStr(EdxltokenNamespacePrefix), element_name);
	xml_serializer->AddAttribute(
		CDXLTokens::GetDXLTokenStr(EdxltokenIndexScanDirection),
		CDXLOperator::GetIdxScanDirectionStr(m_index_scan_dir));

	// serialize properties
	node->SerializePropertiesToDXL(xml_serializer);

	// serialize children
	node->SerializeChildrenToDXL(xml_serializer);

	// serialize index descriptor
	m_dxl_index_descr->SerializeToDXL(xml_serializer);

	// serialize table descriptor
	m_dxl_table_descr->SerializeToDXL(xml_serializer);

	xml_serializer->CloseElement(
		CDXLTokens::GetDXLTokenStr(EdxltokenNamespacePrefix), element_name);
}

#ifdef GPOS_DEBUG
//---------------------------------------------------------------------------
//	@function:
//		CDXLPhysicalShareIndexScan::AssertValid
//
//	@doc:
//		Checks whether operator node is well-structured
//
//---------------------------------------------------------------------------
void
CDXLPhysicalShareIndexScan::AssertValid(const CDXLNode *node,
								   BOOL validate_children) const
{
	// assert proj list and filter are valid
	CDXLPhysical::AssertValid(node, validate_children);

	// index scan has only 3 children
	GPOS_ASSERT(3 == node->Arity());

	// assert validity of the index descriptor
	GPOS_ASSERT(nullptr != m_dxl_index_descr);
	GPOS_ASSERT(nullptr != m_dxl_index_descr->MdName());
	GPOS_ASSERT(m_dxl_index_descr->MdName()->GetMDName()->IsValid());

	// assert validity of the table descriptor
	GPOS_ASSERT(nullptr != m_dxl_table_descr);
	GPOS_ASSERT(nullptr != m_dxl_table_descr->MdName());
	GPOS_ASSERT(m_dxl_table_descr->MdName()->GetMDName()->IsValid());

	CDXLNode *index_cond_dxlnode = (*node)[EdxlisIndexCondition];

	// assert children are of right type (physical/scalar)
	GPOS_ASSERT(EdxlopScalarIndexCondList ==
				index_cond_dxlnode->GetOperator()->GetDXLOperator());

	if (validate_children)
	{
		index_cond_dxlnode->GetOperator()->AssertValid(index_cond_dxlnode,
													   validate_children);
	}
}
#endif	// GPOS_DEBUG

// EOF
