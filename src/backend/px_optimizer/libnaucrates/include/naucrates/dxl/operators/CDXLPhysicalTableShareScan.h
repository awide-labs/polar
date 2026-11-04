//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2010 Greenplum, Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CDXLPhysicalTableShareScan.h
//
//	@doc:
//		Class for representing DXL table scan operators.
//---------------------------------------------------------------------------



#ifndef GPDXL_CDXLPhysicalTableShareScan_H
#define GPDXL_CDXLPhysicalTableShareScan_H

#include "gpos/base.h"
#include "naucrates/dxl/operators/CDXLPhysical.h"
#include "naucrates/dxl/operators/CDXLTableDescr.h"


namespace gpdxl
{
	//---------------------------------------------------------------------------
	//	@class:
	//		CDXLPhysicalTableShareScan
	//
	//	@doc:
	//		Class for representing DXL table scan operators
	//
	//---------------------------------------------------------------------------
	class CDXLPhysicalTableShareScan : public CDXLPhysical
	{
		private:
		
			// table descriptor for the scanned table
		CDXLTableDescr *m_dxl_table_descr;
			
			// private copy ctor
			CDXLPhysicalTableShareScan(CDXLPhysicalTableShareScan&);

		public:
			// ctors
			explicit
			CDXLPhysicalTableShareScan(CMemoryPool *mp);
			
			CDXLPhysicalTableShareScan(CMemoryPool *mp, CDXLTableDescr *table_descr);
			
			// dtor
			virtual
			~CDXLPhysicalTableShareScan();

			// setters
			void SetTableDescriptor(CDXLTableDescr *);
			
			// operator type
			virtual
			Edxlopid GetDXLOperator() const;

			// operator name
			virtual
			const CWStringConst *GetOpNameStr() const;

			// table descriptor
			const CDXLTableDescr *GetDXLTableDescr();
			
			// serialize operator in DXL format
			virtual
			void SerializeToDXL(CXMLSerializer *xml_serializer, const CDXLNode *dxlnode) const;

			// conversion function
			static
			CDXLPhysicalTableShareScan *Cast
				(
				CDXLOperator *dxl_op
				)
			{
				GPOS_ASSERT(NULL != dxl_op);
				GPOS_ASSERT(EdxlopPhysicalTableShareScan == dxl_op->GetDXLOperator());

				return dynamic_cast<CDXLPhysicalTableShareScan*>(dxl_op);
			}

#ifdef GPOS_DEBUG
			// checks whether the operator has valid structure, i.e. number and
			// types of child nodes
			void AssertValid(const CDXLNode *dxlnode, BOOL validate_children) const;
#endif // GPOS_DEBUG
			
	};
}
#endif // !GPDXL_CDXLPhysicalTableShareScan_H

// EOF

