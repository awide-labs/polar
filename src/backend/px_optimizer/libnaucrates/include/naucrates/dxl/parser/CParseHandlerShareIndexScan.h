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
//		CParseHandlerShareIndexScan.h
//
//	@doc:
//		SAX parse handler class for parsing index scan operator nodes
//---------------------------------------------------------------------------

#ifndef GPDXL_CParseHandlerShareIndexScan_H
#define GPDXL_CParseHandlerShareIndexScan_H

#include "gpos/base.h"

#include "naucrates/dxl/operators/CDXLPhysicalShareIndexScan.h"
#include "naucrates/dxl/parser/CParseHandlerPhysicalOp.h"
#include "naucrates/dxl/xml/dxltokens.h"

namespace gpdxl
{
using namespace gpos;

XERCES_CPP_NAMESPACE_USE

//---------------------------------------------------------------------------
//	@class:
//		CParseHandlerShareIndexScan
//
//	@doc:
//		Parse handler for index scan operator nodes
//
//---------------------------------------------------------------------------
class CParseHandlerShareIndexScan : public CParseHandlerPhysicalOp
{
private:
	// index scan direction
	EdxlIndexScanDirection m_index_scan_dir;

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

protected:
	// common StartElement functionality for ShareIndexScan and IndexOnlyScan
	void StartElementHelper(const XMLCh *const element_local_name,
							const Attributes &attrs, Edxltoken token_type);

	// common EndElement functionality for ShareIndexScan and IndexOnlyScan
	void EndElementHelper(const XMLCh *const element_local_name,
						  Edxltoken token_type);

public:
	CParseHandlerShareIndexScan(const CParseHandlerShareIndexScan &) = delete;

	// ctor
	CParseHandlerShareIndexScan(CMemoryPool *mp,
						   CParseHandlerManager *parse_handler_mgr,
						   CParseHandlerBase *parse_handler_root);
};
}  // namespace gpdxl

#endif	// !GPDXL_CParseHandlerShareIndexScan_H

// EOF
