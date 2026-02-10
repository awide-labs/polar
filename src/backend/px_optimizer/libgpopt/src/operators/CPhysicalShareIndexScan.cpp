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
//		CPhysicalShareIndexScan.cpp
//
//	@doc:
//		Implementation of index scan operator
//---------------------------------------------------------------------------

#include "gpopt/operators/CPhysicalShareIndexScan.h"

#include "gpos/base.h"

#include "gpopt/base/CUtils.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/base/CDistributionSpec.h"
#include "gpopt/base/CDistributionSpecReplicated.h"

using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::CPhysicalShareIndexScan
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CPhysicalShareIndexScan::CPhysicalShareIndexScan(
	CMemoryPool *mp, CIndexDescriptor *pindexdesc, CTableDescriptor *ptabdesc,
	ULONG ulOriginOpId, const CName *pnameAlias, CColRefArray *pdrgpcrOutput,
	COrderSpec *pos)
	: CPhysicalScan(mp, pnameAlias, ptabdesc, pdrgpcrOutput),
	  m_pindexdesc(pindexdesc),
	  m_ulOriginOpId(ulOriginOpId),
	  m_pos(pos)
{
	GPOS_ASSERT(nullptr != pindexdesc);
	GPOS_ASSERT(nullptr != pos);

	/* POLAR px */
	m_pds->Release();
	m_pds = GPOS_NEW(mp) CDistributionSpecReplicated(
				CDistributionSpec::EdtStrictReplicated);
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::~CPhysicalShareIndexScan
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CPhysicalShareIndexScan::~CPhysicalShareIndexScan()
{
	m_pindexdesc->Release();
	m_pos->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::EpetOrder
//
//	@doc:
//		Return the enforcing type for order property based on this operator
//
//---------------------------------------------------------------------------
CEnfdProp::EPropEnforcingType
CPhysicalShareIndexScan::EpetOrder(CExpressionHandle &,	// exprhdl
							  const CEnfdOrder *peo) const
{
	GPOS_ASSERT(nullptr != peo);
	GPOS_ASSERT(!peo->PosRequired()->IsEmpty());

	if (peo->FCompatible(m_pos))
	{
		// required order is already established by the index
		return CEnfdProp::EpetUnnecessary;
	}

	return CEnfdProp::EpetRequired;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::HashValue
//
//	@doc:
//		Combine pointers for table descriptor, index descriptor and Eop
//
//---------------------------------------------------------------------------
ULONG
CPhysicalShareIndexScan::HashValue() const
{
	ULONG ulHash = gpos::CombineHashes(
		COperator::HashValue(),
		gpos::CombineHashes(m_pindexdesc->MDId()->HashValue(),
							gpos::HashPtr<CTableDescriptor>(m_ptabdesc)));
	ulHash =
		gpos::CombineHashes(ulHash, CUtils::UlHashColArray(m_pdrgpcrOutput));

	return ulHash;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::Matches
//
//	@doc:
//		match operator
//
//---------------------------------------------------------------------------
BOOL
CPhysicalShareIndexScan::Matches(COperator *pop) const
{
	return CUtils::FMatchIndex(this, pop);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::OsPrint
//
//	@doc:
//		debug print
//
//---------------------------------------------------------------------------
IOstream &
CPhysicalShareIndexScan::OsPrint(IOstream &os) const
{
	if (m_fPattern)
	{
		return COperator::OsPrint(os);
	}

	os << SzId() << " ";
	// index name
	os << "  Index Name: (";
	m_pindexdesc->Name().OsPrint(os);
	// table name
	os << ")";
	os << ", Table Name: (";
	m_ptabdesc->Name().OsPrint(os);
	os << ")";
	os << ", Columns: [";
	CUtils::OsPrintDrgPcr(os, m_pdrgpcrOutput);
	os << "]";

	return os;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalShareIndexScan::PdsDerive
//
//	@doc:
//		Derive distribution
//
//---------------------------------------------------------------------------
CDistributionSpec *
CPhysicalShareIndexScan::PdsDerive
	(
	CMemoryPool *mp,
	CExpressionHandle &
	)
	const
{
	return GPOS_NEW(mp) CDistributionSpecReplicated(CDistributionSpec::EdtStrictReplicated);
}
// EOF
