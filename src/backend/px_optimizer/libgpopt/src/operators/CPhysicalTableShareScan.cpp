//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CPhysicalTableShareScan.cpp
//
//	@doc:
//		Implementation of basic table scan operator
//---------------------------------------------------------------------------

#include "gpos/base.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/base/CDistributionSpec.h"
#include "gpopt/base/CDistributionSpecHashed.h"
#include "gpopt/base/CDistributionSpecReplicated.h"
#include "gpopt/base/CDistributionSpecRandom.h"
#include "gpopt/base/CDistributionSpecSingleton.h"

#include "gpopt/operators/CPhysicalTableShareScan.h"
#include "gpopt/metadata/CTableDescriptor.h"
#include "gpopt/metadata/CName.h"

using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalTableShareScan::CPhysicalTableShareScan
//
//	@doc:
//		ctor
//
//---------------------------------------------------------------------------
CPhysicalTableShareScan::CPhysicalTableShareScan
	(
	CMemoryPool *mp,
	const CName *pnameAlias,
	CTableDescriptor *ptabdesc,
	CColRefArray *pdrgpcrOutput
	)
	:
	CPhysicalScan(mp, pnameAlias, ptabdesc, pdrgpcrOutput)
{
	/* POLAR px */
	m_pds->Release();
	m_pds = GPOS_NEW(mp) CDistributionSpecReplicated(
				CDistributionSpec::EdtStrictReplicated);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalTableShareScan::HashValue
//
//	@doc:
//		Combine pointer for table descriptor and Eop
//
//---------------------------------------------------------------------------
ULONG
CPhysicalTableShareScan::HashValue() const
{
	ULONG ulHash = gpos::CombineHashes(COperator::HashValue(), m_ptabdesc->MDId()->HashValue());
	ulHash = gpos::CombineHashes(ulHash, CUtils::UlHashColArray(m_pdrgpcrOutput));

	return ulHash;
}

	
//---------------------------------------------------------------------------
//	@function:
//		CPhysicalTableShareScan::Matches
//
//	@doc:
//		match operator
//
//---------------------------------------------------------------------------
BOOL
CPhysicalTableShareScan::Matches
	(
	COperator *pop
	)
	const
{
	if (Eopid() != pop->Eopid())
	{
		return false;
	}

	CPhysicalTableShareScan *popTableScan = CPhysicalTableShareScan::PopConvert(pop);
	return m_ptabdesc->MDId()->Equals(popTableScan->Ptabdesc()->MDId()) &&
			m_pdrgpcrOutput->Equals(popTableScan->PdrgpcrOutput());
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalScan::PdsDerive
//
//	@doc:
//		Derive distribution
//
//---------------------------------------------------------------------------
CDistributionSpec *
CPhysicalTableShareScan::PdsDerive
	(
	CMemoryPool *mp,
	CExpressionHandle &
	)
	const
{
	return GPOS_NEW(mp) CDistributionSpecReplicated(CDistributionSpec::EdtStrictReplicated);
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalTableShareScan::OsPrint
//
//	@doc:
//		debug print
//
//---------------------------------------------------------------------------
IOstream &
CPhysicalTableShareScan::OsPrint
	(
	IOstream &os
	)
	const
{
	os << SzId() << " ";
	
	// alias of table as referenced in the query
	m_pnameAlias->OsPrint(os);

	// actual name of table in catalog and columns
	os << " (";
	m_ptabdesc->Name().OsPrint(os);
	os << ")";
	
	return os;
}



// EOF

