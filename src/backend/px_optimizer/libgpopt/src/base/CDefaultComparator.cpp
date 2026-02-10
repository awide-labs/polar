//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2014 VMware, Inc. or its affiliates.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CDefaultComparator.cpp
//
//	@doc:
//		Default comparator for IDatum instances to be used in constraint derivation
//
//	@owner:
//
//
//	@test:
//
//---------------------------------------------------------------------------

#include "gpopt/base/CDefaultComparator.h"

#include "gpos/memory/CAutoMemoryPool.h"

#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/eval/IConstExprEvaluator.h"
#include "gpopt/exception.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "naucrates/base/IDatum.h"
#include "naucrates/base/IDatumBool.h"
#include "naucrates/md/IMDId.h"
#include "naucrates/md/IMDType.h"
#include "naucrates/base/CDatumOidGPDB.h"
#include "gpopt/base/CCastUtils.h"
#include "naucrates/exception.h"

using namespace gpopt;
using namespace gpos;
using namespace gpdxl;
using gpnaucrates::IDatum;

//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::CDefaultComparator
//
//	@doc:
//		Ctor
//		Does not take ownership of the constant expression evaluator
//
//---------------------------------------------------------------------------
CDefaultComparator::CDefaultComparator(IConstExprEvaluator *pceeval)
	: m_pceeval(pceeval)
{
	GPOS_ASSERT(nullptr != pceeval);
}

//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::PexprEvalComparison
//
//	@doc:
//		Constructs a comparison expression of type cmp_type between the two given
//		data and evaluates it.
//
//---------------------------------------------------------------------------
BOOL
CDefaultComparator::FEvalComparison(CMemoryPool *mp, const IDatum *datum1,
									const IDatum *datum2,
									IMDType::ECmpType cmp_type) const
{
	GPOS_ASSERT(m_pceeval->FCanEvalExpressions());

	IDatum *pdatum1Copy = datum1->MakeCopy(mp);
	CExpression *pexpr1 = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarConst(mp, pdatum1Copy));
	IDatum *pdatum2Copy = datum2->MakeCopy(mp);
	CExpression *pexpr2 = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarConst(mp, pdatum2Copy));
	CExpression *pexprComp =
		CUtils::PexprScalarCmp(mp, pexpr1, pexpr2, cmp_type);

	CExpression *pexprResult = m_pceeval->PexprEval(pexprComp);
	pexprComp->Release();
	CScalarConst *popScalarConst = CScalarConst::PopConvert(pexprResult->Pop());
	IDatum *datum = popScalarConst->GetDatum();

	GPOS_ASSERT(IMDType::EtiBool == datum->GetDatumType());
	IDatumBool *pdatumBool = dynamic_cast<IDatumBool *>(datum);
	BOOL result = pdatumBool->GetValue();
	pexprResult->Release();

	return result;
}

/* POLAR px: used for hash partition func */
BOOL
CDefaultComparator::FEvalHashExprResult(const CExpression *pexprHash,
										const IDatum *datum) const
{
	GPOS_ASSERT(CUtils::FScalarFuncForHashPartition(pexprHash));

	CAutoMemoryPool amp;
	CMemoryPool *mp = amp.Pmp();

	IDatum *pdatumCopy = datum->MakeCopy(mp);
	CExpression *pexprIdentValue = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarConst(mp, pdatumCopy));

	CExpressionArray *pexprHashArgs = GPOS_NEW(mp) CExpressionArray(mp);
	(*pexprHash)[0]->AddRef();
	(*pexprHash)[1]->AddRef();
	(*pexprHash)[2]->AddRef();
	pexprHashArgs->Append((*pexprHash)[0]);
	pexprHashArgs->Append((*pexprHash)[1]);
	pexprHashArgs->Append((*pexprHash)[2]);

	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	CDatumOidGPDB *pdatumKey = dynamic_cast<CDatumOidGPDB *>(CScalarConst::PopConvert((*pexprHash)[0]->Pop())->GetDatum());
	IMDId *pdatumKey_imdid = GPOS_NEW(mp) CMDIdGPDB(pdatumKey->OidValue());
	const IMDRelation *pmdrel_left = md_accessor->RetrieveRel(pdatumKey_imdid);
	CMDIdGPDB *mdid_type_left = dynamic_cast<CMDIdGPDB *>(pmdrel_left->PartColAt(0)->MdidType());
	CMDIdGPDB *mdid_type_right = dynamic_cast<CMDIdGPDB *>(datum->MDId());
	BOOL fTypesEqual = mdid_type_left->Equals(mdid_type_right);
	pdatumKey_imdid->Release();

	if (!fTypesEqual)
	{
		GPOS_RAISE(
			gpdxl::ExmaMD, gpdxl::ExmiMDObjUnsupported,
			GPOS_WSZ_LIT("Partitioned compare with different types"));
	}

	pexprHashArgs->Append(pexprIdentValue);
	pexprHash->Pop()->AddRef();

	CExpression *pexprHashNew = GPOS_NEW(mp) CExpression(mp, pexprHash->Pop(), pexprHashArgs);

	CExpression *pexprResult = m_pceeval->PexprEval(pexprHashNew);
	pexprHashNew->Release();
	CScalarConst *popScalarConst = CScalarConst::PopConvert(pexprResult->Pop());
	IDatum *datumResult = popScalarConst->GetDatum();

	GPOS_ASSERT(IMDType::EtiBool == datumResult->GetDatumType());
	IDatumBool *pdatumBool = dynamic_cast<IDatumBool *>(datumResult);
	BOOL result = pdatumBool->GetValue();
	pexprResult->Release();

	return result;
}


BOOL
CDefaultComparator::FUseInternalEvaluator(const IDatum *datum1,
										  const IDatum *datum2,
										  BOOL *can_use_external_evaluator)
{
	IMDId *mdid1 = datum1->MDId();
	IMDId *mdid2 = datum2->MDId();

	// be conservative for now and require this extra condition that
	// has been in place for a while (might be relaxed in the future)
	*can_use_external_evaluator =
		GPOS_FTRACE(EopttraceEnableConstantExpressionEvaluation) &&
		CUtils::FConstrainableType(mdid1) && CUtils::FConstrainableType(mdid2);

	if (CUtils::FIntType(mdid1) && CUtils::FIntType(mdid2) &&
		!(*can_use_external_evaluator &&
		  GPOS_FTRACE(EopttraceUseExternalConstantExpressionEvaluationForInts)))
	{
		// INT types can be processed precisely by the internal evaluator
		return true;
	}

	// For now, specifically target date and timestamp columns, since they
	// are mappable to a double value that represents the number of microseconds
	// since Jan 1, 2000 and therefore those can be compared precisely, just like
	// integer types. Same goes for float types, since they map naturally to a
	// double value
	if (mdid1->Equals(datum2->MDId()) && datum1->StatsAreComparable(datum2) &&
		(CMDIdGPDB::m_mdid_date.Equals(mdid1) ||
		 CMDIdGPDB::m_mdid_time.Equals(mdid1) ||
		 CMDIdGPDB::m_mdid_timestamp.Equals(mdid1) ||
		 CMDIdGPDB::m_mdid_float4.Equals(mdid1) ||
		 CMDIdGPDB::m_mdid_float8.Equals(mdid1) ||
		 CMDIdGPDB::m_mdid_numeric.Equals(mdid1)))
	{
		return true;
	}

	// GPDB_12_MERGE_FIXME: Throw an exception when result = false and can_use_external_evaluator = false

	return false;
}


//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::Equals
//
//	@doc:
//		Tests if the two arguments are equal.
//
//---------------------------------------------------------------------------
BOOL
CDefaultComparator::Equals(const IDatum *datum1, const IDatum *datum2) const
{
	BOOL can_use_external_evaluator = false;

	if (FUseInternalEvaluator(datum1, datum2, &can_use_external_evaluator))
	{
		return datum1->StatsAreEqual(datum2);
	}

	if (!can_use_external_evaluator)
	{
		return false;
	}

	CAutoMemoryPool amp;

	// NULL datum is a special case and is being handled here. Assumptions made are
	// NULL is less than everything else. NULL = NULL.
	// Note : NULL is considered equal to NULL because we are using the comparator for
	//        interval calculation.
	if (datum1->IsNull() && datum2->IsNull())
	{
		return true;
	}

	return FEvalComparison(amp.Pmp(), datum1, datum2, IMDType::EcmptEq);
}

//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::IsLessThan
//
//	@doc:
//		Tests if the first argument is less than the second.
//
//---------------------------------------------------------------------------
BOOL
CDefaultComparator::IsLessThan(const IDatum *datum1, const IDatum *datum2) const
{
	BOOL can_use_external_evaluator = false;

	if (FUseInternalEvaluator(datum1, datum2, &can_use_external_evaluator))
	{
		return datum1->StatsAreLessThan(datum2);
	}

	if (!can_use_external_evaluator)
	{
		return false;
	}

	CAutoMemoryPool amp;

	// NULL datum is a special case and is being handled here. Assumptions made are
	// NULL is less than everything else. NULL = NULL.
	// Note : NULL is considered equal to NULL because we are using the comparator for
	//        interval calculation.
	if (datum1->IsNull() && !datum2->IsNull())
	{
		return true;
	}

	return FEvalComparison(amp.Pmp(), datum1, datum2, IMDType::EcmptL);
}

//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::IsLessThanOrEqual
//
//	@doc:
//		Tests if the first argument is less than or equal to the second.
//
//---------------------------------------------------------------------------
BOOL
CDefaultComparator::IsLessThanOrEqual(const IDatum *datum1,
									  const IDatum *datum2) const
{
	BOOL can_use_external_evaluator = false;

	if (FUseInternalEvaluator(datum1, datum2, &can_use_external_evaluator))
	{
		return datum1->StatsAreLessThan(datum2) ||
			   datum1->StatsAreEqual(datum2);
	}

	if (!can_use_external_evaluator)
	{
		return false;
	}

	CAutoMemoryPool amp;

	// NULL datum is a special case and is being handled here. Assumptions made are
	// NULL is less than everything else. NULL = NULL.
	// Note : NULL is considered equal to NULL because we are using the comparator for
	//        interval calculation.
	if (datum1->IsNull())
	{
		// return true since either:
		// 1. datum1 is NULL and datum2 is not NULL. Since NULL is considered
		// less that not null values for interval computations we return true
		// 2. when datum1 and datum2 are both NULL so they are equal
		// for interval computation

		return true;
	}


	return FEvalComparison(amp.Pmp(), datum1, datum2, IMDType::EcmptLEq);
}

//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::IsGreaterThan
//
//	@doc:
//		Tests if the first argument is greater than the second.
//
//---------------------------------------------------------------------------
BOOL
CDefaultComparator::IsGreaterThan(const IDatum *datum1,
								  const IDatum *datum2) const
{
	BOOL can_use_external_evaluator = false;

	if (FUseInternalEvaluator(datum1, datum2, &can_use_external_evaluator))
	{
		return datum1->StatsAreGreaterThan(datum2);
	}

	if (!can_use_external_evaluator)
	{
		return false;
	}

	CAutoMemoryPool amp;

	// NULL datum is a special case and is being handled here. Assumptions made are
	// NULL is less than everything else. NULL = NULL.
	// Note : NULL is considered equal to NULL because we are using the comparator for
	//        interval calculation.
	if (!datum1->IsNull() && datum2->IsNull())
	{
		return true;
	}

	return FEvalComparison(amp.Pmp(), datum1, datum2, IMDType::EcmptG);
}

//---------------------------------------------------------------------------
//	@function:
//		CDefaultComparator::IsLessThanOrEqual
//
//	@doc:
//		Tests if the first argument is greater than or equal to the second.
//
//---------------------------------------------------------------------------
BOOL
CDefaultComparator::IsGreaterThanOrEqual(const IDatum *datum1,
										 const IDatum *datum2) const
{
	BOOL can_use_external_evaluator = false;

	if (FUseInternalEvaluator(datum1, datum2, &can_use_external_evaluator))
	{
		return datum1->StatsAreGreaterThan(datum2) ||
			   datum1->StatsAreEqual(datum2);
	}

	if (!can_use_external_evaluator)
	{
		return false;
	}

	CAutoMemoryPool amp;

	// NULL datum is a special case and is being handled here. Assumptions made are
	// NULL is less than everything else. NULL = NULL.
	// Note : NULL is considered equal to NULL because we are using the comparator for
	//        interval calculation.
	if (datum2->IsNull())
	{
		// return true since either:
		// 1. datum2 is NULL and datum1 is not NULL. Since NULL is considered
		// less that not null values for interval computations we return true
		// 2. when datum1 and datum2 are both NULL so they are equal
		// for interval computation
		return true;
	}

	return FEvalComparison(amp.Pmp(), datum1, datum2, IMDType::EcmptGEq);
}

// EOF
