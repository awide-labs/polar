/*-------------------------------------------------------------------------
 *
 * px_cat.c
 *	  Provides routines for reading info from mpp schema tables
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/px_cat.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/px_policy.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "catalog/objectaddress.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "px/px_hash.h"
#include "px/px_util.h"
#include "px/px_vars.h"			/* px_role */


PxPolicy *
makePxPolicy(PxPolicyType ptype, int nattrs, int numsegments)
{
	PxPolicy   *policy;
	size_t		size;
	char	   *p;

	size = MAXALIGN(sizeof(PxPolicy)) +
		MAXALIGN(nattrs * sizeof(AttrNumber)) +
		MAXALIGN(nattrs * sizeof(Oid));
	p = palloc(size);
	policy = (PxPolicy *) p;
	p += MAXALIGN(sizeof(PxPolicy));
	if (nattrs > 0)
	{
		policy->attrs = (AttrNumber *) p;
		p += MAXALIGN(nattrs * sizeof(AttrNumber));
		policy->opclasses = (Oid *) p;
		p += MAXALIGN(nattrs * sizeof(Oid));
	}
	else
	{
		policy->attrs = NULL;
		policy->opclasses = NULL;
	}

	policy->type = T_PxPolicy;
	policy->ptype = ptype;
	policy->numsegments = numsegments;
	policy->nattrs = nattrs;

	Assert(numsegments > 0);
	if (numsegments == PX_POLICY_INVALID_NUMSEGMENTS())
	{
		Assert(!"what's the proper value of numsegments?");
	}

	return policy;
}

/*
 * createRandomPartitionedPolicy -- Create a policy with randomly
 * partitioned distribution
 */
PxPolicy *
createRandomPartitionedPolicy(int numsegments)
{
	return makePxPolicy(POLICYTYPE_PARTITIONED, 0, numsegments);
}
