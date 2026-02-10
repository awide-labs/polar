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
//		CPhysicalShareIndexScan.h
//
//	@doc:
//		Base class for physical index scan operators
//---------------------------------------------------------------------------
#ifndef GPOPT_CPhysicalShareIndexScan_H
#define GPOPT_CPhysicalShareIndexScan_H

#include "gpos/base.h"

#include "gpopt/metadata/CIndexDescriptor.h"
#include "gpopt/operators/CPhysicalScan.h"

namespace gpopt
{
// fwd declarations
class CTableDescriptor;
class CIndexDescriptor;
class CName;
class CDistributionSpecHashed;

//---------------------------------------------------------------------------
//	@class:
//		CPhysicalShareIndexScan
//
//	@doc:
//		Base class for physical index scan operators
//
//---------------------------------------------------------------------------
class CPhysicalShareIndexScan : public CPhysicalScan
{
private:
	// index descriptor
	CIndexDescriptor *m_pindexdesc;

	// origin operator id -- gpos::ulong_max if operator was not generated via a transformation
	ULONG m_ulOriginOpId;

	// order
	COrderSpec *m_pos;

public:
	CPhysicalShareIndexScan(const CPhysicalShareIndexScan &) = delete;

	// ctors
	CPhysicalShareIndexScan(CMemoryPool *mp, CIndexDescriptor *pindexdesc,
					   CTableDescriptor *ptabdesc, ULONG ulOriginOpId,
					   const CName *pnameAlias, CColRefArray *colref_array,
					   COrderSpec *pos);

	// dtor
	~CPhysicalShareIndexScan() override;


	// ident accessors
	EOperatorId
	Eopid() const override
	{
		return EopPhysicalShareIndexScan;
	}

	// operator name
	const CHAR *
	SzId() const override
	{
		return "CPhysicalShareIndexScan";
	}

	// table alias name
	const CName &
	NameAlias() const
	{
		return *m_pnameAlias;
	}

	// origin operator id -- gpos::ulong_max if operator was not generated via a transformation
	ULONG
	UlOriginOpId() const
	{
		return m_ulOriginOpId;
	}

	// operator specific hash function
	ULONG HashValue() const override;

	// match function
	BOOL Matches(COperator *pop) const override;

	// index descriptor
	CIndexDescriptor *
	Pindexdesc() const
	{
		return m_pindexdesc;
	}

	// sensitivity to order of inputs
	BOOL
	FInputOrderSensitive() const override
	{
		return true;
	}

	//-------------------------------------------------------------------------------------
	// Derived Plan Properties
	//-------------------------------------------------------------------------------------

	// derive sort order
	COrderSpec *
	PosDerive(CMemoryPool *,	   //mp
			  CExpressionHandle &  //exprhdl
	) const override
	{
		m_pos->AddRef();
		return m_pos;
	}

	CRewindabilitySpec *
	PrsDerive(CMemoryPool *mp,
			  CExpressionHandle &  // exprhdl
	) const override
	{
		// rewindability of output is always true
		return GPOS_NEW(mp)
			CRewindabilitySpec(CRewindabilitySpec::ErtMarkRestore,
							   CRewindabilitySpec::EmhtNoMotion);
	}

	//-------------------------------------------------------------------------------------
	// Enforced Properties
	//-------------------------------------------------------------------------------------

	// return order property enforcing type for this operator
	CEnfdProp::EPropEnforcingType EpetOrder(
		CExpressionHandle &exprhdl, const CEnfdOrder *peo) const override;

	// conversion function
	static CPhysicalShareIndexScan *
	PopConvert(COperator *pop)
	{
		GPOS_ASSERT(nullptr != pop);
		GPOS_ASSERT(EopPhysicalShareIndexScan == pop->Eopid());

		return dynamic_cast<CPhysicalShareIndexScan *>(pop);
	}

	// statistics derivation during costing
	IStatistics *
	PstatsDerive(CMemoryPool *,		   // mp
				 CExpressionHandle &,  // exprhdl
				 CReqdPropPlan *,	   // prpplan
				 IStatisticsArray *	   //stats_ctxt
	) const override
	{
		GPOS_ASSERT(
			!"stats derivation during costing for index scan is invalid");

		return nullptr;
	}

	// debug print
	IOstream &OsPrint(IOstream &) const override;

	// derive distribution
	virtual
	CDistributionSpec *PdsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl) const override;
	// return distribution property enforcing type for this operator
};	// class CPhysicalShareIndexScan

}  // namespace gpopt

#endif	// !GPOPT_CPhysicalShareIndexScan_H

// EOF
