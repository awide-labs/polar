/*-------------------------------------------------------------------------
 *
 * px_policy.h
 *	  definitions for the px_distribution_policy catalog table
 *
 * Portions Copyright (c) 2005-2011, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	    src/include/catalog/px_policy.h
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#ifndef _PX_POLICY_H_
#define _PX_POLICY_H_

#include "access/attnum.h"
#include "catalog/genbki.h"
#include "nodes/pg_list.h"

/*
 * A magic number, setting PxPolicy.numsegments to this value will cause a
 * failed assertion at runtime, which allows developers to debug with gdb.
 */
#define PX_POLICY_INVALID_NUMSEGMENTS()		(-1)

/*
 * The the default numsegments policies when creating a table.
 *
 * - FULL: all the segments;
 * - RANDOM: pick a random set of segments each time;
 * - MINIMAL: the minimal set of segments;
 */
enum
{
	PX_DEFAULT_NUMSEGMENTS_FULL = -1,
	GP_DEFAULT_NUMSEGMENTS_RANDOM = -2,
	GP_DEFAULT_NUMSEGMENTS_MINIMAL = -3,
};

/*
 * PxPolicyType represents a type of policy under which a relation's
 * tuples may be assigned to a component database.
 */
typedef enum PxPolicyType
{
	POLICYTYPE_PARTITIONED,		/* Tuples partitioned onto segment database. */
	POLICYTYPE_ENTRY,			/* Tuples stored on entry database. */
	POLICYTYPE_REPLICATED		/* Tuples stored a copy on all segment
								 * database. */
} PxPolicyType;

/*
 * PxPolicy represents a Greenplum DB data distribution policy. The ptype field
 * is always significant.  Other fields may be specific to a particular
 * type.
 *
 * A PxPolicy is typically palloc'd with space for nattrs integer
 * attribute numbers (attrs) in addition to sizeof(PxPolicy).
 */
typedef struct PxPolicy
{
	NodeTag		type;
	PxPolicyType ptype;
	int			numsegments;

	/* These fields apply to POLICYTYPE_PARTITIONED. */
	int			nattrs;
	AttrNumber *attrs;			/* array of attribute numbers  */
	Oid		   *opclasses;		/* and their opclasses */
} PxPolicy;

extern PxPolicy *makePxPolicy(PxPolicyType ptype, int nattrs, int numsegments);
extern PxPolicy *createRandomPartitionedPolicy(int numsegments);

#endif							/* _PX_POLICY_H_ */
