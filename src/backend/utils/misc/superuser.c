/*-------------------------------------------------------------------------
 *
 * superuser.c
 *	  The superuser() function.  Determines if user has superuser privilege.
 *
 * All code should use either of these two functions to find out
 * whether a given user is a superuser, rather than examining
 * pg_authid.rolsuper directly, so that the escape hatch built in for
 * the single-user case works.
 *
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/superuser.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "utils/inval.h"
#include "utils/syscache.h"

/* POLAR: Shared Server */
#include "storage/polar_session_context.h"

/*
 * In common cases the same roleid (ie, the session or current ID) will
 * be queried repeatedly.  So we maintain a simple one-entry cache for
 * the status of the last requested roleid.  The cache can be flushed
 * at need by watching for cache update events on pg_authid.
 */
static bool roleid_callback_registered = false;

/* POLAR: Shared Server - session-specific superuser cache */
typedef struct PolarSessionSuperUser
{
	Oid			m_last_roleid;	/* InvalidOid == cache not valid */
	bool		m_last_roleid_is_super;
} PolarSessionSuperUser;

PolarSessionSuperUser *
polar_session_superuser_create(MemoryContext mctx)
{
	PolarSessionSuperUser *self;

	if (mctx)
		self = (PolarSessionSuperUser *) MemoryContextAlloc(mctx, sizeof(PolarSessionSuperUser));
	else
		self = (PolarSessionSuperUser *) malloc(sizeof(PolarSessionSuperUser));

	memset(self, 0, sizeof(PolarSessionSuperUser));

	return self;
}

#define POLAR_SESSION_SUPERUSER(name) \
	(polar_session_info()->superuser->m_##name)

#define last_roleid				POLAR_SESSION_SUPERUSER(last_roleid)
#define last_roleid_is_super	POLAR_SESSION_SUPERUSER(last_roleid_is_super)

static void RoleidCallback(Datum arg, int cacheid, uint32 hashvalue);


/*
 * The Postgres user running this command has Postgres superuser privileges
 */
bool
superuser(void)
{
	return superuser_arg(GetUserId());
}


/*
 * The specified role has Postgres superuser privileges
 */
bool
superuser_arg(Oid roleid)
{
	bool		result;
	HeapTuple	rtup;

	/* Quick out for cache hit */
	if (OidIsValid(last_roleid) && last_roleid == roleid)
		return last_roleid_is_super;

	/* Special escape path in case you deleted all your users. */
	if (!IsUnderPostmaster && roleid == BOOTSTRAP_SUPERUSERID)
		return true;

	/* OK, look up the information in pg_authid */
	rtup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(rtup))
	{
		result = ((Form_pg_authid) GETSTRUCT(rtup))->rolsuper;
		ReleaseSysCache(rtup);
	}
	else
	{
		/* Report "not superuser" for invalid roleids */
		result = false;
	}

	/* If first time through, set up callback for cache flushes */
	if (!roleid_callback_registered)
	{
		CacheRegisterSyscacheCallback(AUTHOID,
									  RoleidCallback,
									  (Datum) 0);
		roleid_callback_registered = true;
	}

	/* Cache the result for next time */
	last_roleid = roleid;
	last_roleid_is_super = result;

	return result;
}

/*
 * RoleidCallback
 *		Syscache inval callback function
 */
static void
RoleidCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	/* Invalidate our local cache in case role's superuserness changed */
	last_roleid = InvalidOid;
}
