//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CPhysicalTableShareScan.h
//
//	@doc:
//		Table scan operator
//---------------------------------------------------------------------------
#ifndef GPOPT_CPhysicalTableShareScan_H
#define GPOPT_CPhysicalTableShareScan_H

#include "gpos/base.h"
#include "gpopt/operators/CPhysicalScan.h"

namespace gpopt
{
	
	//---------------------------------------------------------------------------
	//	@class:
	//		CPhysicalTableShareScan
	//
	//	@doc:
	//		Table scan operator
	//
	//---------------------------------------------------------------------------
	class CPhysicalTableShareScan : public CPhysicalScan
	{

		private:

			// private copy ctor
			CPhysicalTableShareScan(const CPhysicalTableShareScan&);

		public:
		
			// ctors
			explicit
			CPhysicalTableShareScan(CMemoryPool *mp);
			CPhysicalTableShareScan(CMemoryPool *, const CName *, CTableDescriptor *, CColRefArray *);

			// ident accessors
			virtual 
			EOperatorId Eopid() const override
			{
				return EopPhysicalTableShareScan;
			}
			
			// return a string for operator name
			virtual 
			const CHAR *SzId() const override
			{
				return "CPhysicalTableShareScan";
			}
			
			// operator specific hash function
			virtual ULONG HashValue() const override;
			
			// match function
			BOOL Matches(COperator *) const override;

			//-------------------------------------------------------------------------------------
			//-------------------------------------------------------------------------------------
			//-------------------------------------------------------------------------------------

			// debug print
			virtual 
			IOstream &OsPrint(IOstream &) const override;


			// conversion function
			static
			CPhysicalTableShareScan *PopConvert
				(
				COperator *pop
				)
			{
				GPOS_ASSERT(NULL != pop);
				GPOS_ASSERT(EopPhysicalTableShareScan == pop->Eopid());

				return reinterpret_cast<CPhysicalTableShareScan*>(pop);
			}

			// statistics derivation during costing
			virtual
			IStatistics *PstatsDerive
				(
				CMemoryPool *, // mp
				CExpressionHandle &, // exprhdl
				CReqdPropPlan *, // prpplan
				IStatisticsArray * //stats_ctxt
				)
				const override
			{
				GPOS_ASSERT(!"stats derivation during costing for table scan is invalid");

				return NULL;
			}

			// derive distribution
			virtual
			CDistributionSpec *PdsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl) const override; 
			
			virtual
			CRewindabilitySpec *PrsDerive
				(
				CMemoryPool *mp,
				CExpressionHandle & // exprhdl
				)
				const override
			{
				// mark-restorability of output is always true
				return GPOS_NEW(mp) CRewindabilitySpec(CRewindabilitySpec::ErtMarkRestore, CRewindabilitySpec::EmhtNoMotion);
			}


	}; // class CPhysicalTableShareScan

}

#endif // !GPOPT_CPhysicalTableShareScan_H

// EOF
