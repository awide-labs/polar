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
//		CDXLPhysicalShareIndexScan.h
//
//	@doc:
//		Class for representing DXL index scan operators
//---------------------------------------------------------------------------

#ifndef GPDXL_CDXLPhysicalShareIndexScan_H
#define GPDXL_CDXLPhysicalShareIndexScan_H

#include "gpos/base.h"

#include "naucrates/dxl/operators/CDXLIndexDescr.h"
#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/dxl/operators/CDXLPhysical.h"
#include "naucrates/dxl/operators/CDXLTableDescr.h"
#include "naucrates/dxl/operators/CDXLPhysicalIndexScan.h"

namespace gpdxl
{
//---------------------------------------------------------------------------
//	@class:
//		CDXLPhysicalShareIndexScan
//
//	@doc:
//		Class for representing DXL index scan operators
//
//---------------------------------------------------------------------------
class CDXLPhysicalShareIndexScan : public CDXLPhysical
{
private:
	// table descriptor for the scanned table
	CDXLTableDescr *m_dxl_table_descr;

	// index descriptor associated with the scanned table
	CDXLIndexDescr *m_dxl_index_descr;

	// scan direction of the index
	EdxlIndexScanDirection m_index_scan_dir;

public:
	CDXLPhysicalShareIndexScan(CDXLPhysicalShareIndexScan &) = delete;

	//ctor
	CDXLPhysicalShareIndexScan(CMemoryPool *mp, CDXLTableDescr *table_descr,
						  CDXLIndexDescr *dxl_index_descr,
						  EdxlIndexScanDirection idx_scan_direction);

	//dtor
	~CDXLPhysicalShareIndexScan() override;

	// operator type
	Edxlopid GetDXLOperator() const override;

	// operator name
	const CWStringConst *GetOpNameStr() const override;

	// index descriptor
	virtual const CDXLIndexDescr *GetDXLIndexDescr() const;

	//table descriptor
	virtual const CDXLTableDescr *GetDXLTableDescr() const;

	// scan direction
	virtual EdxlIndexScanDirection GetIndexScanDir() const;

	// serialize operator in DXL format
	void SerializeToDXL(CXMLSerializer *xml_serializer,
						const CDXLNode *node) const override;

	// conversion function
	static CDXLPhysicalShareIndexScan *
	Cast(CDXLOperator *dxl_op)
	{
		GPOS_ASSERT(nullptr != dxl_op);
		GPOS_ASSERT(EdxlopPhysicalShareIndexScan == dxl_op->GetDXLOperator());

		return dynamic_cast<CDXLPhysicalShareIndexScan *>(dxl_op);
	}

#ifdef GPOS_DEBUG
	// checks whether the operator has valid structure, i.e. number and
	// types of child nodes
	void AssertValid(const CDXLNode *, BOOL validate_children) const override;
#endif	// GPOS_DEBUG
};
}  // namespace gpdxl
#endif	// !GPDXL_CDXLPhysicalShareIndexScan_H

// EOF
