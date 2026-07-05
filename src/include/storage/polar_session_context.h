/*-------------------------------------------------------------------------
 *
 * polar_session_context.h
 *    Polardb session context definitions. Session-related data is aggregated into the structure.
 *
 *
 * Copyright (c) 2020, Alibaba inc.
 *
 * src/include/storage/polar_session_context.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_SESSION_CONTEXT_H
#define POLAR_SESSION_CONTEXT_H

#include "postgres.h"
#include "lib/ilist.h"
#include "storage/lwlock.h"
#include "storage/s_lock.h"
#include "port/atomics.h"


#define POLAR_MAX_PROCESS_COUNT					10000000
#define GET_ID_FROM_SESSION_ID(session_id)		(session_id - POLAR_MAX_PROCESS_COUNT)

typedef struct PolarSessionInfo
{
	uint32		magic;			/* Magic to validate content of session object */
	int			id;

	bool		is_inited;
	int			cancel_key;
	Oid			roleid;

	struct Port *client_port;
	struct ClientChannel *client;

	/* plancache, from plancache.c */
	dlist_head	m_saved_plan_list;

	/* parepared statement, from prepare.c, postgres.c */
	struct HTAB *m_prepared_queries;

	/* portal, from portalmem.c */
	/* struct PolarSessionPortalMem *portalmem; */

	/* superuser, from superuser.c */
	struct PolarSessionSuperUser *superuser;

	/* namespace, from namespace.c */
	struct PolarSessionNamespace *ns;

	/* acl, from acl.c */
	struct PolarSessionAcl *acl;

	/* sequence, from sequence.c  */
	struct HTAB *m_seqhashtab;
	struct SeqTableData *m_last_used_seq;

	/* invalidation message */
	struct PolarDispatcherSISeg *shm_inval_buffer;
	int			nextMsgNum;

	/* local parameter, from guc.c */
	dlist_head	m_saved_guc_list;
	int			saved_guc_count;
	bool		m_ConfigReloadPending;
	bool		m_reporting_enabled;

	/* other local variables */
	LocalTransactionId m_nextLocalTransactionId;
} PolarSessionInfo;

enum PolarSessionStatusEnum
{
	PSSE_IDLE = 0,
	PSSE_ACTIVE,
	PSSE_CLOSED,
	PSSE_RELEASED,
};

typedef struct PolarSessionContext
{
	int			id;
	int			session_id;
	pid_t		current_pid;
	bool		is_shared;
	uint64		n_transactions; /* total number of proroceeded transactions */
	int64		last_wait_start_timestamp;	/* time of last backend activity */

	int			signal;
	int			will_close;

	pg_atomic_uint32 status;
	pg_atomic_uint32 is_shared_backend_exit;
	pg_atomic_uint64 finished_command_count;

	/*
	 * Memory context used for global session data (instead of
	 * TopMemoryContext)
	 */
	MemoryContext memory_context;

	slock_t		holding_lock;
	PolarSessionInfo *info;
} PolarSessionContext;

extern PolarSessionContext *polar_session_contexts;
extern PolarSessionContext *polar_current_session;
extern PolarSessionContext *polar_private_session;
extern bool polar_enable_shared_server;
extern bool polar_enable_shared_server_log;
extern bool polar_enable_shared_server_testmode;
extern bool polar_enable_shared_server_hang;
extern int	polar_ss_shared_memory_size;
extern bool polar_enable_shm_aset;


static inline PolarSessionContext *
polar_session(void)
{
	return polar_current_session;
}

static inline PolarSessionInfo *
polar_session_info(void)
{
	return polar_current_session->info;
}

#define POLAR_SESSION(name) \
	(polar_current_session->info->m_##name)

#define IS_POLAR_SESSION_SHARED() \
	(polar_current_session->is_shared)

#define IS_POLAR_BACKEND_SHARED() \
	(MyProc && !MyProc->polar_is_backend_dedicated)

#define POLAR_SS_NOT_DEDICATED() \
	(IS_POLAR_SESSION_SHARED() && IS_POLAR_BACKEND_SHARED())

#define POLAR_SHARED_SERVER_RUNNING() \
	(polar_enable_shm_aset > 0 && polar_ss_shared_memory_size > 0)

#define POLAR_SHARED_SERVER_ENABLE() \
	(POLAR_SHARED_SERVER_RUNNING() && polar_enable_shared_server)

extern PolarSessionContext *polar_session_create(bool is_shared);
extern void polar_session_delete(PolarSessionContext *session);
extern void polar_private_session_initialize(void);
extern void polar_session_info_init(struct PolarSessionContext *session, PolarSessionContext *old_session);
extern bool polar_need_switch_session(void);
extern void polar_switch_to_session(PolarSessionContext *session);
extern bool polar_get_session_roleid(int pid, Oid *roleid);
extern bool polar_send_signal(int pid, int *cancelAuthCode, int sig);
extern uint32 polar_murmurhash2(const void *key, int32_t len, uint32 seed);
extern MemoryContext polar_shmem_alloc_set_context_create_extended(MemoryContext parent,
																   MemoryContext parent_fallback,
																   const char *name,
																   Size minContextSize,
																   Size initBlockSize,
																   Size maxBlockSize);

extern MemoryContext polar_session_alloc_set_context_create(MemoryContext parent,
															MemoryContext parent_fallback,
															const char *name,
															Size minContextSize,
															Size initBlockSize,
															Size maxBlockSize);

extern struct PolarSessionNamespace *polar_session_namespace_create(MemoryContext mctx);

/*  extern struct PolarSessionPortalMem	*polar_session_portalmem_create(MemoryContext mctx); */
extern struct PolarSessionSuperUser *polar_session_superuser_create(MemoryContext mctx);
extern struct PolarSessionAcl *polar_session_acl_create(MemoryContext mctx);

extern bool polar_session_guc_restore(PolarSessionContext *session, bool to_session);
extern void polar_session_guc_release(PolarSessionContext *session);
extern void polar_check_extention_dedicated(const char *extention_name);
extern bool polar_check_dbuser_dedicated(const char *dbname, const char *username);
extern void polar_session_accept_invalidation_messages(PolarSessionContext *session);

extern void polar_session_id_generator_initialize(void);
extern bool polar_session_id_full(void);
extern int	polar_session_id_alloc(void);
extern void polar_session_id_free(int id);
extern bool polar_set_dedicate_from_oom(MemoryContext context);
extern bool polar_get_local_memory_context(MemoryContext context);
extern void polar_delete_local_memory_context(MemoryContext context);

struct config_generic;

#endif							/* POLAR_SESSION_CONTEXT_H */
