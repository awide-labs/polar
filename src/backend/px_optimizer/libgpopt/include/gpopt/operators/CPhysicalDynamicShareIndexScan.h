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
//		CPhysicalDynamicShareIndexScan.h
//
//	@doc:
//		Physical dynamic index scan operators on partitioned tables
//---------------------------------------------------------------------------
#ifndef GPOPT_CPhysicalDynamicShareIndexScan_H
#define GPOPT_CPhysicalDynamicShareIndexScan_H

#include "gpos/base.h"

#include "gpopt/metadata/CIndexDescriptor.h"
#include "gpopt/operators/CPhysicalDynamicScan.h"

namespace gpopt
{
// fwd declarations
class CTableDescriptor;
class CIndexDescriptor;
class CName;
class CPartConstraint;

//---------------------------------------------------------------------------
//	@class:
//		CPhysicalDynamicShareIndexScan
//
//	@doc:
//		Physical dynamic index scan operators for partitioned tables
//
//---------------------------------------------------------------------------
class CPhysicalDynamicShareIndexScan : public CPhysicalDynamicScan
{
private:
	// index descriptor
	CIndexDescriptor *m_pindexdesc;

	// order
	COrderSpec *m_pos;

public:
	CPhysicalDynamicShareIndexScan(const CPhysicalDynamicShareIndexScan &) = delete;

	// ctors
	CPhysicalDynamicShareIndexScan(CMemoryPool *mp, CIndexDescriptor *pindexdesc,
							  CTableDescriptor *ptabdesc, ULONG ulOriginOpId,
							  const CName *pnameAlias,
							  CColRefArray *pdrgpcrOutput, ULONG scan_id,
							  CColRef2dArray *pdrgpdrgpcrPart, COrderSpec *pos,
							  IMdIdArray *partition_mdids,
							  ColRefToUlongMapArray *root_col_mapping_per_part);

	// dtor
	~CPhysicalDynamicShareIndexScan() override;


	// ident accessors
	EOperatorId
	Eopid() const override
	{
		return EopPhysicalDynamicShareIndexScan;
	}

	// operator name
	const CHAR *
	SzId() const override
	{
		return "CPhysicalDynamicShareIndexScan";
	}

	// index descriptor
	CIndexDescriptor *
	Pindexdesc() const
	{
		return m_pindexdesc;
	}

	// operator specific hash function
	ULONG HashValue() const override;

	// match function
	BOOL Matches(COperator *pop) const override;

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

	//-------------------------------------------------------------------------------------
	// Enforced Properties
	//-------------------------------------------------------------------------------------

	// return order property enforcing type for this operator
	CEnfdProp::EPropEnforcingType EpetOrder(
		CExpressionHandle &exprhdl, const CEnfdOrder *peo) const override;

	// conversion function
	static CPhysicalDynamicShareIndexScan *
	PopConvert(COperator *pop)
	{
		GPOS_ASSERT(nullptr != pop);
		GPOS_ASSERT(EopPhysicalDynamicShareIndexScan == pop->Eopid());

		return dynamic_cast<CPhysicalDynamicShareIndexScan *>(pop);
	}

	// debug print
	IOstream &OsPrint(IOstream &) const override;

	// statistics derivation during costing
	IStatistics *PstatsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl,
							  CReqdPropPlan *prpplan,
							  IStatisticsArray *stats_ctxt) const override;


	// derive distribution
	virtual
	CDistributionSpec *PdsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl) const override;
	// return distribution property enforcing type for this operator

};	// class CPhysicalDynamicShareIndexScan

}  // namespace gpopt

#endif	// !GPOPT_CPhysicalDynamicShareIndexScan_H

// EOF
