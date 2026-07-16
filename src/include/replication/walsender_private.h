/*-------------------------------------------------------------------------
 *
 * walsender_private.h
 *	  Private definitions from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_PRIVATE_H
#define _WALSENDER_PRIVATE_H

#include "access/xlog.h"
#include "lib/ilist.h"
#include "nodes/nodes.h"
#include "nodes/replnodes.h"
#include "replication/syncrep.h"
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/spin.h"

/* POLAR */
#include "port/atomics.h"

/* POLAR: promote-wait subsystem */
#define POLAR_RESET_WALSND_RECEIVE_PROMOTE() (pg_atomic_write_u32(&MyWalSnd->polar_walsender_receive_promote, 0))
#define POLAR_SET_WALSND_RECEIVE_PROMOTE() (pg_atomic_write_u32(&MyWalSnd->polar_walsender_receive_promote, 1))
#define POLAR_WALSND_RECEIVE_PROMOTE() (pg_atomic_read_u32(&MyWalSnd->polar_walsender_receive_promote) == 1)
#define POLAR_WALSNDCTL_RECEIVE_PROMOTE_TRIGGER() (WalSndCtl->polar_receive_promote)
/* POLAR end */

typedef enum WalSndState
{
	WALSNDSTATE_STARTUP = 0,
	WALSNDSTATE_BACKUP,
	WALSNDSTATE_CATCHUP,
	WALSNDSTATE_STREAMING,
	WALSNDSTATE_STOPPING,
} WalSndState;

/*
 * Each walsender has a WalSnd struct in shared memory.
 *
 * This struct is protected by its 'mutex' spinlock field, except that some
 * members are only written by the walsender process itself, and thus that
 * process is free to read those members without holding spinlock.  pid and
 * needreload always require the spinlock to be held for all accesses.
 */
typedef struct WalSnd
{
	pid_t		pid;			/* this walsender's PID, or 0 if not active */

	WalSndState state;			/* this walsender's state */
	XLogRecPtr	sentPtr;		/* WAL has been sent up to this point */
	bool		needreload;		/* does currently-open file need to be
								 * reloaded? */

	/*
	 * The xlog locations that have been written, flushed, and applied by
	 * standby-side. These may be invalid if the standby-side has not offered
	 * values yet.
	 */
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;

	/* Measured lag times, or -1 for unknown/none. */
	TimeOffset	writeLag;
	TimeOffset	flushLag;
	TimeOffset	applyLag;

	/*
	 * The priority order of the standby managed by this WALSender, as listed
	 * in synchronous_standby_names, or 0 if not-listed.
	 */
	int			sync_standby_priority;

	/* Protects shared variables in this structure. */
	slock_t		mutex;

	/*
	 * Pointer to the walsender's latch. Used by backends to wake up this
	 * walsender when it has work to do. NULL if the walsender isn't active.
	 */
	Latch	   *latch;

	/*
	 * Timestamp of the last message received from standby.
	 */
	TimestampTz replyTime;

	ReplicationKind kind;

	/* POLAR: mark current user is superuser or not */
	bool		is_super;

	/* POLAR: mark whether send to replica or not */
	bool		to_replica;

	/* POLAR: mark whether received promote request from downstream */
	pg_atomic_uint32 polar_walsender_receive_promote;

	/*
	 * POLAR: counters for WAL buffer read hit rate.
	 *
	 * Field 'ws_changecount' works as a lock. Before/after updating fields
	 * below it, it needs to be incremented once. Reading fields below from
	 * shared-memory should double check its value before and after reading,
	 * and the values are valid only when 'ws_changecount' hasn't changed and
	 * is even. See comments in structure 'PgBackendStatus'.
	 */
	int			wbr_changecount;

	uint64		wbr_hit;
	uint64		wbr_prefetched;
	uint64		wbr_read;

	uint64		wbr_hit_bytes;
	uint64		wbr_prefetched_bytes;
	uint64		wbr_read_bytes;
	/* POLAR end */
} WalSnd;

extern PGDLLIMPORT WalSnd *MyWalSnd;

/* POLAR: macros for reading/writing WAL sender statistics */
#define WAL_SND_BEGIN_WRITE_STAT(walsnd) \
	do { \
		START_CRIT_SECTION(); \
		(walsnd)->wbr_changecount++; \
		pg_write_barrier(); \
	} while (0)

#define WAL_SND_END_WRITE_STAT(walsnd) \
	do { \
		pg_write_barrier(); \
		(walsnd)->wbr_changecount++; \
		Assert(((walsnd)->wbr_changecount & 1) == 0); \
		END_CRIT_SECTION(); \
	} while (0)

#define WAL_SND_BEGIN_READ_STAT(walsnd, before_changecount) \
	do { \
		(before_changecount) = (walsnd)->wbr_changecount; \
		pg_read_barrier(); \
	} while (0)

#define WAL_SND_END_READ_STAT(walsnd, after_changecount) \
	do { \
		pg_read_barrier(); \
		(after_changecount) = (walsnd)->wbr_changecount; \
	} while (0)

#define WAL_SND_READ_STAT_COMPLETE(before_changecount, after_changecount) \
	((before_changecount) == (after_changecount) && \
	 ((before_changecount) & 1) == 0)
/* POLAR end */

/* There is one WalSndCtl struct for the whole database cluster */
typedef struct
{
	/*
	 * Synchronous replication queue with one queue per request type.
	 * Protected by SyncRepLock.
	 */
	dlist_head	SyncRepQueue[POLAR_NUM_ALL_REP_WAIT_MODE];

	/*
	 * Current location of the head of the queue. All waiters should have a
	 * waitLSN that follows this value. Protected by SyncRepLock.
	 */
	XLogRecPtr	lsn[POLAR_NUM_ALL_REP_WAIT_MODE];

	/*
	 * Status of data related to the synchronous standbys.  Waiting backends
	 * can't reload the config file safely, so checkpointer updates this value
	 * as needed. Protected by SyncRepLock.
	 */
	bits8		sync_standbys_status;

	/* used as a registry of physical / logical walsenders to wake */
	ConditionVariable wal_flush_cv;
	ConditionVariable wal_replay_cv;

	/*
	 * Used by physical walsenders holding slots specified in
	 * synchronized_standby_slots to wake up logical walsenders holding
	 * logical failover slots when a walreceiver confirms the receipt of LSN.
	 */
	ConditionVariable wal_confirm_rcv_cv;

	/* POLAR: Points to the end of the record waiting DDL */
	pg_atomic_uint64 polar_wait_ddl_lsn;

	/* POLAR: set true when any walsender received promote request */
	bool		polar_receive_promote;

	WalSnd		walsnds[FLEXIBLE_ARRAY_MEMBER];
} WalSndCtlData;

/* Flags for WalSndCtlData->sync_standbys_status */

/*
 * Is the synchronous standby data initialized from the GUC?  This is set the
 * first time synchronous_standby_names is processed by the checkpointer.
 */
#define SYNC_STANDBY_INIT			(1 << 0)

/*
 * Is the synchronous standby data defined?  This is set when
 * synchronous_standby_names has some data, after being processed by the
 * checkpointer.
 */
#define SYNC_STANDBY_DEFINED		(1 << 1)

extern PGDLLIMPORT WalSndCtlData *WalSndCtl;


extern void WalSndSetState(WalSndState state);

/*
 * Internal functions for parsing the replication grammar, in repl_gram.y and
 * repl_scanner.l
 */
extern int	replication_yyparse(void);
extern int	replication_yylex(void);
extern void replication_yyerror(const char *message) pg_attribute_noreturn();
extern void replication_scanner_init(const char *str);
extern void replication_scanner_finish(void);
extern bool replication_scanner_is_replication_command(void);

extern PGDLLIMPORT Node *replication_parse_result;

#endif							/* _WALSENDER_PRIVATE_H */
