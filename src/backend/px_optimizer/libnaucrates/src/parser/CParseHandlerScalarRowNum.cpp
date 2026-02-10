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
//		CParseHandlerScalarRowNum.cpp
//
//	@doc:
//		Implementation of the SAX parse handler class for parsing
//		scalar rownum  operators.
//---------------------------------------------------------------------------

#include "naucrates/dxl/parser/CParseHandlerScalarRowNum.h"

#include "naucrates/dxl/operators/CDXLOperatorFactory.h"
#include "naucrates/dxl/parser/CParseHandlerFactory.h"
#include "naucrates/dxl/parser/CParseHandlerScalarOp.h"

using namespace gpdxl;


XERCES_CPP_NAMESPACE_USE

//---------------------------------------------------------------------------
//	@function:
//		CParseHandlerScalarRowNum::CParseHandlerScalarRowNum
//
//	@doc:
//		Constructor
//
//---------------------------------------------------------------------------
CParseHandlerScalarRowNum::CParseHandlerScalarRowNum(
	CMemoryPool *mp, CParseHandlerManager *parse_handler_mgr,
	CParseHandlerBase *parse_handler_root)
	: CParseHandlerScalarOp(mp, parse_handler_mgr, parse_handler_root),
	  m_dxl_op(nullptr)
{
}

//---------------------------------------------------------------------------
//	@function:
//		CParseHandlerScalarRowNum::~CParseHandlerScalarRowNum
//
//	@doc:
//		Destructor
//
//---------------------------------------------------------------------------
CParseHandlerScalarRowNum::~CParseHandlerScalarRowNum() = default;

//---------------------------------------------------------------------------
//	@function:
//		CParseHandlerScalarRowNum::StartElement
//
//	@doc:
//		Invoked by Xerces to process an opening tag
//
//---------------------------------------------------------------------------
void
CParseHandlerScalarRowNum::StartElement(const XMLCh *const,	// element_uri,
									   const XMLCh *const element_local_name,
									   const XMLCh *const,	// element_qname
									   const Attributes &attrs)
{
	if (0 !=
		XMLString::compareString(CDXLTokens::XmlstrToken(EdxltokenScalarRowNum),
								 element_local_name))
	{
		CWStringDynamic *str = CDXLUtils::CreateDynamicStringFromXMLChArray(
			m_parse_handler_mgr->GetDXLMemoryManager(), element_local_name);
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXLUnexpectedTag,
				   str->GetBuffer());
	}

	// parse and create rownum operator
	m_dxl_op = (CDXLScalarRowNum *) CDXLOperatorFactory::MakeDXLScalarRowNum(
		m_parse_handler_mgr->GetDXLMemoryManager(), attrs);
}

//---------------------------------------------------------------------------
//	@function:
//		CParseHandlerScalarRowNum::EndElement
//
//	@doc:
//		Invoked by Xerces to process a closing tag
//
//---------------------------------------------------------------------------
void
CParseHandlerScalarRowNum::EndElement(const XMLCh *const,  // element_uri,
									 const XMLCh *const element_local_name,
									 const XMLCh *const	 // element_qname
)
{
	if (0 !=
		XMLString::compareString(CDXLTokens::XmlstrToken(EdxltokenScalarRowNum),
								 element_local_name))
	{
		CWStringDynamic *str = CDXLUtils::CreateDynamicStringFromXMLChArray(
			m_parse_handler_mgr->GetDXLMemoryManager(), element_local_name);
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXLUnexpectedTag,
				   str->GetBuffer());
	}

	// construct scalar ident node
	GPOS_ASSERT(nullptr != m_dxl_op);
	m_dxl_node = GPOS_NEW(m_mp) CDXLNode(m_mp);
	m_dxl_node->SetOperator(m_dxl_op);

	// deactivate handler
	m_parse_handler_mgr->DeactivateHandler();
}

// EOF
