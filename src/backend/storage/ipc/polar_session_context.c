/*-------------------------------------------------------------------------
 *
 * polar_session_context.c
 *    Polardb session context interface routines.
 *
 *
 * Copyright (c) 2020, Alibaba inc.
 *
 * src/backend/storage/ipc/polar_session_context.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/polar_session_context.h"
#include "storage/polar_memutils.h"
#include "catalog/namespace.h"
#include "utils/memutils.h"
#include "utils/backend_status.h"
#include "utils/portal.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "postmaster/polar_dispatcher.h"
#include "nodes/bitmapset.h"
#include "access/xact.h"

#define SESSION_MAGIC    0xDEFA1234U

/* Pointer to the active session */
PolarSessionContext *polar_current_session = NULL;
PolarSessionContext *polar_private_session = NULL;

static Bitmapset *session_id_bitmapset = NULL;
PolarSessionContext *polar_session_contexts = NULL;

#define GET_LOCAL_ID_FROM_SESSION_ID(session_id)		(session_id % MaxPolarSessionsPerDispatcher)

static MemoryContext polar_get_next_memory_context(MemoryContext context);

static PolarSessionInfo *
polar_session_info_create(MemoryContext mctx)
{
	PolarSessionInfo *info;

	if (mctx != NULL)
	{
		info = (PolarSessionInfo *) MemoryContextAllocZero(mctx, sizeof(PolarSessionInfo));
		info->ns = polar_session_namespace_create(mctx);
		/* info->portalmem = polar_session_portalmem_create(mctx); */
		info->superuser = polar_session_superuser_create(mctx);
		info->acl = polar_session_acl_create(mctx);
	}
	else
	{
		info = (PolarSessionInfo *) malloc(sizeof(PolarSessionInfo));
		memset(info, 0, sizeof(PolarSessionInfo));

		info->ns = polar_session_namespace_create(NULL);
		/* info->portalmem = polar_session_portalmem_create(NULL); */
		info->superuser = polar_session_superuser_create(NULL);
		info->acl = polar_session_acl_create(NULL);
	}

	info->magic = SESSION_MAGIC;
	/* prepared statement */
	dlist_init(&info->m_saved_plan_list);
	dlist_init(&info->m_saved_guc_list);

	return info;
}

PolarSessionContext *
polar_session_create(bool is_shared)
{
	PolarSessionContext *session;
	MemoryContext mctx;

	if (is_shared)
	{
		int			id = polar_session_id_alloc();

		mctx = polar_shmem_alloc_set_context_create_extended(NULL, TopMemoryContext, "polar shared session",
															 ALLOCSET_DEFAULT_SIZES);
		Assert(mctx != NULL);
		session = &polar_session_contexts[id];
		Assert(pg_atomic_read_u32(&session->status) == PSSE_RELEASED);
		memset(session, 0, sizeof(PolarSessionContext));

		session->id = id;
		session->session_id = POLAR_MAX_PROCESS_COUNT + id;

		session->info = polar_session_info_create(mctx);
		session->info->id = id;
	}
	else
	{
		if (POLAR_SHARED_SERVER_RUNNING())
			mctx = AllocSetContextCreate(TopMemoryContext, "polar private session",
										 ALLOCSET_DEFAULT_SIZES);
		else
			mctx = TopMemoryContext;
		session = (PolarSessionContext *) malloc(sizeof(PolarSessionContext));
		memset(session, 0, sizeof(PolarSessionContext));
		session->id = -1;
		session->session_id = MyProcPid;

		session->info = polar_session_info_create(NULL);
		session->info->id = -1;

	}

	pg_atomic_write_u32(&session->status, PSSE_IDLE);
	pg_atomic_write_u32(&session->is_shared_backend_exit, 0);
	pg_atomic_write_u64(&session->finished_command_count, 0);

	session->is_shared = is_shared;
	session->memory_context = mctx;
	session->current_pid = MyProcPid;
	SpinLockInit(&session->holding_lock);
	return session;
}

void
polar_session_delete(PolarSessionContext *session)
{
	if (session->is_shared)
	{
		polar_session_id_free(session->id);
		polar_pgstat_beshutdown(session->id);
		MemoryContextDelete(session->memory_context);
		pg_atomic_write_u32(&session->status, PSSE_RELEASED);
		session->session_id = 0;
		session->info = NULL;
		session->memory_context = NULL;
		session->signal = -1;
	}
}

bool
polar_send_signal(int pid, int *cancelAuthCode, int sig)
{
	int			id = GET_ID_FROM_SESSION_ID(pid);
	int			status;
	PolarSessionContext *session = &polar_session_contexts[id];

	if (!POLAR_IS_SESSION_ID(pid))
		return false;

	if (pg_atomic_read_u32(&session->status) > PSSE_ACTIVE)
		return true;

	if (!session->info)
		return true;

	if (cancelAuthCode != NULL && *cancelAuthCode != session->info->cancel_key)
		return false;

	SpinLockAcquire(&session->holding_lock);
	/* fast handle */
	if ((status = pg_atomic_read_u32(&session->status)) == PSSE_ACTIVE)
	{
		bool		ret = true;

		if (sig > __SIGRTMAX)
		{
			ret = SendProcSignal(session->current_pid, sig - __SIGRTMAX, InvalidBackendId);
		}
		else
		{
#ifdef HAVE_SETSID
			ret = (kill(-session->current_pid, sig) == 0);
#else
			ret = (kill(session->current_pid, sig) == 0);
#endif
		}
		SpinLockRelease(&session->holding_lock);
		ELOG_PSS(LOG, "polar_send_signal active %d %d, %d", pid, session->current_pid, sig);
		return ret;
	}

	/* forward to dispatcher, only active session handle SIGINT */
	if (status == PSSE_IDLE && sig != SIGINT)
	{
		int			dispatcher_id = id / MaxPolarSessionsPerDispatcher;
		int			local_id = id % MaxPolarSessionsPerDispatcher;
		PolarDispatcherProc *dispatcher_proc = &polar_dispatcher_proc[dispatcher_id];

		session->signal = sig;
		SpinLockRelease(&session->holding_lock);

		SpinLockAcquire(&dispatcher_proc->signalling_session_lock);

		if (!bms_is_member(local_id, dispatcher_proc->signalling_sessions))
		{
			polar_bms_add_member(dispatcher_proc->signalling_sessions, local_id);
			dispatcher_proc->n_signalling_clients++;
		}

		Assert(bms_num_members(dispatcher_proc->signalling_sessions) ==
			   dispatcher_proc->n_signalling_clients);

		SpinLockRelease(&dispatcher_proc->signalling_session_lock);
		ELOG_PSS(LOG, "polar_send_signal inactive %d %d, %d, %d",
				 pid, id, dispatcher_proc->n_signalling_clients, sig);
		return true;
	}
	SpinLockRelease(&session->holding_lock);
	return true;
}

bool
polar_get_session_roleid(int pid, Oid *roleid)
{
	int			id = GET_ID_FROM_SESSION_ID(pid);
	PolarSessionContext *session = &polar_session_contexts[id];

	if (!POLAR_IS_SESSION_ID(pid))
		return false;

	if (!session->info || pg_atomic_read_u32(&session->status) > PSSE_ACTIVE)
		return false;

	*roleid = session->info->roleid;

	return true;
}

void
polar_private_session_initialize(void)
{
	Assert(polar_private_session == NULL);
	if (!polar_private_session)
	{
		polar_private_session = polar_session_create(false);
		polar_current_session = polar_private_session;
		pg_atomic_write_u32(&polar_current_session->status, PSSE_ACTIVE);
	}
}


void
polar_session_id_generator_initialize(void)
{
	session_id_bitmapset = bms_add_range(NULL, 0, MaxPolarSessionsPerDispatcher - 1);
}

bool
polar_session_id_full(void)
{
	return bms_is_empty(session_id_bitmapset);
}

int
polar_session_id_alloc(void)
{
	int			result;
	static int	last_session_id = 0;

	Assert(IsPolarDispatcher);
	result = bms_next_member(session_id_bitmapset, last_session_id);
	if (result < 0)
	{
		/* retry from first member */
		result = bms_first_member(session_id_bitmapset);
	}

	Assert(result >= 0);
	Assert(result < MaxPolarSessionsPerDispatcher);

	if (bms_is_member(result, session_id_bitmapset))
		bms_del_member(session_id_bitmapset, result);

	last_session_id = result;
	return result + polar_my_dispatcher_id * MaxPolarSessionsPerDispatcher;
}

void
polar_session_id_free(int id)
{
	Assert(IsPolarDispatcher);
	bms_add_member(session_id_bitmapset, GET_LOCAL_ID_FROM_SESSION_ID(id));
}

/*
 * The MurmurHash 2 from Austin Appleby, faster and better mixed (but weaker
 * crypto-wise with one pair of obvious differential) than both Lookup3 and
 * SuperFastHash. Not-endian neutral for speed
 */
uint32
polar_murmurhash2(const void *key, int32_t len, uint32 seed)
{
	/* 'm' and 'r' are mixing constants generated offline. */
	/* They're not really 'magic', they just happen to work well. */
	const uint32 m = 0x5bd1e995;
	const int32_t r = 24;

	/* Initialize the hash to a 'random' value */
	uint32		h = seed ^ len;

	/* Mix 4 bytes at a time into the hash */
	const unsigned char *data = (const unsigned char *) (key);

	while (len >= 4)
	{
		uint32		k =
			((uint32) data[0]) | (((uint32) data[1]) << 8) |
			(((uint32) data[2]) << 16) | (((uint32) data[3]) << 24);

		k *= m;
		k ^= k >> r;
		k *= m;

		h *= m;
		h ^= k;

		data += 4;
		len -= 4;
	}

	/* Handle the last few bytes of the input array */
	switch (len)
	{
		case 3:
			h ^= data[2] << 16;
			/* FALLTHROUGH */
		case 2:
			h ^= data[1] << 8;
			/* FALLTHROUGH */
		case 1:
			h ^= data[0];
			h *= m;
	};

	/* Do a few final mixes of the hash to ensure the last few */
	/* bytes are well-incorporated. */
	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return h;
}

MemoryContext
polar_shmem_alloc_set_context_create_extended(MemoryContext parent,
											  MemoryContext parent_fallback,
											  const char *name,
											  Size minContextSize,
											  Size initBlockSize,
											  Size maxBlockSize)
{
	MemoryContext ret;

	if (IsPolarDispatcher || POLAR_SS_NOT_DEDICATED())
	{
		ELOG_PSS(LOG, "parent(%d-%s-%s), parent_fallback(%s-%s), name(%s)",
				 parent ? parent->type : 0,
				 parent ? parent->name : "null",
				 parent && parent->ident ? parent->ident : "null",
				 parent_fallback->name ? parent_fallback->name : "null",
				 parent_fallback->ident ? parent_fallback->ident : "null",
				 name ? name : "null");

		ret = ShmAllocSetContextCreate(parent,
									   name,
									   minContextSize,
									   initBlockSize,
									   maxBlockSize,
									   &shm_aset_ctl[SHM_ASET_TYPE_SHARED_SERVER]);

		if (ret == NULL)
		{
			if (parent_fallback != NULL)
			{
				MyProc->polar_is_backend_dedicated = true;
				elog(LOG, "polar shared server set dedicated from shared memory oom(%d-%s-%s), use (%s-%s) instead",
					 parent ? parent->type : 0, parent ? parent->name : "null",
					 parent && parent->ident ? parent->ident : "null",
					 parent_fallback->name ? parent_fallback->name : "null",
					 parent_fallback->ident ? parent_fallback->ident : "null");

				ret = AllocSetContextCreateExtended(parent_fallback,
													name,
													minContextSize,
													initBlockSize,
													maxBlockSize);
			}
		}
		else
		{
			ret->fallback.parent_name = parent_fallback->name;
			ret->fallback.parent_ident = parent_fallback->ident;
			ret->fallback.minContextSize = minContextSize;
			ret->fallback.initBlockSize = initBlockSize;
			ret->fallback.maxBlockSize = maxBlockSize;
			ret->fallback.mcxt = NULL;
		}
	}
	else
	{
		return AllocSetContextCreateExtended(parent_fallback,
											 name,
											 minContextSize,
											 initBlockSize,
											 maxBlockSize);
	}

	return ret;
}

MemoryContext
polar_session_alloc_set_context_create(MemoryContext parent,
									   MemoryContext parent_fallback,
									   const char *name,
									   Size minContextSize,
									   Size initBlockSize,
									   Size maxBlockSize)
{
	if (POLAR_SS_NOT_DEDICATED())
		return polar_shmem_alloc_set_context_create_extended(parent,
															 parent_fallback,
															 name,
															 minContextSize,
															 initBlockSize,
															 maxBlockSize);
	else
		return AllocSetContextCreateExtended(parent_fallback,
											 name,
											 minContextSize,
											 initBlockSize,
											 maxBlockSize);
}


bool
polar_set_dedicate_from_oom(MemoryContext context)
{
	if (POLAR_SS_NOT_DEDICATED() &&
		context->type == T_ShmAllocSetContext)
	{
		MyProc->polar_is_backend_dedicated = true;
		elog(LOG, "polar shared server set dedicated from shared memory oom(%d-%s-%s)",
			 context->type, context->name, context->ident ? context->ident : "null");
		return true;
	}
	return false;
}

bool
polar_get_local_memory_context(MemoryContext context)
{
	MemoryContext local_context = TopMemoryContext;

	if (context->fallback.mcxt != NULL)
		return true;

	if (context->fallback.parent_name == NULL)
		return false;

	while (NULL != local_context && MemoryContextIsValid(local_context))
	{
		if (local_context->type == T_AllocSetContext &&
			local_context->name != NULL &&
			strcmp(local_context->name, context->name) == 0 &&
			((context->ident == NULL && local_context->ident == NULL) ||
			 (context->ident != NULL &&
			  local_context->ident != NULL &&
			  strcmp(local_context->ident, context->ident) == 0))
			)
		{
			context->fallback.mcxt = local_context;
			elog(LOG, "polar shared server use falllback memory %p, (%d-%s-%s)(%d-%s-%s), %s",
				 context->fallback.mcxt,
				 local_context->type, local_context->name, local_context->ident ? local_context->ident : "null",
				 context->type, context->name, context->ident ? context->ident : "null",
				 polar_get_backtrace()
				);
			return true;
		}
		local_context = polar_get_next_memory_context(local_context);
	}

	if (!IsPolarDispatcher &&
		context->fallback.initBlockSize > 0)
	{
		local_context = TopMemoryContext;
		while (NULL != local_context && MemoryContextIsValid(local_context))
		{
			if (local_context->type == T_AllocSetContext &&
				local_context->name != NULL &&
				strcmp(local_context->name, context->fallback.parent_name) == 0 &&
				((context->fallback.parent_ident == NULL && local_context->ident == NULL) ||
				 (context->fallback.parent_ident != NULL &&
				  local_context->ident != NULL &&
				  strcmp(local_context->ident, context->fallback.parent_ident) == 0))
				)
			{
				context->fallback.mcxt = AllocSetContextCreateExtended(local_context,
																	   context->name,
																	   context->fallback.minContextSize,
																	   context->fallback.initBlockSize,
																	   context->fallback.maxBlockSize);
				elog(LOG, "polar shared server use falllback memory from new %p,(%d-%s-%s)(%s-%s)(%d-%s-%s), %s",
					 context->fallback.mcxt,
					 context->type, context->name, context->ident ? context->ident : "null",
					 context->fallback.parent_name, context->fallback.parent_ident ? context->fallback.parent_ident : "null",
					 local_context->type, local_context->name, local_context->ident ? local_context->ident : "null",
					 polar_get_backtrace()
					);
				return true;
			}
			local_context = polar_get_next_memory_context(local_context);
		}
	}

	elog(LOG, "not find context with given falllback memory(%d-%s-%s)(%s-%s), %s",
		 context->type, context->name, context->ident ? context->ident : "null",
		 context->fallback.parent_name, context->fallback.parent_ident ? context->fallback.parent_ident : "null",
		 polar_get_backtrace()
		);

	context->fallback.parent_name = NULL;
	return false;
}

void
polar_delete_local_memory_context(MemoryContext context)
{
	MemoryContext last_context = context;

	while (NULL != context && MemoryContextIsValid(context))
	{
		if (context->type != T_ShmAllocSetContext)
		{
			elog(LOG, "polar_delete_local_memory_context(%s-%s)(%s-%s) before exist",
				 context->name, context->ident ? context->ident : "null",
				 last_context->name, last_context->ident ? last_context->ident : "null"
				);
			MemoryContextDelete(context);
			context = last_context;
		}
		last_context = context;
		context = polar_get_next_memory_context(context);
	}
}

MemoryContext
polar_get_next_memory_context(MemoryContext context)
{
	MemoryContext ret = NULL;

	AssertArg(MemoryContextIsValid(context));

	if (context->firstchild)
	{
		/* perfor first-depth search */
		ret = context->firstchild;
	}
	else if (context->nextchild)
	{
		/* goto next child if current context doesn't have a child */
		ret = context->nextchild;
	}
	else if (context->parent)
	{
		/*
		 * walk up on tree to first parent which has a next child, that parent
		 * context was already visited
		 */
		while (context)
		{
			context = context->parent;

			if (context == NULL)
			{
				/* we visited the whole context's tree */
				ret = NULL;
				break;
			}
			else if (context->nextchild)
			{
				ret = context->nextchild;
				break;
			}
		}
	}
	AssertArg(!ret || MemoryContextIsValid(ret));
	return ret;
}

static void
polar_check_memory_context(PolarSessionContext *session)
{
	MemoryContext context = session->memory_context;
	MemoryContext last_context = context;

	while (NULL != context && MemoryContextIsValid(context))
	{
		if (context->type != T_ShmAllocSetContext)
		{
			MyProc->polar_is_backend_dedicated = true;
			elog(LOG, "polar shared server set dedicated from local memory(%s-%s)(%s-%s) under shared memory used after transaction finish",
				 context->name, context->ident ? context->ident : "null",
				 last_context->name, last_context->ident ? last_context->ident : "null"
				);
			return;
		}
		last_context = context;
		context = polar_get_next_memory_context(context);
	}

	context = TopMemoryContext;
	while (NULL != context && MemoryContextIsValid(context))
	{
		if (context->type == T_ShmAllocSetContext)
		{
			MyProc->polar_is_backend_dedicated = true;
			elog(LOG, "polar shared server set dedicated from shared memory(%s-%s)(%s-%s) under local memory used after transaction finish",
				 context->name, context->ident ? context->ident : "null",
				 last_context->name, last_context->ident ? last_context->ident : "null"
				);
			return;
		}

		last_context = context;
		context = polar_get_next_memory_context(context);
	}
}

bool
polar_need_switch_session(void)
{
	if (!POLAR_SHARED_SERVER_RUNNING())
		return false;

	if (IS_POLAR_SESSION_SHARED())
	{
		if (!IS_POLAR_BACKEND_SHARED())
			return false;

		if (IsInTransactionBlock(true))
			return false;

		polar_check_memory_context(polar_session());

		if (!IS_POLAR_BACKEND_SHARED())
			return false;
	}
	return true;
}


void
polar_switch_to_session(PolarSessionContext *session)
{
	if (polar_current_session != session)
	{
		PolarSessionContext *old_session = polar_current_session;

		ELOG_PSS(DEBUG1, "polar_switch_to_session %p->%p, %d->%d, ntrans:%lu->%lu",
				 old_session, session,
				 old_session->session_id, session->session_id,
				 old_session->n_transactions, session->n_transactions);


		PG_TRY();
		{
			if (old_session->is_shared)
				SpinLockAcquire(&old_session->holding_lock);
			else
				SpinLockAcquire(&session->holding_lock);

			CHECK_FOR_INTERRUPTS();

			if (old_session->is_shared)
			{
				if (!old_session->will_close &&
					!polar_session_guc_restore(old_session, false))
				{
					SpinLockRelease(&old_session->holding_lock);
					return;
				}
				session->current_pid = -1;
				MySessionPid = MyProcPid;
			}

			polar_current_session = session;
			pg_atomic_write_u32(&old_session->status, PSSE_IDLE);
			pg_atomic_write_u32(&session->status, PSSE_ACTIVE);

			if (session->is_shared)
			{
				session->current_pid = MyProcPid;
				MySessionPid = session->session_id;

				polar_session_info_init(session, old_session);
				polar_session_guc_restore(session, true);

				if (polar_ss_session_schedule_policy == SESSION_SCHEDULE_DEDICATED &&
					POLAR_SS_NOT_DEDICATED())
				{
					MyProc->polar_is_backend_dedicated = true;
					elog(LOG, "polar shared server set dedicated from polar_ss_session_schedule_policy");
				}
			}
			if (old_session->is_shared)
				SpinLockRelease(&old_session->holding_lock);
			else
				SpinLockRelease(&session->holding_lock);
		}
		PG_CATCH();
		{
			if (old_session->is_shared)
				SpinLockRelease(&old_session->holding_lock);
			else
				SpinLockRelease(&session->holding_lock);
			PG_RE_THROW();
		}
		PG_END_TRY();

	}
}
