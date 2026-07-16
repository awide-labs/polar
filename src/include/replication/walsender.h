/*-------------------------------------------------------------------------
 *
 * walsender.h
 *	  Exports from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_H
#define _WALSENDER_H

#include "access/xlogdefs.h"

/* POLAR */
#include "utils/tuplestore.h"

/*
 * What to do with a snapshot in create replication slot command.
 */
typedef enum
{
	CRS_EXPORT_SNAPSHOT,
	CRS_NOEXPORT_SNAPSHOT,
	CRS_USE_SNAPSHOT,
} CRSSnapshotAction;

/* POLAR: customized replication mode */
typedef enum polar_repl_mode_t
{
	POLAR_REPL_DEFAULT,			/* Default mode, as original one */
	POLAR_REPL_REPLICA,			/* Replica mode */
	POLAR_REPL_STANDBY,			/* Standby mode */
	POLAR_REPL_SA_DATAMAX,		/* Standalone datamax mode */
} polar_repl_mode_t;

/* POLAR end */

/* global state */
extern PGDLLIMPORT bool am_walsender;
extern PGDLLIMPORT bool am_cascading_walsender;
extern PGDLLIMPORT bool am_db_walsender;
extern PGDLLIMPORT bool wake_wal_senders;

/* user-settable parameters */
extern PGDLLIMPORT int max_wal_senders;
extern PGDLLIMPORT int wal_sender_timeout;
extern PGDLLIMPORT bool log_replication_commands;

/* POLAR: GUC */
extern PGDLLIMPORT int polar_max_non_super_wal_snd;
extern PGDLLIMPORT int polar_send_xlog_meta_size;
extern PGDLLIMPORT int polar_logical_repl_xlog_bulk_read_size;
extern PGDLLIMPORT bool polar_enable_read_from_wal_buffers;
extern int	polar_replica_max_wal_sender_timeout;

/* POLAR end */

extern void InitWalSender(void);
extern bool exec_replication_command(const char *cmd_string);
extern void WalSndErrorCleanup(void);
extern void WalSndResourceCleanup(bool isCommit);
extern void PhysicalWakeupLogicalWalSnd(void);
extern XLogRecPtr GetStandbyFlushRecPtr(TimeLineID *tli);
extern void WalSndSignals(void);
extern Size WalSndShmemSize(void);
extern void WalSndShmemInit(void);
extern void WalSndWakeup(bool physical, bool logical);
extern void WalSndInitStopping(void);
extern void WalSndWaitStopping(void);
extern void HandleWalSndInitStopping(void);
extern void WalSndRqstFileReload(void);

/* POLAR */
extern polar_repl_mode_t polar_gen_replication_mode(void);
extern const char *polar_replication_mode_str(polar_repl_mode_t mode);
extern void polar_stat_walsnd_xlog_read(Tuplestorestate *tupstore, TupleDesc tupdesc);
extern void polar_stat_walsnd_reset(void);

/*
 * Remember that we want to wakeup walsenders later
 *
 * This is separated from doing the actual wakeup because the writeout is done
 * while holding contended locks.
 */
#define WalSndWakeupRequest() \
	do { wake_wal_senders = true; } while (0)

/*
 * wakeup walsenders if there is work to be done
 */
static inline void
WalSndWakeupProcessRequests(bool physical, bool logical)
{
	if (wake_wal_senders)
	{
		wake_wal_senders = false;
		if (max_wal_senders > 0)
			WalSndWakeup(physical, logical);
	}
}

#endif							/* _WALSENDER_H */
