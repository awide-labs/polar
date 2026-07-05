/*-------------------------------------------------------------------------
 *
 * polar_session_inval.c
 *    Process invalidation message for polar session context.
 *
 *
 * Copyright (c) 2020, Alibaba inc.
 *
 * src/backend/utils/cache/polar_session_inval.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/polar_dispatcher.h"
#include "storage/polar_session_context.h"
#include "utils/plancache.h"
#include "utils/polar_session_inval.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"

/*
 * Dynamically-registered callback functions.  Current implementation
 * assumes there won't be enough of these to justify a dynamically resizable
 * array; it'd be easy to improve that if needed.
 *
 * To avoid searching in CallSyscacheCallbacks, all callbacks for a given
 * syscache are linked into a list pointed to by syscache_callback_links[id].
 * The link values are syscache_callback_list[] index plus 1, or 0 for none.
 */

#define MAX_SYSCACHE_CALLBACKS 64
#define MAX_RELCACHE_CALLBACKS 10

static struct SYSCACHECALLBACK
{
	int16		id;				/* cache number */
	int16		link;			/* next callback index+1 for same cache */
	SyscacheCallbackFunction function;
	Datum		arg;
}			syscache_callback_list[MAX_SYSCACHE_CALLBACKS];

static int16 syscache_callback_links[SysCacheSize];

static int	syscache_callback_count = 0;

static struct RELCACHECALLBACK
{
	RelcacheCallbackFunction function;
	Datum		arg;
}			relcache_callback_list[MAX_RELCACHE_CALLBACKS];

static int	relcache_callback_count = 0;

static void polar_call_syscache_callbacks(int cacheid, uint32 hashvalue);
static void polar_session_invalid_all(void);

/* Process a single invalidation message (which could be of any type). */
static void
polar_session_local_execute_invalidation_message(SharedInvalidationMessage *msg)
{
	if (msg->id >= 0)
	{
		if (msg->cc.dbId == MyDatabaseId || msg->cc.dbId == InvalidOid)
		{
			polar_call_syscache_callbacks(msg->cc.id, msg->cc.hashValue);
		}
	}
	else if (msg->id == SHAREDINVALCATALOG_ID)
	{
		if (msg->cat.dbId == MyDatabaseId || msg->cat.dbId == InvalidOid)
		{
			polar_session_invalid_all();
		}
	}
	else if (msg->id == SHAREDINVALRELCACHE_ID)
	{
		if (msg->rc.dbId == MyDatabaseId || msg->rc.dbId == InvalidOid)
		{
			int			i;

			for (i = 0; i < relcache_callback_count; i++)
			{
				struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

				ccitem->function(ccitem->arg, msg->rc.relId);
			}
		}
	}
	else
	{
		elog(FATAL, "unrecognized SI message ID: %d for polar session", msg->id);
	}
}

static void
polar_session_invalid_all(void)
{
	/* prepared statement */
	ResetPlanCache();

	/* namespace */
	/* Force search path to be recomputed on next use */
	polar_inval_namespace_path();
}

void
polar_session_accept_invalidation_messages(PolarSessionContext *session)
{
#define MAXINVALMSGS 32
	static SharedInvalidationMessage messages[MAXINVALMSGS];

	do
	{
		int			getResult;
		int			i;

		getResult = polar_session_SI_get_data_entries(session, messages, MAXINVALMSGS);
		if (getResult < 0)
		{
			/* got a reset message */
			polar_session_invalid_all();
			break;				/* nothing more to do */
		}
		for (i = 0; i < getResult; ++i)
		{
			polar_session_local_execute_invalidation_message(&messages[i]);
		}
		if (getResult < MAXINVALMSGS)	/* nothing more to do */
			break;
	} while (true);
}

/*
 * polar_ss_cache_register_syscache_callback
 *		Register the specified function to be called for all future
 *		invalidation events in the specified cache.  The cache ID and the
 *		hash value of the tuple being invalidated will be passed to the
 *		function.
 *
 * NOTE: Hash value zero will be passed if a cache reset request is received.
 * In this case the called routines should flush all cached state.
 * Yes, there's a possibility of a false match to zero, but it doesn't seem
 * worth troubling over, especially since most of the current callees just
 * flush all cached state anyway.
 */
void
polar_ss_cache_register_syscache_callback(int cacheid,
										  SyscacheCallbackFunction func,
										  Datum arg)
{
	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	if (cacheid < 0 || cacheid >= SysCacheSize)
		elog(FATAL, "invalid cache ID: %d", cacheid);
	if (syscache_callback_count >= MAX_SYSCACHE_CALLBACKS)
		elog(FATAL, "out of syscache_callback_list slots");

	if (syscache_callback_links[cacheid] == 0)
	{
		/* first callback for this cache */
		syscache_callback_links[cacheid] = syscache_callback_count + 1;
	}
	else
	{
		/* add to end of chain, so that older callbacks are called first */
		int			i = syscache_callback_links[cacheid] - 1;

		while (syscache_callback_list[i].link > 0)
		{
			if (syscache_callback_list[i].id == cacheid &&
				syscache_callback_list[i].function == func &&
				syscache_callback_list[i].arg == arg)
				/* already existed, ignore */
				return;

			i = syscache_callback_list[i].link - 1;
		}
		syscache_callback_list[i].link = syscache_callback_count + 1;
	}

	syscache_callback_list[syscache_callback_count].id = cacheid;
	syscache_callback_list[syscache_callback_count].link = 0;
	syscache_callback_list[syscache_callback_count].function = func;
	syscache_callback_list[syscache_callback_count].arg = arg;

	++syscache_callback_count;
}

/*
 * polar_cache_register_relcache_callback
 *		Register the specified function to be called for all future
 *		relcache invalidation events.  The OID of the relation being
 *		invalidated will be passed to the function.
 *
 * NOTE: InvalidOid will be passed if a cache reset request is received.
 * In this case the called routines should flush all cached state.
 */
void
polar_ss_cache_register_relcache_callback(RelcacheCallbackFunction func,
										  Datum arg)
{
	int			i = 0;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	if (relcache_callback_count >= MAX_RELCACHE_CALLBACKS)
		elog(FATAL, "out of relcache_callback_list slots");

	for (i = 0; i < relcache_callback_count; i++)
	{
		if (relcache_callback_list[i].function == func &&
			relcache_callback_list[i].arg == arg)
			/* already existed, ignore */
			return;
	}

	relcache_callback_list[relcache_callback_count].function = func;
	relcache_callback_list[relcache_callback_count].arg = arg;

	++relcache_callback_count;
}

/*
 * polar_call_syscache_callbacks
 *
 * This is exported so that CatalogCacheFlushCatalog can call it, saving
 * this module from knowing which catcache IDs correspond to which catalogs.
 */
static void
polar_call_syscache_callbacks(int cacheid, uint32 hashvalue)
{
	int			i;

	if (cacheid < 0 || cacheid >= SysCacheSize)
		elog(ERROR, "invalid cache ID: %d", cacheid);

	i = syscache_callback_links[cacheid] - 1;
	while (i >= 0)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		Assert(ccitem->id == cacheid);
		ccitem->function(ccitem->arg, cacheid, hashvalue);
		i = ccitem->link - 1;
	}
}
