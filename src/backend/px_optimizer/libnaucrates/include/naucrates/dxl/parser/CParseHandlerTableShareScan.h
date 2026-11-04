//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2010 Greenplum, Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CParseHandlerTableShareScan.h
//
//	@doc:
//		SAX parse handler class for parsing table scan operator nodes.
//---------------------------------------------------------------------------

#ifndef GPDXL_CParseHandlerTableShareScan_H
#define GPDXL_CParseHandlerTableShareScan_H

#include "gpos/base.h"
#include "naucrates/dxl/parser/CParseHandlerPhysicalOp.h"

#include "naucrates/dxl/operators/CDXLPhysicalTableShareScan.h"
#include "naucrates/dxl/xml/dxltokens.h"


namespace gpdxl
{
	using namespace gpos;

	XERCES_CPP_NAMESPACE_USE
	
	//---------------------------------------------------------------------------
	//	@class:
	//		CParseHandlerTableShareScan
	//
	//	@doc:
	//		Parse handler for parsing a table scan operator
	//
	//---------------------------------------------------------------------------
	class CParseHandlerTableShareScan : public CParseHandlerPhysicalOp
	{
		private:
			
			// the table scan operator
			CDXLPhysicalTableShareScan *m_dxl_op;
			
			// private copy ctor
			CParseHandlerTableShareScan(const CParseHandlerTableShareScan &);

			// process the start of an element
			virtual
			void StartElement
				(
					const XMLCh* const element_uri, 		// URI of element's namespace
 					const XMLCh* const element_local_name,	// local part of element's name
					const XMLCh* const element_qname,		// element's qname
					const Attributes& attr				// element's attributes
				);
				
			// process the end of an element
			virtual
			void EndElement
				(
					const XMLCh* const element_uri, 		// URI of element's namespace
					const XMLCh* const element_local_name,	// local part of element's name
					const XMLCh* const element_qname		// element's qname
				);
			
		protected:

			// start element helper function
			void StartElement(const XMLCh* const element_local_name, Edxltoken token_type);

			// end element helper function
			void EndElement(const XMLCh* const element_local_name, Edxltoken token_type);

		public:
			// ctor
			CParseHandlerTableShareScan
				(
				CMemoryPool *mp,
				CParseHandlerManager *parse_handler_mgr,
				CParseHandlerBase *parse_handler_root
				);
	};
}

#endif // !GPDXL_CParseHandlerTableShareScan_H

// EOF
