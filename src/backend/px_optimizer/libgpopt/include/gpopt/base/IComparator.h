//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2014 VMware, Inc. or its affiliates.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		IComparator.h
//
//	@doc:
//		Interface for comparing IDatum instances.
//
//	@owner:
//
//
//	@test:
//
//---------------------------------------------------------------------------

#ifndef GPOPT_IComparator_H
#define GPOPT_IComparator_H

#include "gpos/base.h"

namespace gpnaucrates
{
// fwd declarations
class IDatum;
}  // namespace gpnaucrates

namespace gpopt
{
using gpnaucrates::IDatum;

/* POLAR px: used for hash partition func */
class CExpression;

//---------------------------------------------------------------------------
//	@class:
//		IComparator
//
//	@doc:
//		Interface for comparing IDatum instances.
//
//---------------------------------------------------------------------------
class IComparator
{
public:
	virtual ~IComparator() = default;

	// tests if the two arguments are equal
	virtual gpos::BOOL Equals(const IDatum *datum1,
							  const IDatum *datum2) const = 0;

	// tests if the first argument is less than the second
	virtual gpos::BOOL IsLessThan(const IDatum *datum1,
								  const IDatum *datum2) const = 0;

	// tests if the first argument is less or equal to the second
	virtual gpos::BOOL IsLessThanOrEqual(const IDatum *datum1,
										 const IDatum *datum2) const = 0;

	// tests if the first argument is greater than the second
	virtual gpos::BOOL IsGreaterThan(const IDatum *datum1,
									 const IDatum *datum2) const = 0;

	// tests if the first argument is greater or equal to the second
	virtual gpos::BOOL IsGreaterThanOrEqual(const IDatum *datum1,
											const IDatum *datum2) const = 0;

	/* POLAR px: used for hash partition func */
	virtual gpos::BOOL FEvalHashExprResult(const CExpression *pexprHash,
										   const IDatum *datum) const = 0;

};
}  // namespace gpopt

#endif	// !GPOPT_IComparator_H

// EOF
