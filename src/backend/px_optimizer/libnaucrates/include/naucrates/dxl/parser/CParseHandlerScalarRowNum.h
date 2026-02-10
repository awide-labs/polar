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
//		CParseHandlerScalarRowNum.h
//
//	@doc:
//		SAX parse handler class for parsing scalar rownum nodes.
//---------------------------------------------------------------------------

#ifndef GPDXL_CParseHandlerScalarRowNum_H
#define GPDXL_CParseHandlerScalarRowNum_H

#include "gpos/base.h"

#include "naucrates/dxl/operators/CDXLScalarRowNum.h"
#include "naucrates/dxl/parser/CParseHandlerScalarOp.h"

namespace gpdxl
{
using namespace gpos;

XERCES_CPP_NAMESPACE_USE

//---------------------------------------------------------------------------
//	@class:
//		CParseHandlerScalarRowNum
//
//	@doc:
//		Parse handler for parsing a scalar rownum operator
//
//---------------------------------------------------------------------------
class CParseHandlerScalarRowNum : public CParseHandlerScalarOp
{
private:
	// the scalar rownum
	CDXLScalarRowNum *m_dxl_op;

	// process the start of an element
	void StartElement(
		const XMLCh *const element_uri,			// URI of element's namespace
		const XMLCh *const element_local_name,	// local part of element's name
		const XMLCh *const element_qname,		// element's qname
		const Attributes &attr					// element's attributes
		) override;

	// process the end of an element
	void EndElement(
		const XMLCh *const element_uri,			// URI of element's namespace
		const XMLCh *const element_local_name,	// local part of element's name
		const XMLCh *const element_qname		// element's qname
		) override;

public:
	CParseHandlerScalarRowNum(const CParseHandlerScalarRowNum &) = delete;

	CParseHandlerScalarRowNum(CMemoryPool *mp,
							 CParseHandlerManager *parse_handler_mgr,
							 CParseHandlerBase *parse_handler_root);

	~CParseHandlerScalarRowNum() override;
};
}  // namespace gpdxl

#endif	// !GPDXL_CParseHandlerScalarRowNum_H

// EOF
