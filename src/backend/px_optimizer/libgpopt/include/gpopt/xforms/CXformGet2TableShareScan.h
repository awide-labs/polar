//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CXformGet2TableShareScan.h
//
//	@doc:
//		Transform Get to TableShareScan
//---------------------------------------------------------------------------
#ifndef GPOPT_CXformGet2TableShareScan_H
#define GPOPT_CXformGet2TableShareScan_H

#include "gpos/base.h"
#include "gpopt/xforms/CXformImplementation.h"

namespace gpopt
{
	using namespace gpos;
	
	//---------------------------------------------------------------------------
	//	@class:
	//		CXformGet2TableShareScan
	//
	//	@doc:
	//		Transform Get to TableShareScan
	//
	//---------------------------------------------------------------------------
	class CXformGet2TableShareScan : public CXformImplementation
	{

		private:

			// private copy ctor
			CXformGet2TableShareScan(const CXformGet2TableShareScan &);

		public:
		
			// ctor
			explicit
			CXformGet2TableShareScan(CMemoryPool *);

			// dtor
			virtual 
			~CXformGet2TableShareScan() {}

			// ident accessors
			virtual
			EXformId Exfid() const
			{
				return ExfGet2TableShareScan;
			}
			
			// return a string for xform name
			virtual 
			const CHAR *SzId() const
			{
				return "CXformGet2TableShareScan";
			}
			
			// compute xform promise for a given expression handle
			virtual
			EXformPromise Exfp(CExpressionHandle &exprhdl) const;
			
			// actual transform
			void Transform
				(
				CXformContext *pxfctxt,
				CXformResult *pxfres,
				CExpression *pexpr
				) 
				const;
		
	}; // class CXformGet2TableShareScan

}


#endif // !GPOPT_CXformGet2TableShareScan_H

// EOF
