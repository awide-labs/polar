/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES buffer manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include "port/pg_iovec.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

/* POLAR: for code compile */
typedef struct BufferDesc BufferDesc;

typedef void *Block;

/*
 * Possible arguments for GetAccessStrategy().
 *
 * If adding a new BufferAccessStrategyType, also add a new IOContext so
 * IO statistics using this strategy are tracked.
 */
typedef enum BufferAccessStrategyType
{
	BAS_NORMAL,					/* Normal random access */
	BAS_BULKREAD,				/* Large read-only scan (hint bit updates are
								 * ok) */
	BAS_BULKWRITE,				/* Large multi-block write (e.g. COPY IN) */
	BAS_VACUUM,					/* VACUUM */
} BufferAccessStrategyType;

/* Possible modes for ReadBufferExtended() */
typedef enum
{
	RBM_NORMAL,					/* Normal read */
	RBM_ZERO_AND_LOCK,			/* Don't read from disk, caller will
								 * initialize. Also locks the page. */
	RBM_ZERO_AND_CLEANUP_LOCK,	/* Like RBM_ZERO_AND_LOCK, but locks the page
								 * in "cleanup" mode */
	RBM_ZERO_ON_ERROR,			/* Read, but return an all-zeros page on error */
	RBM_NORMAL_NO_LOG,			/* Don't log page as invalid during WAL
								 * replay; otherwise same as RBM_NORMAL */
	RBM_NORMAL_VALID,			/* POLAR: Don't read from disk, buffer is
								 * valid; otherwise same as RBM_NORMAL */
	RBM_NORMAL_VALID_NO_LOG,	/* POLAR: Don't log page as invalid during WAL
								 * replay; otherwise same as RBM_NORMAL_VALID */
} ReadBufferMode;

/*
 * Type returned by PrefetchBuffer().
 */
typedef struct PrefetchBufferResult
{
	Buffer		recent_buffer;	/* If valid, a hit (recheck needed!) */
	bool		initiated_io;	/* If true, a miss resulting in async I/O */
} PrefetchBufferResult;

/*
 * Flags influencing the behaviour of ExtendBufferedRel*
 */
typedef enum ExtendBufferedFlags
{
	/*
	 * Don't acquire extension lock. This is safe only if the relation isn't
	 * shared, an access exclusive lock is held or if this is the startup
	 * process.
	 */
	EB_SKIP_EXTENSION_LOCK = (1 << 0),

	/* Is this extension part of recovery? */
	EB_PERFORMING_RECOVERY = (1 << 1),

	/*
	 * Should the fork be created if it does not currently exist? This likely
	 * only ever makes sense for relation forks.
	 */
	EB_CREATE_FORK_IF_NEEDED = (1 << 2),

	/* Should the first (possibly only) return buffer be returned locked? */
	EB_LOCK_FIRST = (1 << 3),

	/* Should the smgr size cache be cleared? */
	EB_CLEAR_SIZE_CACHE = (1 << 4),

	/* internal flags follow */
	EB_LOCK_TARGET = (1 << 5),
}			ExtendBufferedFlags;

/*
 * Some functions identify relations either by relation or smgr +
 * relpersistence.  Used via the BMR_REL()/BMR_SMGR() macros below.  This
 * allows us to use the same function for both recovery and normal operation.
 */
typedef struct BufferManagerRelation
{
	Relation	rel;
	struct SMgrRelationData *smgr;
	char		relpersistence;
} BufferManagerRelation;

#define BMR_REL(p_rel) ((BufferManagerRelation){.rel = p_rel})
#define BMR_SMGR(p_smgr, p_relpersistence) ((BufferManagerRelation){.smgr = p_smgr, .relpersistence = p_relpersistence})

/* Zero out page if reading fails. */
#define READ_BUFFERS_ZERO_ON_ERROR (1 << 0)
/* Call smgrprefetch() if I/O necessary. */
#define READ_BUFFERS_ISSUE_ADVICE (1 << 1)

/* POLAR redo action */
typedef enum polar_redo_action
{
	POLAR_REDO_NO_ACTION,
	POLAR_REDO_REPLAY_XLOG,
	POLAR_REDO_MARK_OUTDATE
} polar_redo_action;

/* POLAR end */

struct ReadBuffersOperation
{
	/*
	 * The following members should be set by the caller.  If only smgr is
	 * provided without rel, then smgr_persistence can be set to override the
	 * default assumption of RELPERSISTENCE_PERMANENT.
	 */
	Relation	rel;
	struct SMgrRelationData *smgr;
	char		smgr_persistence;
	ForkNumber	forknum;
	BufferAccessStrategy strategy;

	/*
	 * The following private members are private state for communication
	 * between StartReadBuffers() and WaitReadBuffers(), initialized only if
	 * an actual read is required, and should not be modified.
	 */
	Buffer	   *buffers;
	BlockNumber blocknum;
	int			flags;
	int16		nblocks;
	int16		io_buffers_len;

	/* POLAR: redo information and repeat read information */
	polar_redo_action polar_replay_action;
	XLogRecPtr	polar_replay_from;
	XLogRecPtr	polar_replay_redo_lsn;
	uint64		polar_repeat_read_times;
};

typedef struct ReadBuffersOperation ReadBuffersOperation;

/* forward declared, to avoid having to expose buf_internals.h here */
struct WritebackContext;

/* forward declared, to avoid including smgr.h here */
struct SMgrRelationData;

/* in globals.c ... this duplicates miscadmin.h */
extern PGDLLIMPORT int NBuffers;

/* in bufmgr.c */
extern PGDLLIMPORT bool zero_damaged_pages;
extern PGDLLIMPORT int bgwriter_lru_maxpages;
extern PGDLLIMPORT double bgwriter_lru_multiplier;
extern PGDLLIMPORT bool track_io_timing;

/* only applicable when prefetching is available */
#ifdef USE_PREFETCH
#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 1
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 10
#else
#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 0
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 0
#endif
extern PGDLLIMPORT int effective_io_concurrency;
extern PGDLLIMPORT int maintenance_io_concurrency;

#define MAX_IO_COMBINE_LIMIT PG_IOV_MAX
#define DEFAULT_IO_COMBINE_LIMIT Min(MAX_IO_COMBINE_LIMIT, (128 * 1024) / BLCKSZ)

extern PGDLLIMPORT int io_combine_limit;

extern PGDLLIMPORT int checkpoint_flush_after;
extern PGDLLIMPORT int backend_flush_after;
extern PGDLLIMPORT int bgwriter_flush_after;

/* POLAR */
extern PGDLLIMPORT bool polar_rsc_optimize_drop_buffers;
extern PGDLLIMPORT bool polar_rsc_optimize_rel_size_udf;

/* POLAR end */

/* in buf_init.c */
extern PGDLLIMPORT char *BufferBlocks;
extern char *polar_checksum_copy_slot(void);

/* in localbuf.c */
extern PGDLLIMPORT int NLocBuffer;
extern PGDLLIMPORT Block *LocalBufferBlockPointers;
extern PGDLLIMPORT int32 *LocalRefCount;

/* upper limit for effective_io_concurrency */
#define MAX_IO_CONCURRENCY 1000

/* special block number for ReadBuffer() */
#define P_NEW	InvalidBlockNumber	/* grow the file to get a new page */

/*
 * Buffer content lock modes (mode argument for LockBuffer())
 */
#define BUFFER_LOCK_UNLOCK		0
#define BUFFER_LOCK_SHARE		1
#define BUFFER_LOCK_EXCLUSIVE	2

/*
 * Note: these two macros only work on shared buffers, not local ones!
 *
 * POLAR: these two macros are moved from bufmgr.c.
 */
#define BufHdrGetBlock(bufHdr)	((Block) (BufferBlocks + ((Size) (bufHdr)->buf_id) * BLCKSZ))
#define BufferGetLSN(bufHdr)	(PageGetLSN(BufHdrGetBlock(bufHdr)))

/*
 * prototypes for functions in bufmgr.c
 */
extern PrefetchBufferResult PrefetchSharedBuffer(struct SMgrRelationData *smgr_reln,
												 ForkNumber forkNum,
												 BlockNumber blockNum);
extern PrefetchBufferResult PrefetchBuffer(Relation reln, ForkNumber forkNum,
										   BlockNumber blockNum);
extern bool ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum,
							 BlockNumber blockNum, Buffer recent_buffer);
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern Buffer ReadBufferExtended(Relation reln, ForkNumber forkNum,
								 BlockNumber blockNum, ReadBufferMode mode,
								 BufferAccessStrategy strategy);
extern Buffer ReadBufferWithoutRelcache(RelFileLocator rlocator,
										ForkNumber forkNum, BlockNumber blockNum,
										ReadBufferMode mode, BufferAccessStrategy strategy,
										bool permanent);

extern bool StartReadBuffer(ReadBuffersOperation *operation,
							Buffer *buffer,
							BlockNumber blocknum,
							int flags);
extern bool StartReadBuffers(ReadBuffersOperation *operation,
							 Buffer *buffers,
							 BlockNumber blockNum,
							 int *nblocks,
							 int flags);
extern void WaitReadBuffers(ReadBuffersOperation *operation);

extern void ReleaseBuffer(Buffer buffer);
extern void UnlockReleaseBuffer(Buffer buffer);
extern bool BufferIsExclusiveLocked(Buffer buffer);
extern bool BufferIsDirty(Buffer buffer);
extern void MarkBufferDirty(Buffer buffer);
extern void IncrBufferRefCount(Buffer buffer);
extern void CheckBufferIsPinnedOnce(Buffer buffer);
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
								   BlockNumber blockNum);

extern Buffer ExtendBufferedRel(BufferManagerRelation bmr,
								ForkNumber forkNum,
								BufferAccessStrategy strategy,
								uint32 flags);
extern BlockNumber ExtendBufferedRelBy(BufferManagerRelation bmr,
									   ForkNumber fork,
									   BufferAccessStrategy strategy,
									   uint32 flags,
									   uint32 extend_by,
									   Buffer *buffers,
									   uint32 *extended_by);
extern Buffer ExtendBufferedRelTo(BufferManagerRelation bmr,
								  ForkNumber fork,
								  BufferAccessStrategy strategy,
								  uint32 flags,
								  BlockNumber extend_to,
								  ReadBufferMode mode);

extern void InitBufferPoolAccess(void);
extern void AtEOXact_Buffers(bool isCommit);
#ifdef USE_ASSERT_CHECKING
extern void AssertBufferLocksPermitCatalogRead(void);
#endif
extern char *DebugPrintBufferRefcount(Buffer buffer);
extern void CheckPointBuffers(int flags);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocksInFork(Relation relation,
												   ForkNumber forkNum);
extern void FlushOneBuffer(Buffer buffer);
extern void FlushRelationBuffers(Relation rel);
extern void FlushRelationsAllBuffers(struct SMgrRelationData **smgrs, int nrels);
extern void CreateAndCopyRelationData(RelFileLocator src_rlocator,
									  RelFileLocator dst_rlocator,
									  bool permanent);
extern void FlushDatabaseBuffers(Oid dbid);
extern void DropRelationBuffers(struct SMgrRelationData *smgr_reln,
								ForkNumber *forkNum,
								int nforks, BlockNumber *firstDelBlock);
extern void DropRelationsAllBuffers(struct SMgrRelationData **smgr_reln,
									int nlocators);
extern void DropDatabaseBuffers(Oid dbid);

#define RelationGetNumberOfBlocks(reln) \
	RelationGetNumberOfBlocksInFork(reln, MAIN_FORKNUM)

extern bool BufferIsPermanent(Buffer buffer);
extern XLogRecPtr BufferGetLSNAtomic(Buffer buffer);

#ifdef NOT_USED
extern void PrintPinnedBufs(void);
#endif
extern void BufferGetTag(Buffer buffer, RelFileLocator *rlocator,
						 ForkNumber *forknum, BlockNumber *blknum);

extern void MarkBufferDirtyHint(Buffer buffer, bool buffer_std);

extern void UnlockBuffers(void);
extern void LockBuffer(Buffer buffer, int mode);
extern bool ConditionalLockBuffer(Buffer buffer);
extern void LockBufferForCleanup(Buffer buffer);
extern bool ConditionalLockBufferForCleanup(Buffer buffer);
extern bool IsBufferCleanupOK(Buffer buffer);
extern bool HoldingBufferPinThatDelaysRecovery(void);

extern bool BgBufferSync(struct WritebackContext *wb_context, int flags);

extern void LimitAdditionalPins(uint32 *additional_pins);
extern void LimitAdditionalLocalPins(uint32 *additional_pins);

extern bool EvictUnpinnedBuffer(Buffer buf, bool *buffer_flushed);
extern void EvictAllUnpinnedBuffers(int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);
extern void EvictRelUnpinnedBuffers(Relation rel,
									int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);
extern bool MarkDirtyUnpinnedBuffer(Buffer buf, bool *buffer_already_dirty);
extern void MarkDirtyRelUnpinnedBuffers(Relation rel,
										int32 *buffers_dirtied,
										int32 *buffers_already_dirty,
										int32 *buffers_skipped);
extern void MarkDirtyAllUnpinnedBuffers(int32 *buffers_dirtied,
										int32 *buffers_already_dirty,
										int32 *buffers_skipped);

/* in buf_init.c */
extern void InitBufferPool(void);
extern Size BufferShmemSize(void);

/* in localbuf.c */
extern void AtProcExit_LocalBuffers(void);

/* in freelist.c */

extern BufferAccessStrategy GetAccessStrategy(BufferAccessStrategyType btype);
extern BufferAccessStrategy GetAccessStrategyWithSize(BufferAccessStrategyType btype,
													  int ring_size_kb);
extern int	GetAccessStrategyBufferCount(BufferAccessStrategy strategy);
extern int	GetAccessStrategyPinLimit(BufferAccessStrategy strategy);

extern void FreeAccessStrategy(BufferAccessStrategy strategy);

/* POLAR */
extern void PolarMarkBufferDirty(Buffer buffer, XLogRecPtr oldest_lsn);
extern bool polar_try_to_wake_bgwriter(void);
extern bool polar_start_buffer_io_extend(BufferDesc *buf,
										 bool forInput,
										 bool nowait,
										 bool polar_copy_buf);

/* POLAR: change static to extern */
extern void TerminateBufferIO(BufferDesc *buf, bool clear_dirty,
							  uint32 set_flag_bits, bool forget_owner);

extern Buffer polar_read_buffer_common(struct SMgrRelationData *smgr, char relpersistence, ForkNumber forkNum,
									   BlockNumber blockNum, ReadBufferMode mode,
									   BufferAccessStrategy strategy);

/* inline functions */

/*
 * Although this header file is nominally backend-only, certain frontend
 * programs like pg_waldump include it.  For compilers that emit static
 * inline functions even when they're unused, that leads to unsatisfied
 * external references; hence hide these with #ifndef FRONTEND.
 */

#ifndef FRONTEND

/*
 * BufferIsValid
 *		True iff the given buffer number is valid (either as a shared
 *		or local buffer).
 *
 * Note: For a long time this was defined the same as BufferIsPinned,
 * that is it would say False if you didn't hold a pin on the buffer.
 * I believe this was bogus and served only to mask logic errors.
 * Code should always know whether it has a buffer reference,
 * independently of the pin state.
 *
 * Note: For a further long time this was not quite the inverse of the
 * BufferIsInvalid() macro, in that it also did sanity checks to verify
 * that the buffer number was in range.  Most likely, this macro was
 * originally intended only to be used in assertions, but its use has
 * since expanded quite a bit, and the overhead of making those checks
 * even in non-assert-enabled builds can be significant.  Thus, we've
 * now demoted the range checks to assertions within the macro itself.
 */
static inline bool
BufferIsValid(Buffer bufnum)
{
	Assert(bufnum <= NBuffers);
	Assert(bufnum >= -NLocBuffer);

	return bufnum != InvalidBuffer;
}

/*
 * BufferGetBlock
 *		Returns a reference to a disk page image associated with a buffer.
 *
 * Note:
 *		Assumes buffer is valid.
 */
static inline Block
BufferGetBlock(Buffer buffer)
{
	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferBlockPointers[-buffer - 1];
	else
		return (Block) (BufferBlocks + ((Size) (buffer - 1)) * BLCKSZ);
}

/*
 * BufferGetPageSize
 *		Returns the page size within a buffer.
 *
 * Notes:
 *		Assumes buffer is valid.
 *
 *		The buffer can be a raw disk block and need not contain a valid
 *		(formatted) disk page.
 */
/* XXX should dig out of buffer descriptor */
static inline Size
BufferGetPageSize(Buffer buffer)
{
	AssertMacro(BufferIsValid(buffer));
	return (Size) BLCKSZ;
}

/*
 * BufferGetPage
 *		Returns the page associated with a buffer.
 */
static inline Page
BufferGetPage(Buffer buffer)
{
	return (Page) BufferGetBlock(buffer);
}

/* POLAR */
#define DEFAULT_WRITE_COMBINE_LIMIT 8
#define MAX_BUFFERS_TO_READ_BY 64
#define MAX_BUFFERS_TO_WRITE_BY 32
#define MAX_BUFFERS_TO_EXTEND_BY 1024

extern PGDLLIMPORT int polar_bulk_read_size;
extern PGDLLIMPORT int polar_heap_bulk_extend_size;
extern PGDLLIMPORT int polar_index_bulk_extend_size;
extern PGDLLIMPORT int polar_recovery_bulk_extend_size;
extern PGDLLIMPORT int polar_write_combine_limit;
extern PGDLLIMPORT bool polar_force_write_combine;
extern PGDLLIMPORT bool polar_flush_buffer_nowait;
extern PGDLLIMPORT bool polar_has_partial_write;

extern int	polar_get_recovery_bulk_extend_size(BlockNumber target_block,
												BlockNumber nblocks);
extern Buffer polar_index_add_blocks(Relation relation);

extern void polar_lock_buffer_for_cleanup_ext(Buffer buffer, bool fresh_check);
extern void polar_lock_buffer_ext(Buffer buffer, int mode, bool fresh_check);
extern bool polar_conditional_lock_buffer_ext(Buffer buffer, bool fresh_check);

/* POLAR end */

#endif							/* FRONTEND */

#endif							/* BUFMGR_H */
