/*-------------------------------------------------------------------------
 *
 * bulk_write.c
 *	  Efficiently and reliably populate a new relation
 *
 * The assumption is that no other backends access the relation while we are
 * loading it, so we can take some shortcuts.  Pages already present in the
 * indicated fork when the bulk write operation is started are not modified
 * unless explicitly written to.  Do not mix operations through the regular
 * buffer manager and the bulk loading interface!
 *
 * We bypass the buffer manager to avoid the locking overhead, and call
 * smgrextend() directly.  A downside is that the pages will need to be
 * re-read into shared buffers on first use after the build finishes.  That's
 * usually a good tradeoff for large relations, and for small relations, the
 * overhead isn't very significant compared to creating the relation in the
 * first place.
 *
 * The pages are WAL-logged if needed.  To save on WAL header overhead, we
 * WAL-log several pages in one record.
 *
 * One tricky point is that because we bypass the buffer manager, we need to
 * register the relation for fsyncing at the next checkpoint ourselves, and
 * make sure that the relation is correctly fsync'd by us or the checkpointer
 * even if a checkpoint happens concurrently.
 *
 * NOTE:
 * fsync is removed from PolarDB for we use buffer pool to cache those pages.
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/bulk_write.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "access/xlogrecord.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/bulk_write.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/rel.h"

#define MAX_PENDING_WRITES 512

typedef struct PendingWrite
{
	BulkWriteBuffer buf;
	BlockNumber blkno;
	bool		page_std;
} PendingWrite;

/*
 * Bulk writer state for one relation fork.
 */
struct BulkWriteState
{
	/* Information about the target relation we're writing */
	SMgrRelation smgr;
	ForkNumber	forknum;
	bool		use_wal;
	char		relpersistence;

	/* We keep several writes queued, and WAL-log them in batches */
	int			npending;
	PendingWrite pending_writes[MAX_PENDING_WRITES];

	/* Current size of the relation */
	BlockNumber relsize;

	MemoryContext memcxt;
};

/* GUCs */
int			polar_bulk_write_maxpages;

static void smgr_bulk_flush(BulkWriteState *bulkstate);

/*
 * Start a bulk write operation on a relation fork.
 */
BulkWriteState *
smgr_bulk_start_rel(Relation rel, ForkNumber forknum)
{
	return smgr_bulk_start_smgr(RelationGetSmgr(rel),
								forknum,
								RelationNeedsWAL(rel) || forknum == INIT_FORKNUM,
								rel->rd_rel->relpersistence);
}

/*
 * Start a bulk write operation on a relation fork.
 *
 * This is like smgr_bulk_start_rel, but can be used without a relcache entry.
 */
BulkWriteState *
smgr_bulk_start_smgr(SMgrRelation smgr, ForkNumber forknum, bool use_wal, char relpersistence)
{
	BulkWriteState *state;

	state = palloc(sizeof(BulkWriteState));
	state->smgr = smgr;
	state->forknum = forknum;
	state->use_wal = use_wal;
	state->relpersistence = relpersistence;

	state->npending = 0;
	state->relsize = smgrnblocks(smgr, forknum);

	/*
	 * Remember the memory context.  We will use it to allocate all the
	 * buffers later.
	 */
	state->memcxt = CurrentMemoryContext;

	return state;
}

/*
 * Finish bulk write operation.
 */
void
smgr_bulk_finish(BulkWriteState *bulkstate)
{
	/* WAL-log and flush any remaining pages */
	smgr_bulk_flush(bulkstate);
}

static int
buffer_cmp(const void *a, const void *b)
{
	const PendingWrite *bufa = (const PendingWrite *) a;
	const PendingWrite *bufb = (const PendingWrite *) b;

	/* We should not see duplicated writes for the same block */
	Assert(bufa->blkno != bufb->blkno);
	if (bufa->blkno > bufb->blkno)
		return 1;
	else
		return -1;
}

/*
 * Finish all the pending writes.
 */
static void
smgr_bulk_flush(BulkWriteState *bulkstate)
{
	int			npending = bulkstate->npending;
	PendingWrite *pending_writes = bulkstate->pending_writes;
	BlockNumber nblocks;

	if (npending == 0)
		return;

	if (npending > 1)
		qsort(pending_writes, npending, sizeof(PendingWrite), buffer_cmp);

	nblocks = bulkstate->pending_writes[npending - 1].blkno + 1;

	/*
	 * Before we alloc buffers from buffer pool for those pages, extend the
	 * underlying file first.
	 */
	if (nblocks > bulkstate->relsize)
	{
		smgrzeroextend(bulkstate->smgr, bulkstate->forknum, bulkstate->relsize,
					   nblocks - bulkstate->relsize, true);
		bulkstate->relsize = nblocks;
	}

	for (int i = 0; i < npending;)
	{
		int			nbatch = 0;
		BlockNumber blknos[XLR_MAX_BLOCK_ID];
		Page		pages[XLR_MAX_BLOCK_ID];
		Buffer		buffers[XLR_MAX_BLOCK_ID];
		bool		page_std = true;

		/*
		 * Accumulate XLR_MAX_BLOCK_ID pages at most per round. For
		 * log_newpages takes those count of pages into one record. Also to
		 * reduce the usage of LWLock to avoid "too many LWLocks taken" ERROR.
		 */
		do
		{
			BlockNumber blkno = pending_writes[i].blkno;
			Page		cached_page = pending_writes[i].buf->data;
			Page		page;
			Buffer		buffer;

			buffer = polar_read_buffer_common(bulkstate->smgr, bulkstate->relpersistence,
											  bulkstate->forknum, blkno, RBM_ZERO_AND_LOCK, NULL);
			page = BufferGetPage(buffer);

			memcpy(page, cached_page, BLCKSZ);
			pfree(cached_page);

			MarkBufferDirty(buffer);

			/*
			 * If any of the pages use !page_std, we log them all as such.
			 * That's a bit wasteful, but in practice, a mix of standard and
			 * non-standard page layout is rare.  None of the built-in AMs do
			 * that.
			 */
			if (!pending_writes[i].page_std)
				page_std = false;

			blknos[nbatch] = blkno;
			pages[nbatch] = page;
			buffers[nbatch] = buffer;

			i++;
			nbatch++;
		} while (i < npending && nbatch < XLR_MAX_BLOCK_ID);

		/*
		 * log_newpages takes pages from buffer pool, it will do PageSetLSN
		 * for those pages. After the logging stuff, we can mark dirty and
		 * release those buffers.
		 */
		if (bulkstate->use_wal)
			log_newpages(&bulkstate->smgr->smgr_rnode.node, bulkstate->forknum,
						 nbatch, blknos, pages, page_std);

		for (int j = 0; j < nbatch; j++)
			UnlockReleaseBuffer(buffers[j]);
	}

	bulkstate->npending = 0;
}

/*
 * Queue write of 'buf'.
 *
 * NB: this takes ownership of 'buf'!
 *
 * You are only allowed to write a given block once as part of one bulk write
 * operation.
 */
void
smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum, BulkWriteBuffer buf, bool page_std)
{
	PendingWrite *w;

	w = &bulkstate->pending_writes[bulkstate->npending++];
	w->buf = buf;
	w->blkno = blocknum;
	w->page_std = page_std;

	if (bulkstate->npending >= polar_bulk_write_maxpages)
		smgr_bulk_flush(bulkstate);
}

/*
 * Allocate a new buffer which can later be written with smgr_bulk_write().
 *
 * There is no function to free the buffer.  When you pass it to
 * smgr_bulk_write(), it takes ownership and frees it when it's no longer
 * needed.
 *
 * This is currently implemented as a simple palloc, but could be implemented
 * using a ring buffer or larger chunks in the future, so don't rely on it.
 */
BulkWriteBuffer
smgr_bulk_get_buf(BulkWriteState *bulkstate)
{
	return MemoryContextAllocAligned(bulkstate->memcxt, BLCKSZ, PG_IO_ALIGN_SIZE, 0);
}
