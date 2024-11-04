/*-------------------------------------------------------------------------
 *
 * polar_logindex_redo.c
 *	  Implementation of parse xlog states and replay.
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/access/logindex/polar_logindex_redo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/commit_ts.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/polar_logindex_redo.h"
#include "access/rmgr.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "catalog/pg_control.h"
#include "executor/instrument.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "miscadmin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "storage/buf_internals.h"
#include "storage/ipc.h"
#include "storage/polar_fd.h"
#include "storage/procarray.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"
#include "utils/polar_bitpos.h"
#include "utils/polar_local_cache.h"
#include "utils/resowner_private.h"

#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode,polar_idx_save,polar_idx_parse,polar_idx_redo) \
	{name, polar_idx_save, polar_idx_parse, polar_idx_redo},

bool		polar_enable_replica_prewarm = false;
bool		polar_enable_parallel_replay_standby_mode = false;
int			polar_parallel_replay_task_queue_depth = 0;
int			polar_parallel_replay_proc_num = 0;
int			polar_logindex_max_local_cache_segments = 0;
bool		polar_enable_fullpage_snapshot = true;
int			polar_write_logindex_active_table_delay = 200;
int			polar_wait_old_version_page_timeout = 30 * 1000;
int			polar_bg_replay_batch_size;
int			polar_startup_replay_delay_size = 0;
int			polar_logindex_replay_delay_threshold = 0;
bool		polar_enable_resolve_conflict = false;
int			polar_logindex_mem_size = 0;
int			polar_logindex_bloom_blocks = 0;
int			polar_rel_size_cache_blocks = 0;
int			polar_xlog_queue_buffers = 0;
bool		polar_force_change_checkpoint = false;
bool		polar_enable_standby_instant_recovery = false;

polar_logindex_redo_ctl_t polar_logindex_redo_instance = NULL;

/* POLAR: Flag set when replaying and marking buffer dirty */
polar_logindex_bg_proc_t polar_bg_replaying_process = POLAR_NOT_LOGINDEX_BG_PROC;

/* Define the ratio of memory configured by polar_logindex_mem_size used for wal logindex and fullpage logindex */
#define WAL_LOGINDEX_MEM_RATIO          (0.75)
#define FULLPAGE_LOGINDEX_MEM_RATIO     (0.25)

#define WAL_LOGINDEX_DIR                "pg_logindex"
#define FULLPAGE_LOGINDEX_DIR           "polar_fullpage"
#define RELATION_SIZE_CACHE_DIR         "polar_rel_size_cache"

typedef enum
{
	BUF_NEED_REPLAY,
	BUF_IS_REPLAYING,
	BUF_IS_REPLAYED,
	BUF_IS_FLUSHED,
	BUF_IS_TRUNCATED,
}			buf_replay_stat_t;

/*
 * The backend process can have only one page iterator for wal logindex snapshot or fullpage logindex snapshot.
 * When backend process receive cancel query signal the memory allocated for
 * page iterator must be released.
 * This static variable is used to track allocated memory for page iterator and release
 * memory when receive signal.
 */
static log_index_page_iter_t wal_page_iter = NULL;
static log_index_page_iter_t fullpage_page_iter = NULL;

static BufferDesc *polar_replaying_buffer = NULL;
static log_index_page_iter_t iter_context = NULL;
static MemoryContext polar_redo_context = NULL;

static const log_index_redo_t polar_idx_redo[RM_MAX_ID + 1] =
{
#include "access/rmgrlist.h"
};

static void polar_only_replay_exists_buffer(polar_logindex_redo_ctl_t instance, XLogReaderState *state, log_index_lsn_t * log_index_page);
static void polar_logindex_apply_one_page(polar_logindex_redo_ctl_t instance, BufferTag *tag, Buffer *buffer, log_index_page_iter_t iter);
static XLogRecPtr polar_logindex_apply_page_from(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn, BufferTag *tag, Buffer *buffer, polar_page_lock_t page_lock);
static bool polar_evict_buffer(Buffer buffer);
static void polar_extend_block_if_not_exist(BufferTag *tag);
static MemoryContext polar_redo_memory_context(void);

static void
polar_xlog_log(int level, XLogReaderState *record, const char *func)
{
	RmgrId		rmid = XLogRecGetRmid(record);
	uint8		info = XLogRecGetInfo(record);
	XLogRecPtr	lsn = record->ReadRecPtr;
	const char *id;

	id = RmgrTable[rmid].rm_identify(info);

	if (id == NULL)
		ereport(level, (errmsg("%s lsn=%X/%X UNKNOWN (%X)", func,
							   LSN_FORMAT_ARGS(lsn),
							   info & ~XLR_INFO_MASK),
						errhidestmt(true),
						errhidecontext(true)));
	else
		ereport(level, (errmsg("%s lsn=%X/%X %s/%s",
							   func, LSN_FORMAT_ARGS(lsn),
							   RmgrTable[rmid].rm_name, id),
						errhidestmt(true),
						errhidecontext(true)));
}

/*
 * Call this function when we abort transaction.
 * If transaction is aborted, the buffer replaying is stopped,
 * so we have to clear its POLAR_REDO_REPLAYING flag.
 */
static void
polar_logindex_abort_replaying_buffer(void)
{
	if (polar_replaying_buffer != NULL)
	{
		uint32		redo_state = polar_lock_redo_state(polar_replaying_buffer);

		redo_state &= (~POLAR_REDO_REPLAYING);
		polar_unlock_redo_state(polar_replaying_buffer, redo_state);

		elog(LOG, "Abort replaying buf_id=%d, " POLAR_LOG_BUFFER_TAG_FORMAT, polar_replaying_buffer->buf_id,
			 POLAR_LOG_BUFFER_TAG(&polar_replaying_buffer->tag));
		polar_replaying_buffer = NULL;
	}
}

/*
 * For the block file in tag, extend it to at least tag->blockNum blocks (might
 * be longer to reduce smgrzeroextend invocations).
 */
static void
polar_extend_block_if_not_exist(BufferTag *tag)
{
	SMgrRelation smgr;
	BlockNumber nblocks;

	smgr = smgropen(tag->rnode, InvalidBackendId);
	smgrcreate(smgr, tag->forkNum, true);
	nblocks = smgrnblocks(smgr, tag->forkNum);

	if (tag->blockNum >= nblocks)
	{
		int			block_count = polar_get_recovery_bulk_extend_size(tag->blockNum, nblocks);

		Assert(nblocks + block_count > tag->blockNum);

		smgrzeroextend(smgr, tag->forkNum, nblocks, block_count, false);
	}
}

void
polar_logindex_apply_one_record(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer)
{
	MemoryContext old_ctx,
				redo_ctx;

	redo_ctx = polar_redo_memory_context();
	old_ctx = MemoryContextSwitchTo(redo_ctx);

	if (polar_idx_redo[XLogRecGetRmid(state)].rm_polar_idx_redo == NULL)
	{
		POLAR_LOG_CONSISTENT_LSN();
		POLAR_LOG_XLOG_RECORD_INFO(state);
		POLAR_LOG_BUFFER_TAG_INFO(tag);
		elog(PANIC, "rmid = %d has not polar_idx_redo function", XLogRecGetRmid(state));
	}

	ereport(polar_trace_logindex(DEBUG3), (errmsg("%s %d %X/%X, " POLAR_LOG_BUFFER_TAG_FORMAT, PG_FUNCNAME_MACRO,
												  *buffer, LSN_FORMAT_ARGS(state->ReadRecPtr),
												  POLAR_LOG_BUFFER_TAG(tag)),
										   errhidestmt(true),
										   errhidecontext(true)));

	polar_idx_redo[XLogRecGetRmid(state)].rm_polar_idx_redo(instance, state, tag, buffer);

	MemoryContextSwitchTo(old_ctx);
	POLAR_ASSERT_PANIC(!redo_ctx->firstchild);
	MemoryContextReset(redo_ctx);
}

XLogRecord *
polar_logindex_read_xlog(XLogReaderState *state, XLogRecPtr lsn)
{
	XLogRecord *record = NULL;
	char	   *errormsg = NULL;
	int			i = 1;

	while (record == NULL)
	{
		XLogBeginRead(state, lsn);
		record = XLogReadRecord(state, &errormsg);

		if (record == NULL)
		{
			if (i % 100000 == 0)
				elog(WARNING, "Failed to read record which lsn=%X/%X",
					 LSN_FORMAT_ARGS(lsn));

			i++;
			pg_usleep(1000);
		}
	}

	return record;
}

static Buffer
polar_read_vm_buffer(BufferTag *heap_tag)
{
	Buffer		buf = InvalidBuffer;
	Relation	rel = CreateFakeRelcacheEntry(heap_tag->rnode);

	visibilitymap_pin(rel, heap_tag->blockNum, &buf);

	FreeFakeRelcacheEntry(rel);

	return buf;
}

Buffer
polar_logindex_outdate_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *heap_tag,
							 bool get_cleanup_lock, polar_page_lock_t * page_lock, bool vm_parse)
{
	uint32		page_hash;
	LWLock	   *partition_lock; /* buffer partition lock for it */
	int			buf_id;
	Buffer		buffer = InvalidBuffer;
	BufferTag	vm_tag;
	BufferTag  *tag;

	if (!vm_parse)
		tag = heap_tag;
	else
	{
		vm_tag.rnode = heap_tag->rnode;
		vm_tag.forkNum = VISIBILITYMAP_FORKNUM;
		vm_tag.blockNum = HEAPBLK_TO_MAPBLOCK(heap_tag->blockNum);
		tag = &vm_tag;
	}

	page_hash = BufTableHashCode(tag);
	partition_lock = BufMappingPartitionLock(page_hash);

	/* Check whether the block is already in the buffer pool */
	LWLockAcquire(partition_lock, LW_SHARED);
	buf_id = BufTableLookup(tag, page_hash);

	/* Found the buffer */
	if (buf_id >= 0)
	{
		uint32		redo_state;
		BufferDesc *buf_desc = GetBufferDescriptor(buf_id);

		polar_pin_buffer(buf_desc, NULL);
		LWLockRelease(partition_lock);

		redo_state = polar_lock_redo_state(buf_desc);

		/*
		 * 1. A backend process is reading this buffer from storage and it
		 * will redo for this buffer. 2. The mini transaction lock is acquired
		 * by the startup process. 3. We add lsn to logindex in startup
		 * process with the acquired mini transaction lock. 4. When backend
		 * process compete buffer read and start to redo for buffer, it will
		 * redo to this lsn
		 */
		if (!(redo_state & POLAR_REDO_READ_IO_END) ||
			((redo_state & POLAR_REDO_REPLAYING) && !get_cleanup_lock))
		{
			redo_state |= POLAR_REDO_OUTDATE;
			polar_unlock_redo_state(buf_desc, redo_state);

			ReleaseBuffer(BufferDescriptorGetBuffer(buf_desc));
			POLAR_LOGINDEX_MINI_TRANS_ADD_LSN(instance->wal_logindex_snapshot,
											  instance->mini_trans, *page_lock, state, tag);
		}
		else
		{
			polar_unlock_redo_state(buf_desc, redo_state);
			polar_logindex_mini_trans_unlock(instance->mini_trans, *page_lock);

			if (!vm_parse)
				buffer = XLogReadBufferExtended(tag->rnode, tag->forkNum, tag->blockNum, RBM_NORMAL_NO_LOG, InvalidBuffer);
			else
				buffer = polar_read_vm_buffer(heap_tag);

			ReleaseBuffer(BufferDescriptorGetBuffer(buf_desc));

			if (BufferIsValid(buffer))
			{
				if (get_cleanup_lock)
					polar_lock_buffer_for_cleanup_ext(buffer, false);
				else
					polar_lock_buffer_ext(buffer, BUFFER_LOCK_EXCLUSIVE, false);
			}
			else
			{
				POLAR_LOG_CONSISTENT_LSN();
				POLAR_LOG_XLOG_RECORD_INFO(state);
				POLAR_LOG_BUFFER_TAG_INFO(tag);
				elog(FATAL, "Failed to read page");
			}

			*page_lock = polar_logindex_mini_trans_lock(instance->mini_trans, tag, LW_EXCLUSIVE, NULL);
			POLAR_LOGINDEX_MINI_TRANS_ADD_LSN(instance->wal_logindex_snapshot,
											  instance->mini_trans, *page_lock, state, tag);

			/*
			 * Force to get buffer descriptor again to avoid different buffer
			 * descriptor when buffer is evicted before
			 */
			buf_desc = GetBufferDescriptor(buffer - 1);
			redo_state = polar_lock_redo_state(buf_desc);
			redo_state |= POLAR_REDO_OUTDATE;
			polar_unlock_redo_state(buf_desc, redo_state);
		}

		if (unlikely(polar_trace_logindex_messages <= DEBUG4))
		{
			XLogRecPtr	page_lsn = InvalidXLogRecPtr;

			if (!BufferIsInvalid(buffer))
			{
				Page		page = BufferGetPage(buffer);

				page_lsn = PageGetLSN(page);
			}

			ereport(LOG, (errmsg("%s buf_id=%d page_lsn=%X/%X, xlog=%X/%X, " POLAR_LOG_BUFFER_TAG_FORMAT, PG_FUNCNAME_MACRO,
								 buf_id,
								 LSN_FORMAT_ARGS(page_lsn),
								 LSN_FORMAT_ARGS(state->ReadRecPtr),
								 POLAR_LOG_BUFFER_TAG(tag)),
						  errhidestmt(true),
						  errhidecontext(true)));
			polar_xlog_log(LOG, state, PG_FUNCNAME_MACRO);
		}
	}
	else
	{
		LWLockRelease(partition_lock);

		/*
		 * POLAR: extend blocks if not exist in standby mode;
		 *
		 * The semantic of parsing WAL is that the data of the WAL can be
		 * queried after parsing. However, some queries depend on the number
		 * of blocks in the storage (e.g. select * from <table>). The data in
		 * the buffer won't be queried if its block is not extended in the
		 * storage.
		 */
		if (POLAR_IN_PARALLEL_REPLAY_STANDBY_MODE(polar_logindex_redo_instance) || polar_should_launch_standby_instant_recovery())
			polar_extend_block_if_not_exist(tag);

		POLAR_LOGINDEX_MINI_TRANS_ADD_LSN(instance->wal_logindex_snapshot,
										  instance->mini_trans, *page_lock, state, tag);
	}

	return buffer;
}

Buffer
polar_logindex_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag,
					 bool get_cleanup_lock, polar_page_lock_t * page_lock)
{
	Buffer		buffer;

	buffer = polar_logindex_outdate_parse(instance, state, tag, get_cleanup_lock, page_lock, false);

	return buffer;
}

static void
polar_logindex_parse_tag(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag,
						 bool get_cleanup_lock)
{
	polar_page_lock_t page_lock;
	Buffer		buffer;

	page_lock = polar_logindex_mini_trans_lock(instance->mini_trans, tag, LW_EXCLUSIVE, NULL);
	buffer = polar_logindex_parse(instance, state, tag, get_cleanup_lock, &page_lock);

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	polar_logindex_mini_trans_unlock(instance->mini_trans, page_lock);
}

void
polar_logindex_save_vm_block(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 heap_block)
{
	BufferTag	vm_tag;
	DecodedBkpBlock *blk;

	POLAR_ASSERT_PANIC(heap_block <= XLR_MAX_BLOCK_ID);

	blk = XLogRecGetBlock(state, heap_block);

	POLAR_ASSERT_PANIC(blk->in_use);

	INIT_BUFFERTAG(vm_tag, blk->rnode, VISIBILITYMAP_FORKNUM, HEAPBLK_TO_MAPBLOCK(blk->blkno));

	POLAR_LOG_INDEX_ADD_LSN(instance->wal_logindex_snapshot, state, &vm_tag);
}

static void
polar_logindex_save_block_common(logindex_snapshot_t snapshot, XLogReaderState *state, uint8 block_id)
{
	BufferTag	tag;
	DecodedBkpBlock *blk;

	POLAR_ASSERT_PANIC(block_id <= XLR_MAX_BLOCK_ID);

	blk = XLogRecGetBlock(state, block_id);

	POLAR_ASSERT_PANIC(blk->in_use);

	INIT_BUFFERTAG(tag, blk->rnode, blk->forknum, blk->blkno);

	POLAR_LOG_INDEX_ADD_LSN(snapshot, state, &tag);
}

void
polar_logindex_save_block(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id)
{
	polar_logindex_save_block_common(instance->wal_logindex_snapshot, state, block_id);
}

void
polar_fullpage_logindex_save_block(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id)
{
	polar_logindex_save_block_common(instance->fullpage_logindex_snapshot, state, block_id);
}

void
polar_logindex_redo_vm_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record, uint8 heap_block)
{
	BufferTag	heap_tag,
				vm_tag;
	polar_page_lock_t vm_lock;
	Relation	rel;
	Buffer		buf;

	POLAR_GET_LOG_TAG(record, heap_tag, heap_block);
	INIT_BUFFERTAG(vm_tag, heap_tag.rnode, VISIBILITYMAP_FORKNUM, HEAPBLK_TO_MAPBLOCK(heap_tag.blockNum));

	rel = CreateFakeRelcacheEntry(heap_tag.rnode);
	vm_lock = polar_logindex_mini_trans_lock(instance->mini_trans, &vm_tag, LW_EXCLUSIVE, NULL);

	buf = polar_logindex_outdate_parse(instance, record, &heap_tag, false, &vm_lock, true);

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	polar_logindex_mini_trans_unlock(instance->mini_trans, vm_lock);

	FreeFakeRelcacheEntry(rel);
}

void
polar_logindex_redo_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id)
{
	BufferTag	tag;

	POLAR_GET_LOG_TAG(state, tag, block_id);
	polar_logindex_parse_tag(instance, state, &tag, false);
}

void
polar_bitmap_logindex_redo_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag tag)
{
	polar_logindex_parse_tag(instance, state, &tag, false);
}

void
polar_logindex_cleanup_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id)
{
	BufferTag	tag;

	POLAR_GET_LOG_TAG(state, tag, block_id);
	polar_logindex_parse_tag(instance, state, &tag, true);
}

void
polar_logindex_save_lsn(polar_logindex_redo_ctl_t instance, XLogReaderState *state)
{
	RmgrId		rmid = XLogRecGetRmid(state);

	if (polar_idx_redo[rmid].rm_polar_idx_save != NULL)
		polar_idx_redo[rmid].rm_polar_idx_save(instance, state);
	else
	{
		POLAR_LOG_CONSISTENT_LSN();
		POLAR_LOG_XLOG_RECORD_INFO(state);
		elog(PANIC, "rm_polar_idx_save is not set for rmid=%d", rmid);
	}
}

bool
polar_enable_logindex_parse(void)
{
	/*
	 * If it's replica mode and logindex is enabled, we parse XLOG and save it
	 * to logindex. If it's standby parallel replay mode and logindex is
	 * enabled, we also parse XLOG and save it to logindex.
	 *
	 * During recovery we read XLOG from checkpoint, primary node can drop or
	 * truncate table after this checkpoint. We can't read these removed data
	 * blocks, so it will be PANIC if we read data block and replay XLOG. In
	 * primary node it will create table if it does not exist, but we can not
	 * do this in replica mode.
	 */
	return (polar_is_replica() && polar_logindex_redo_instance) ||
		POLAR_IN_PARALLEL_REPLAY_STANDBY_MODE(polar_logindex_redo_instance);
}

XLogRecPtr
polar_logindex_redo_parse_start_lsn(polar_logindex_redo_ctl_t instance)
{
	if (!instance)
		return InvalidXLogRecPtr;

	return polar_logindex_check_valid_start_lsn(instance->wal_logindex_snapshot);
}

bool
polar_logindex_parse_xlog(polar_logindex_redo_ctl_t instance, RmgrId rmid, XLogReaderState *state,
						  XLogRecPtr redo_start_lsn, XLogRecPtr *mini_trans_lsn)
{
	bool		redo = false;
	static bool parse_valid = false;

	if (unlikely(!parse_valid))
	{
		XLogRecPtr	start_lsn = polar_logindex_redo_parse_start_lsn(instance);

		if (!XLogRecPtrIsInvalid(start_lsn))
			parse_valid = true;

		if (!parse_valid)
			return redo;

		elog(LOG, "wal logindex parse from %lX", state->ReadRecPtr);
	}

	POLAR_ASSERT_PANIC(mini_trans_lsn != NULL);
	*mini_trans_lsn = InvalidXLogRecPtr;

	/*
	 * POLAR: In standby mode, parallel replay can be triggered only when
	 * consistency reached, temporarily!
	 */
	if (polar_is_replica() ||
		(polar_is_standby() && polar_should_standby_launch_async_parse()))
	{
		/* Make sure there's room for us to pin buffer */
		ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

		if (polar_idx_redo[rmid].rm_polar_idx_parse != NULL)
		{
			if (unlikely(polar_trace_logindex_messages <= DEBUG4))
				polar_xlog_log(LOG, state, PG_FUNCNAME_MACRO);

			polar_logindex_mini_trans_start(instance->mini_trans, state->EndRecPtr);
			redo = polar_idx_redo[rmid].rm_polar_idx_parse(instance, state);

			/*
			 * We can not end mini transaction here because
			 * XLogRecoveryCtl->lastReplayedEndRecPtr is not updated. If we
			 * end mini transaction here, and one backend start to do buffer
			 * replay, it can not acquire mini transaction lock and  replay to
			 * XLogRecoveryCtl->lastReplayedEndRecPtr, so the current record
			 * which was parsed and saved to logindex will be lost. We will
			 * end mini transaction after startup process update
			 * XLogRecoveryCtl->lastReplayedEndRecPtr.
			 */
			*mini_trans_lsn = state->EndRecPtr;
		}
	}
	/* POLAR: create and save logindex in primary and standby. */
	else
	{
		if (polar_idx_redo[rmid].rm_polar_idx_save != NULL)
			polar_idx_redo[rmid].rm_polar_idx_save(instance, state);
	}

	/*
	 * POLAR: If current record lsn is smaller than redo start lsn, then we
	 * only parse xlog and create logindex.
	 */
	if (state->ReadRecPtr < redo_start_lsn)
		redo = true;

	return redo;
}

/*
 * POLAR: Apply the WAL record from start_lsn to end_lsn - 1.
 */
XLogRecPtr
polar_logindex_apply_page(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn, XLogRecPtr end_lsn,
						  BufferTag *tag, Buffer *buffer)
{
	Page		page;

	POLAR_ASSERT_PANIC(BufferIsValid(*buffer));
	page = BufferGetPage(*buffer);

	if (!PageIsNew(page))
		start_lsn = Max(start_lsn, PageGetLSN(page));

	if (unlikely(end_lsn <= start_lsn))
	{
		/*
		 * 1. When do online promote, the page could be flushed after
		 * replayed, so end_lsn may be smaller than start_lsn 2. The new
		 * created Replica node, which received primary consistent lsn but
		 * primary didn't have new replica node's replayed lsn, and then
		 * end_lsn may be smaller than start_lsn 3. The end_lsn is the start
		 * point of last replayed record, while start_lsn is set by consistent
		 * lsn , which is the end point of last replayed record, then end_lsn
		 * is smaller than start_lsn.
		 */
		if (end_lsn < start_lsn && polar_is_replica())
		{
			/*
			 * POLAR: Sometimes, Replica output lots of errorlog to fullfill
			 * the errorlog pipe. So, startup process spent much time inside
			 * elog function and wal apply latency would be increased.
			 */
			ereport(polar_trace_logindex(DEBUG1), (errmsg("Try to replay page " POLAR_LOG_BUFFER_TAG_FORMAT " which end_lsn=%lX is smaller than start_lsn=%lX, page_lsn=%lX",
														  POLAR_LOG_BUFFER_TAG(tag), end_lsn, start_lsn, PageGetLSN(page)), errhidestmt(true), errhidecontext(true)));
		}

		return start_lsn;
	}

	ereport(polar_trace_logindex(DEBUG3), (errmsg("%s %d %X/%X %X/%X, " POLAR_LOG_BUFFER_TAG_FORMAT, PG_FUNCNAME_MACRO,
												  *buffer,
												  LSN_FORMAT_ARGS(start_lsn),
												  LSN_FORMAT_ARGS(end_lsn),
												  POLAR_LOG_BUFFER_TAG(tag)),
										   errhidestmt(true),
										   errhidecontext(true)));

	POLAR_ASSERT_PANIC(wal_page_iter == NULL);

	/*
	 * Logindex record the start position of XLOG and we search LSN between
	 * [start_lsn, end_lsn]. And end_lsn points to the end position of the
	 * last xlog, so we should subtract 1 here .
	 */
	wal_page_iter = polar_logindex_create_page_iterator(instance->wal_logindex_snapshot,
														tag, start_lsn, end_lsn - 1, polar_get_bg_redo_state(instance) == POLAR_BG_ONLINE_PROMOTE);

	if (unlikely(polar_logindex_page_iterator_state(wal_page_iter) != ITERATE_STATE_FINISHED))
	{
		elog(PANIC, "Failed to iterate data for " POLAR_LOG_BUFFER_TAG_FORMAT ", which start_lsn=%X/%X and end_lsn=%X/%X",
			 POLAR_LOG_BUFFER_TAG(tag),
			 LSN_FORMAT_ARGS(start_lsn),
			 LSN_FORMAT_ARGS(end_lsn));
	}

	polar_logindex_apply_one_page(instance, tag, buffer, wal_page_iter);

	polar_logindex_release_page_iterator(wal_page_iter);
	wal_page_iter = NULL;

	return end_lsn;
}

static XLogRecPtr
polar_logindex_apply_page_from(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn, BufferTag *tag, Buffer *buffer, polar_page_lock_t page_lock)
{
	XLogRecPtr	end_lsn;
	Page		page;

	POLAR_ASSERT_PANIC(BufferIsValid(*buffer));
	page = BufferGetPage(*buffer);

	/*
	 * If block was truncated before, then we change the start lsn to the lsn
	 * when truncate block
	 */
	if (instance->rel_size_cache)
	{
		bool		valid;
		XLogRecPtr	lsn_changed;

		LWLockAcquire(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache), LW_SHARED);
		valid = polar_check_rel_block_valid_and_lsn(instance->rel_size_cache, start_lsn, tag, &lsn_changed);
		LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache));

		if (!valid)
		{
			ereport(polar_trace_logindex(DEBUG1), (errmsg("%s change start_lsn from %lX to %lX for page_lsn=%lX " POLAR_LOG_BUFFER_TAG_FORMAT,
														  PG_FUNCNAME_MACRO, start_lsn, lsn_changed, PageGetLSN(page), POLAR_LOG_BUFFER_TAG(tag)), errhidestmt(true), errhidecontext(true)));
			start_lsn = lsn_changed;
		}
	}

	/*
	 * If this buffer replaying is protected by mini transaction page_lock and
	 * replaying lsn is added to logindex then we replay to the record which
	 * is currently replaying. Otherwise we replay to the last record which is
	 * successfully replayed. If we replay to the currently replaying record
	 * without mini transaction page_lock, we may get inconsistent date
	 * structure in memory.
	 */
	if (page_lock != POLAR_INVALID_PAGE_LOCK &&
		polar_logindex_mini_trans_get_page_added(instance->mini_trans, page_lock))
		end_lsn = GetCurrentReplayRecPtr(NULL);
	else
		end_lsn = polar_get_xlog_replay_recptr_nolock();

	return polar_logindex_apply_page(instance, start_lsn, end_lsn, tag, buffer);
}

/*
 * Search xlog base on the buffer tag and replay these xlog record for the buffer.
 * Return true if page lsn changed after replay
 */
bool
polar_logindex_lock_apply_page_from(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn, BufferTag *tag, Buffer *buffer)
{
	BufferDesc *buf_hdr;
	polar_page_lock_t page_lock;
	uint32		redo_state;
	char	   *page;
	XLogRecPtr	origin_lsn;
	MemoryContext oldcontext;

	POLAR_ASSERT_PANIC(BufferIsValid(*buffer));

	/*
	 * Record the buffer that is replaying.If we abort transaction in this
	 * backend process, we need to clear POLAR_REDO_REPLAYING from buffer redo
	 * state
	 */
	polar_replaying_buffer = buf_hdr = GetBufferDescriptor(*buffer - 1);

	/*
	 * Prevent interrupts while replaying to avoid inconsistent buffer redo
	 * state
	 */
	HOLD_INTERRUPTS();

	oldcontext = MemoryContextSwitchTo(polar_logindex_memory_context());

	/*
	 * We should finish reading data from storage and then replay xlog for
	 * page, so we set redo_state to be POLAR_REDO_READ_IO_END |
	 * POLAR_REDO_REPLAYING.
	 */
	redo_state = polar_lock_redo_state(buf_hdr);
	redo_state |= (POLAR_REDO_READ_IO_END | POLAR_REDO_REPLAYING);
	redo_state &= (~POLAR_REDO_OUTDATE);
	polar_unlock_redo_state(buf_hdr, redo_state);

	page = BufferGetPage(*buffer);
	origin_lsn = PageGetLSN(page);

	do
	{
		page_lock = polar_logindex_mini_trans_cond_lock(instance->mini_trans, tag, LW_EXCLUSIVE, NULL);

		start_lsn = polar_logindex_apply_page_from(instance, start_lsn, tag, buffer, page_lock);

		redo_state = polar_lock_redo_state(buf_hdr);

		if (redo_state & POLAR_REDO_OUTDATE)
			redo_state &= (~POLAR_REDO_OUTDATE);
		else
			redo_state &= (~POLAR_REDO_REPLAYING);

		polar_unlock_redo_state(buf_hdr, redo_state);

		if (page_lock != POLAR_INVALID_PAGE_LOCK)
			polar_logindex_mini_trans_unlock(instance->mini_trans, page_lock);
	}
	while (redo_state & POLAR_REDO_REPLAYING);

	MemoryContextSwitchTo(oldcontext);

	/* Now we can allow interrupts again */
	RESUME_INTERRUPTS();

	polar_replaying_buffer = NULL;

	return PageGetLSN(page) != origin_lsn;
}

static void
polar_promote_mark_buf_dirty(polar_logindex_redo_ctl_t instance, Buffer buffer, XLogRecPtr start_lsn)
{
	Page		page;
	XLogRecPtr	page_lsn;

	if (likely(!polar_bg_redo_state_is_parallel(instance)))
		return;

	page = BufferGetPage(buffer);
	page_lsn = PageGetLSN(page);

	/*
	 * If page lsn is larger than the last replayed xlog lsn, then this buffer
	 * was modified after online promote and we don't need to mark it dirty
	 * again
	 *
	 * There is a situation when a wal is parsed, dispatched, replayed and the
	 * page lsn is updated but XLogRecoveryCtl->lastReplayedEndRecPtr has not
	 * been updated, so that the new page won't be marked as dirty, which is
	 * wrong.
	 *
	 * XLogRecoveryCtl->lastReplayedEndRecPtr won't be changed during online
	 * promote.
	 */
	if (polar_get_bg_redo_state(instance) != POLAR_BG_PARALLEL_REPLAYING &&
		page_lsn > polar_get_xlog_replay_recptr_nolock())
		return;

	/*
	 * During online promote the start_lsn is the background process replayed
	 * lsn which used as the lower limit to create logindex iterator. If page
	 * lsn is smaller or equal to start_lsn, then we have no xlog record to
	 * replay when visist this buffer, so we don't need to mark it dirty
	 */
	if (page_lsn <= start_lsn)
		return;

	MarkBufferDirty(buffer);
}

/*
 * Apply the specific buffer from the start lsn to the last parsed lsn.
 * We must acquire mini transaction page lock
 * to avoid add lsn for this page.This is used when enable page invalid and
 * apply buffer when a backend process use this buffer.
 */
void
polar_logindex_lock_apply_buffer(polar_logindex_redo_ctl_t instance, Buffer *buffer)
{
	BufferDesc *buf_desc;
	XLogRecPtr	bg_replayed_lsn;

	POLAR_ASSERT_PANIC(BufferIsValid(*buffer));
	buf_desc = GetBufferDescriptor(*buffer - 1);

	if (polar_redo_check_state(buf_desc, POLAR_REDO_OUTDATE))
	{
		SpinLockAcquire(&instance->info_lck);
		bg_replayed_lsn = instance->bg_replayed_lsn;
		POLAR_SET_BACKEND_READ_MIN_LSN(bg_replayed_lsn);
		SpinLockRelease(&instance->info_lck);

		polar_logindex_lock_apply_page_from(instance, bg_replayed_lsn, &buf_desc->tag, buffer);
		polar_promote_mark_buf_dirty(instance, *buffer, bg_replayed_lsn);

		POLAR_RESET_BACKEND_READ_MIN_LSN();
	}
}

static XLogReaderState *
polar_allocate_xlog_reader(void)
{
	XLogReaderState *state = XLogReaderAllocate(wal_segment_size, NULL,
												XL_ROUTINE(.page_read = &read_local_xlog_page,
														   .segment_open = &wal_segment_open,
														   .segment_close = &wal_segment_close),
												NULL);

	if (!state)
	{
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	}

	return state;
}

static void
polar_logindex_apply_one_page(polar_logindex_redo_ctl_t instance, BufferTag *tag, Buffer *buffer, log_index_page_iter_t iter)
{
	static XLogReaderState *state = NULL;
	log_index_lsn_t *lsn_info;
	MemoryContext oldcontext;

	iter_context = iter;

	oldcontext = MemoryContextSwitchTo(polar_logindex_memory_context());

	if (unlikely(state == NULL))
		state = polar_allocate_xlog_reader();

	while ((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL)
	{
		polar_logindex_read_xlog(state, lsn_info->lsn);
		polar_logindex_apply_one_record(instance, state, tag, buffer);

		CHECK_FOR_INTERRUPTS();
	}

	MemoryContextSwitchTo(oldcontext);
}

polar_logindex_bg_redo_ctl_t *
polar_create_bg_redo_ctl(polar_logindex_redo_ctl_t instance, bool enable_processes_pool)
{
	polar_logindex_bg_redo_ctl_t *ctl = palloc0(sizeof(polar_logindex_bg_redo_ctl_t));
	XLogRecPtr	bg_replayed_lsn = polar_bg_redo_get_replayed_lsn(instance);

	ctl->instance = instance;
	ctl->lsn_iter = polar_logindex_create_lsn_iterator(instance->wal_logindex_snapshot, bg_replayed_lsn);

	ctl->state = polar_allocate_xlog_reader();
	ctl->replay_batch_size = polar_bg_replay_batch_size;

	if (enable_processes_pool)
	{
		ctl->sched_ctl = polar_create_parallel_replay_sched_ctl(ctl);
		polar_start_proc_pool(ctl->sched_ctl);
	}

	ereport(LOG, (errmsg("Start background replay iter from %X/%X",
						 LSN_FORMAT_ARGS(bg_replayed_lsn))));
	return ctl;
}

void
polar_release_bg_redo_ctl(polar_logindex_bg_redo_ctl_t *ctl)
{
	if (ctl->sched_ctl)
		polar_release_task_sched_ctl(ctl->sched_ctl);

	polar_logindex_release_lsn_iterator(ctl->lsn_iter);
	XLogReaderFree(ctl->state);

	pfree(ctl);
}

/*
 * POLAR: Apply record in background to catch up replayed lsn updated in Startup.
 *
 * In this func, it use a iterator from logindex to go through xlog record, and
 * apply record on related page if page buffer is in buffer pool. If current record
 * is not in logindex, such as clog record, it will skip replay of it and advance
 * bg_replayed_lsn directly. Initial xlog record lsn is set when reachedConsistency
 * turns true in Startup process.
 *
 * This func is called in Bgwriter process in Replica node. Each run, it apply
 * polar_bg_replay_batch_size xlog records at most.
 *
 * It will advance bg_replayed_lsn which will be replied to primary as restart_lsn of
 * replication slot. And that will stop primary from deleting wal segments before
 * bg_replayed_lsn.
 *
 * Return true if no more records need replay, otherwise false.
 */
static bool
polar_logindex_apply_xlog_background(polar_logindex_bg_redo_ctl_t *ctl)
{
	int			replayed_count = 0;
	XLogRecPtr	replayed_lsn;
	uint32		bg_redo_state = polar_get_bg_redo_state(ctl->instance);

	if (unlikely(bg_redo_state == POLAR_BG_REDO_NOT_START || bg_redo_state == POLAR_BG_WAITING_RESET))
		return true;

	replayed_lsn = polar_get_last_replayed_read_ptr();

	/* Make sure there's room for us to pin buffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	for (replayed_count = 0; replayed_count < ctl->replay_batch_size; replayed_count++)
	{
		XLogRecPtr	current_ptr;

		if (ctl->replay_page == NULL)
			ctl->replay_page =
				polar_logindex_lsn_iterator_next(ctl->instance->wal_logindex_snapshot, ctl->lsn_iter);

		/* If no more item in logindex, exit and retry next run */
		if (ctl->replay_page == NULL)
			break;

		POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(ctl->replay_page->lsn));

		/* Now, we get one record from logindex lsn iterator */

		/* If current xlog record is beyond replayed lsn, just break */
		if (ctl->replay_page->lsn > replayed_lsn)
			break;

		/* we get a new record now, set current_ptr as the lsn of it */
		current_ptr = ctl->replay_page->lsn;

		/*
		 * replay each page of current record. because we checked
		 * log_index_page->lsn < replayed_lsn, we will get all related pages
		 * of the record, so use do-while to go through each page replay
		 */
		do
		{
			polar_only_replay_exists_buffer(ctl->instance, ctl->state, ctl->replay_page);
			ctl->replay_page =
				polar_logindex_lsn_iterator_next(ctl->instance->wal_logindex_snapshot, ctl->lsn_iter);
		}
		while (ctl->replay_page != NULL && current_ptr == ctl->replay_page->lsn);

		/*
		 * now, record related pages have all been replayed, so we can advance
		 * bg_replayed_lsn
		 */
		polar_bg_redo_set_replayed_lsn(ctl->instance, current_ptr);
	}

	return (replayed_count < polar_bg_replay_batch_size);
}

/*
 * POLAR: Evict selectd buffer.
 *
 * In some case, we need to release buffer's content lock if we have before
 * put it back to free list. In other word, if the buffer is locked and pined
 * before this func, and it will be released and un-pined while being evicted.
 */
static bool
polar_evict_buffer(Buffer buffer)
{
	uint32		buf_state;
	BufferDesc *buf_desc = GetBufferDescriptor(buffer - 1);
	BufferTag	tag = buf_desc->tag;
	uint32		hash = BufTableHashCode(&tag);
	LWLock	   *partition_lock = BufMappingPartitionLock(hash);
	bool		evict = false;

	LWLockAcquire(partition_lock, LW_EXCLUSIVE);
	buf_state = LockBufHdr(buf_desc);

	/* We should already pin this buffer, and then evict it */
	if (BUF_STATE_GET_REFCOUNT(buf_state) == 1 && !(buf_state & BM_DIRTY)
		&& (buf_state & BM_TAG_VALID))
	{
		CLEAR_BUFFERTAG(buf_desc->tag);
		buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
		evict = true;
	}

	UnlockBufHdr(buf_desc, buf_state);

	if (evict)
	{
		uint32		redo_state;

		redo_state = polar_lock_redo_state(buf_desc);
		redo_state &= ~(POLAR_BUF_REDO_FLAG_MASK);
		polar_unlock_redo_state(buf_desc, redo_state);

		BufTableDelete(&tag, hash);
		ReleaseBuffer(buffer);

		/*
		 * Insert the buffer at the head of the list of free buffers.
		 */
		StrategyFreeBuffer(buf_desc);
	}

	LWLockRelease(partition_lock);

	return evict;
}

static bool
polar_replica_evict_buffer(Buffer buffer)
{
	if (!polar_is_replica())
		return false;

	return polar_evict_buffer(buffer);
}

static void
polar_bg_redo_read_record(XLogReaderState *state, XLogRecPtr lsn)
{
	/* If needed record has not been read, just read it */
	if (state->ReadRecPtr != lsn)
	{
		char	   *errormsg = NULL;
		XLogRecord *record = NULL;

		/*
		 * In XLogReadRecord, it may enlarge buffer of XlogReaderState, so we
		 * should switch context
		 */
		MemoryContext oldcontext = MemoryContextSwitchTo(polar_logindex_memory_context());

		XLogBeginRead(state, lsn);
		record = XLogReadRecord(state, &errormsg);
		MemoryContextSwitchTo(oldcontext);

		if (record == NULL)
		{
			POLAR_LOG_CONSISTENT_LSN();
			POLAR_LOG_XLOG_RECORD_INFO(state);

			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read record from WAL at %lX", lsn)));
		}
	}
}

static int
polar_bg_redo_get_record_id(XLogReaderState *state, BufferTag *tag)
{
	int			i;

	for (i = 0; i <= XLogRecMaxBlockId(state); i++)
	{
		BufferTag	blk_tag;

		POLAR_GET_LOG_TAG(state, blk_tag, i);

		if (BUFFERTAGS_EQUAL(blk_tag, *tag))
			return i;
	}

	elog(PANIC, "Failed to find block id in XLogRecord when lsn=%lX for " POLAR_LOG_BUFFER_TAG_FORMAT,
		 state->ReadRecPtr, POLAR_LOG_BUFFER_TAG(tag));

	return -1;
}

static void
polar_bg_redo_apply_read_record(polar_logindex_redo_ctl_t instance, XLogReaderState *state, XLogRecPtr lsn,
								BufferTag *tag, Buffer *buffer)
{
	BufferDesc *buf_desc;
	Page		page;

	POLAR_ASSERT_PANIC(buffer != NULL);

	polar_bg_redo_read_record(state, lsn);

	if (BufferIsInvalid(*buffer))
	{
		int			blk_id = polar_bg_redo_get_record_id(state, tag);

		/*
		 * When create new page, it call log_newpage which is FPI xlog record
		 * and then extend the file. So during online promote when replay FPI
		 * xlog record, RBM_NORMAL_NO_LOG could return InvalidBuffer. We use
		 * flag RBM_ZERO_ON_ERROR which will extend the file if page does not
		 * exist.
		 *
		 * Note: Here's another similar situation: the primary node may
		 * truncate self-created relation file before flushing abort record
		 * during aborting transaction, then abort record could have no chance
		 * to be flushed after immediate shutdown command. It's fixed by
		 * forcing to flush abort record before truncating self-created
		 * relations whose details can be found in function
		 * RecordTransactionAbort.
		 */
		if (XLogRecBlockImageApply(state, blk_id))
		{
			LWLockAcquire(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache), LW_SHARED);

			/* Don't replay this block if it's truncated or dropped */
			if (polar_check_rel_block_valid_only(instance->rel_size_cache, lsn, tag))
			{
				*buffer = XLogReadBufferExtended(tag->rnode, tag->forkNum, tag->blockNum, RBM_ZERO_ON_ERROR,
												 XLogRecGetBlock(state, blk_id)->prefetch_buffer);
				LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache));
			}
			else
			{
				LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache));
				return;
			}
		}
	}

	POLAR_ASSERT_PANIC(BufferIsValid(*buffer));
	buf_desc = GetBufferDescriptor(*buffer - 1);
	page = BufferGetPage(*buffer);

	if (unlikely(polar_trace_logindex_messages <= DEBUG4))
	{
		POLAR_LOG_BUFFER_DESC(buf_desc);
		POLAR_LOG_PAGE_INFO(page);
		POLAR_LOG_CONSISTENT_LSN();
		POLAR_LOG_XLOG_RECORD_INFO(state);
	}

	/*
	 * Check redo state again, if it's replaying then this xlog will be
	 * replayed by replaying process.
	 */
	polar_lock_buffer_ext(*buffer, BUFFER_LOCK_EXCLUSIVE, false);

	/* Critical section begin */

	/* Apply current record on page */
	if (PageGetLSN(page) <= lsn)
		polar_logindex_apply_one_record(instance, state, tag, buffer);

	polar_promote_mark_buf_dirty(instance, *buffer, lsn);

	/* Critical section end */
	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
}

/* Pin buffer if it exists in the buffer pool */
static Buffer
polar_pin_buffer_for_replay(BufferTag *tag, XLogRecPtr lsn, uint32 bg_redo_state, bool *is_flushed)
{
	uint32		hash;
	LWLock	   *partition_lock;
	int			buf_id;

	POLAR_ASSERT_PANIC(tag != NULL);
	hash = BufTableHashCode(tag);
	partition_lock = BufMappingPartitionLock(hash);

	/* See if the block is in the buffer pool already */
	LWLockAcquire(partition_lock, LW_SHARED);
	buf_id = BufTableLookup(tag, hash);

	/* If page is in buffer, we can apply record, otherwise we do nothing */
	if (buf_id >= 0)
	{
		Buffer		buffer;
		BufferDesc *buf_desc = GetBufferDescriptor(buf_id);
		bool		valid;

		/* Pin buffer from being evicted */
		valid = polar_pin_buffer(buf_desc, NULL);
		/* Can release the mapping lock as soon as possible */
		LWLockRelease(partition_lock);

		buffer = BufferDescriptorGetBuffer(buf_desc);

		if (!valid)
		{
			uint32		redo_state = pg_atomic_read_u32(&buf_desc->polar_redo_state);

			/*
			 * We can only replay buffer after POLAR_REDO_IO_END is set, which
			 * means buffer content is read from storage. The backend process
			 * clear POLAR_REDO_REPLAYING and then set BM_VALID flag. If
			 * startup parse xlog and set POLAR_REDO_OUTDATE,
			 * POLAR_REDO_REPLAYING flag is cleard but BM_VALID is not set,
			 * then we need to replay this buffer
			 */
			if ((redo_state & POLAR_REDO_READ_IO_END) &&
				(redo_state & POLAR_REDO_OUTDATE) && !(redo_state & POLAR_REDO_REPLAYING))
				return buffer;

			/*
			 * Invalid buffer means some other process is reading or replaying
			 * this page currently. After reading or replaying, xlog replay
			 * will be done, so we don't need to replay it. Even error occurs
			 * while io, invalid buffer will be read from disk next access
			 * try, or evicted from buffer pool if no one else access it.
			 */
			ReleaseBuffer(buffer);
			return InvalidBuffer;
		}

		return buffer;
	}
	else
		LWLockRelease(partition_lock);

	return InvalidBuffer;
}

static buf_replay_stat_t
polar_buffer_need_replay(polar_logindex_redo_ctl_t instance, Buffer buffer, XLogRecPtr lsn)
{
	XLogRecPtr	page_lsn;
	BufferDesc *buf_desc = GetBufferDescriptor(buffer - 1);
	uint32		redo_state = pg_atomic_read_u32(&buf_desc->polar_redo_state);
	char	   *page = BufferGetPage(buffer);

	/*
	 * Background process don't need to replay if Buffer is replaying or
	 * already replayed
	 */
	if (redo_state & POLAR_REDO_INVALIDATE)
		return BUF_IS_TRUNCATED;
	else if (redo_state & POLAR_REDO_REPLAYING)
		return BUF_IS_REPLAYING;

	if (PageIsNew(page) || PageIsEmpty(page))
	{
		bool		in_range;

		LWLockAcquire(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache), LW_SHARED);
		in_range = polar_check_rel_block_valid_only(instance->rel_size_cache, lsn, &buf_desc->tag);
		LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache));

		if (!in_range)
			return BUF_IS_TRUNCATED;
	}
	else
	{
		page_lsn = BufferGetLSNAtomic(buffer);

		if (!XLogRecPtrIsInvalid(page_lsn))
		{
			/*
			 * Background process don't need to replay if page lsn is larger
			 * than xlog record's lsn, which means this record is already
			 * replayed by backend process
			 */
			if (page_lsn > lsn)
				return BUF_IS_REPLAYED;
		}
	}

	return BUF_NEED_REPLAY;
}

/*
 * POLAR: Apply one record on one page.
 *
 * We will check existence of buffer page in buffer pool first. If hit, replay record on it,
 * otherwise do nothing. While buffer hit, buffer should be pinned before replaying from being
 * evicted in which target buffer may be not the requested one.
 *
 * If pinned buffer is invalid, buffer is doing io now. After io operation, page will be replayed,
 * so no need to replay it in background.
 */
static void
polar_only_replay_exists_buffer(polar_logindex_redo_ctl_t instance, XLogReaderState *state, log_index_lsn_t * log_index_page)
{
	Buffer		buffer;

	POLAR_ASSERT_PANIC(log_index_page != NULL);

	buffer = polar_pin_buffer_for_replay(log_index_page->tag, log_index_page->lsn,
										 polar_get_bg_redo_state(instance), NULL);

	/* If page is in buffer, we can apply record, otherwise we do nothing */
	if (BufferIsValid(buffer))
	{
		buf_replay_stat_t stat = polar_buffer_need_replay(instance, buffer, log_index_page->lsn);

		if (stat == BUF_NEED_REPLAY)
		{
			XLogRecPtr	consist_lsn = polar_get_primary_consistent_lsn();

			if (consist_lsn > log_index_page->lsn && polar_replica_evict_buffer(buffer))
			{
				elog(polar_trace_logindex(DEBUG3),
					 "Evict buffer=%d when consist_lsn=%lX and xlog_lsn=%lX for " POLAR_LOG_BUFFER_TAG_FORMAT,
					 buffer, consist_lsn, log_index_page->lsn, POLAR_LOG_BUFFER_TAG(log_index_page->tag));
				buffer = InvalidBuffer;
			}
			else
				polar_bg_redo_apply_read_record(instance, state, log_index_page->lsn, log_index_page->tag, &buffer);
		}
		else
			elog(polar_trace_logindex(DEBUG3),
				 "The buf state=%d, so don't replay buf=%d when lsn=%lX for " POLAR_LOG_BUFFER_TAG_FORMAT,
				 stat, buffer, log_index_page->lsn, POLAR_LOG_BUFFER_TAG(log_index_page->tag));
	}

	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);
}

/*
 * POLAR: Return max lsn of a and b.
 */
XLogRecPtr
polar_max_xlog_rec_ptr(XLogRecPtr a, XLogRecPtr b)
{
	if (XLogRecPtrIsInvalid(a))
		return b;
	else if (XLogRecPtrIsInvalid(b))
		return a;
	else
		return Max(a, b);
}

/*
 * POLAR: Return min lsn of a and b.
 */
XLogRecPtr
polar_min_xlog_rec_ptr(XLogRecPtr a, XLogRecPtr b)
{
	if (XLogRecPtrIsInvalid(a))
		return b;
	else if (XLogRecPtrIsInvalid(b))
		return a;
	else
		return Min(a, b);
}

/*
 * POLAR: Lock redo state - set POLAR_REDO_LOCKED in redo state
 */
uint32
polar_lock_redo_state(BufferDesc *desc)
{
	SpinDelayStatus delayStatus;
	uint32		old_redo_state;

	init_local_spin_delay(&delayStatus);

	while (true)
	{
		/* set POLAR_REDO_LOCKED flag */
		old_redo_state = pg_atomic_fetch_or_u32(&desc->polar_redo_state, POLAR_REDO_LOCKED);

		/* if it wasn't set before we're OK */
		if (!(old_redo_state & POLAR_REDO_LOCKED))
			break;

		perform_spin_delay(&delayStatus);
	}

	finish_spin_delay(&delayStatus);
	return old_redo_state | POLAR_REDO_LOCKED;
}

void
polar_log_page_iter_context(void)
{
	if (iter_context != NULL)
	{
		BufferTag  *tag = polar_logindex_page_iterator_buf_tag(iter_context);

		elog(LOG, "Page iter start_lsn=%lX, end_lsn=%lX for page=" POLAR_LOG_BUFFER_TAG_FORMAT,
			 polar_logindex_page_iterator_min_lsn(iter_context),
			 polar_logindex_page_iterator_max_lsn(iter_context), POLAR_LOG_BUFFER_TAG(tag));
	}
}

static Size
polar_logindex_redo_ctl_shmem_size(void)
{
	return MAXALIGN(sizeof(polar_logindex_redo_ctl_data_t));
}

Size
polar_logindex_redo_shmem_size(void)
{
	Size		size = 0;

	if (polar_logindex_mem_size <= 0 || !IsPostmasterEnvironment)
		return size;

	size = add_size(size, polar_logindex_redo_ctl_shmem_size());

	size = add_size(size, polar_logindex_mini_trans_shmem_size());

	size = add_size(size,
					polar_logindex_shmem_size(polar_logindex_convert_mem_tbl_size(polar_logindex_mem_size * WAL_LOGINDEX_MEM_RATIO),
											  polar_logindex_bloom_blocks * WAL_LOGINDEX_MEM_RATIO));

	if (polar_rel_size_cache_blocks > 0)
		size = add_size(size, polar_rel_size_shmem_size(polar_rel_size_cache_blocks));

	if (polar_xlog_queue_buffers > 0)
		size = add_size(size, polar_xlog_queue_size(polar_xlog_queue_buffers));

	if (polar_logindex_max_local_cache_segments > 0)
		size = add_size(size, polar_local_cache_shmem_size(polar_logindex_max_local_cache_segments));

	size = add_size(size,
					polar_logindex_shmem_size(polar_logindex_convert_mem_tbl_size(polar_logindex_mem_size * FULLPAGE_LOGINDEX_MEM_RATIO),
											  polar_logindex_bloom_blocks * FULLPAGE_LOGINDEX_MEM_RATIO));

	size = add_size(size, polar_fullpage_shmem_size());

	if (polar_logindex_max_local_cache_segments > 0)
		size = add_size(size, polar_local_cache_shmem_size(polar_logindex_max_local_cache_segments));

	if (polar_parallel_replay_proc_num > 0 && polar_parallel_replay_task_queue_depth > 0)
	{
		size = add_size(size, polar_calc_task_sched_shmem_size(polar_parallel_replay_proc_num,
															   sizeof(parallel_replay_task_node_t), polar_parallel_replay_task_queue_depth));
	}

	return size;
}

static polar_logindex_redo_ctl_t
polar_logindex_redo_ctl_init(const char *name)
{
	bool		found;
	polar_logindex_redo_ctl_t ctl;

	ctl = (polar_logindex_redo_ctl_t) ShmemInitStruct(name, polar_logindex_redo_ctl_shmem_size(), &found);

	if (!IsUnderPostmaster)
	{
		POLAR_ASSERT_PANIC(!found);
		MemSet(ctl, 0, sizeof(polar_logindex_redo_ctl_data_t));

		/* Init logindex memory context which is static variable */
		polar_logindex_memory_context();
		/* Init logindex redo memory context which is static variable */
		polar_redo_memory_context();
	}
	else
		POLAR_ASSERT_PANIC(found);

	return ctl;
}

static bool
polar_logindex_redo_table_flushable(struct log_mem_table_t *table, void *data)
{
	return POLAR_LOGINDEX_FLUSHABLE_LSN() > polar_logindex_mem_table_max_lsn(table);
}

void
polar_logindex_redo_shmem_init(void)
{
	polar_logindex_redo_ctl_t instance;

	if (polar_logindex_mem_size <= 0 || !IsPostmasterEnvironment)
		return;

	instance = (polar_logindex_redo_ctl_t) polar_logindex_redo_ctl_init("logindex_redo_ctl");

	instance->mini_trans = polar_logindex_mini_trans_shmem_init("logindex_redo_minitrans");

	instance->wal_logindex_snapshot = polar_logindex_snapshot_shmem_init(WAL_LOGINDEX_DIR,
																		 polar_logindex_convert_mem_tbl_size(polar_logindex_mem_size * WAL_LOGINDEX_MEM_RATIO),
																		 polar_logindex_bloom_blocks * WAL_LOGINDEX_MEM_RATIO, LWTRANCHE_WAL_LOGINDEX_BEGIN, LWTRANCHE_WAL_LOGINDEX_END,
																		 polar_logindex_redo_table_flushable, NULL);

	if (polar_rel_size_cache_blocks > 0)
		instance->rel_size_cache = polar_rel_size_shmem_init(RELATION_SIZE_CACHE_DIR, polar_rel_size_cache_blocks);
	else
		elog(FATAL, "%s: PolarDB relation size cache use wrong block size %d",
			 PG_FUNCNAME_MACRO, polar_rel_size_cache_blocks);

	if (polar_xlog_queue_buffers > 0)
		instance->xlog_queue = polar_xlog_queue_init("polar_xlog_queue", LWTRANCHE_POLAR_XLOG_QUEUE,
													 polar_xlog_queue_buffers);
	else
		elog(FATAL, "%s: PolarDB xlog queue use wrong buffer size %d",
			 PG_FUNCNAME_MACRO, polar_xlog_queue_buffers);

	if (polar_logindex_max_local_cache_segments > 0)
		polar_logindex_create_local_cache(instance->wal_logindex_snapshot, "wal_logindex",
										  polar_logindex_max_local_cache_segments);

	instance->fullpage_logindex_snapshot = polar_logindex_snapshot_shmem_init(FULLPAGE_LOGINDEX_DIR,
																			  polar_logindex_convert_mem_tbl_size(polar_logindex_mem_size * FULLPAGE_LOGINDEX_MEM_RATIO),
																			  polar_logindex_bloom_blocks * FULLPAGE_LOGINDEX_MEM_RATIO, LWTRANCHE_FULLPAGE_LOGINDEX_BEGIN,
																			  LWTRANCHE_FULLPAGE_LOGINDEX_END, polar_logindex_redo_table_flushable, NULL);

	instance->fullpage_ctl = polar_fullpage_shmem_init(FULLPAGE_LOGINDEX_DIR,
													   instance->xlog_queue, instance->fullpage_logindex_snapshot);

	if (polar_logindex_max_local_cache_segments > 0)
		polar_logindex_create_local_cache(instance->fullpage_logindex_snapshot, "fullpage_logindex",
										  polar_logindex_max_local_cache_segments);

	pg_atomic_init_u32(&instance->bg_redo_state, POLAR_BG_REDO_NOT_START);
	SpinLockInit(&instance->info_lck);

	if (polar_parallel_replay_proc_num > 0 && polar_parallel_replay_task_queue_depth > 0)
	{
		instance->parallel_sched = polar_create_proc_task_sched("polar_parallel_replay_sched",
																polar_parallel_replay_proc_num, sizeof(parallel_replay_task_node_t),
																polar_parallel_replay_task_queue_depth, instance);
	}

	polar_logindex_redo_instance = instance;
}

void
polar_logindex_remove_old_files(polar_logindex_redo_ctl_t instance)
{
	XLogRecPtr	min_lsn;

	if (instance == NULL)
		return;

	min_lsn = polar_calc_min_used_lsn(true);

	/* Truncate relation size cache which saved in local file system */
	if (instance->rel_size_cache)
		polar_truncate_rel_size_cache(instance->rel_size_cache, min_lsn);

	if (polar_is_replica())
		return;

	/* Truncate the following files which saved in shared storage */

	/* Truncate logindex before removing wal files */
	if (instance->wal_logindex_snapshot != NULL)
		polar_logindex_truncate(instance->wal_logindex_snapshot, min_lsn);

	/* Truncate fullpage data */
	if (instance->fullpage_ctl != NULL)
		polar_remove_old_fullpage_files(instance->fullpage_ctl, min_lsn);
}

void
polar_logindex_remove_all(void)
{
	char		path[MAXPGPATH] = {0};
	struct stat statbuf;

	/* replica cannot remove file from storage */
	if (polar_is_replica())
		return;

	POLAR_FILE_PATH(path, WAL_LOGINDEX_DIR);
	if (polar_stat(path, &statbuf) == 0)
		rmtree(path, false);

	POLAR_FILE_PATH(path, FULLPAGE_LOGINDEX_DIR);
	if (polar_stat(path, &statbuf) == 0)
		rmtree(path, false);
}

XLogRecPtr
polar_logindex_redo_start_lsn(polar_logindex_redo_ctl_t instance)
{
	if (instance && instance->wal_logindex_snapshot)
		return polar_logindex_start_lsn(instance->wal_logindex_snapshot);

	return InvalidXLogRecPtr;
}

XLogRecPtr
polar_logindex_redo_init(polar_logindex_redo_ctl_t instance, XLogRecPtr checkpoint_lsn,
						 TimeLineID checkpoint_tli, bool read_only)
{
	XLogRecPtr	start_lsn = checkpoint_lsn;

	if (instance)
	{
		if (instance->wal_logindex_snapshot)
			start_lsn = polar_logindex_snapshot_init(instance->wal_logindex_snapshot, checkpoint_lsn, checkpoint_tli, read_only, false);

		if (instance->fullpage_logindex_snapshot)
			polar_logindex_snapshot_init(instance->fullpage_logindex_snapshot, checkpoint_lsn, checkpoint_tli, read_only, true);

		elog(LOG, "PolarDB logindex change from %lX to %lX", checkpoint_lsn, start_lsn);

		instance->xlog_replay_from = checkpoint_lsn;
	}

	return start_lsn;
}

void
polar_logindex_redo_flush_data(polar_logindex_redo_ctl_t instance, XLogRecPtr checkpoint_lsn)
{
	if (polar_is_replica() || !instance)
		return;

	if (instance->fullpage_logindex_snapshot)
		polar_logindex_flush_table(instance->fullpage_logindex_snapshot, checkpoint_lsn, true);

	if (instance->wal_logindex_snapshot)
		polar_logindex_flush_table(instance->wal_logindex_snapshot, checkpoint_lsn, false);
}

bool
polar_logindex_redo_bg_flush_data(polar_logindex_redo_ctl_t instance)
{
	bool		write_done = true;

	if (likely(instance))
	{
		if (instance->fullpage_logindex_snapshot)
			write_done &= polar_logindex_bg_write(instance->fullpage_logindex_snapshot);

		write_done &= polar_logindex_bg_write(instance->wal_logindex_snapshot);
	}

	return write_done;
}

static bool
polar_logindex_bg_dispatch(polar_logindex_bg_redo_ctl_t *ctl, bool *can_hold)
{
	polar_task_sched_ctl_t *sched_ctl = ctl->sched_ctl;
	bool		dispatch_done = true;
	XLogRecPtr	bg_replayed_lsn,
				backend_min_lsn,
				xlog_replayed_lsn,
				replayed_oldest_lsn;

#ifdef FAULT_INJECTOR
	XLogRecPtr	prev_replay_lsn = InvalidXLogRecPtr;
#endif

	*can_hold = false;

	/* Remove finished task from running queue */
	if (!polar_sched_empty_running_task(sched_ctl))
		polar_sched_remove_finished_task(sched_ctl);

	/* Dispatch new task to replay */
	do
	{
		parallel_replay_task_node_t node,
				   *dst_node;
		BufferTag  *tag;

		xlog_replayed_lsn = polar_get_xlog_replay_recptr_nolock();

		if (!ctl->replay_page)
			ctl->replay_page = polar_logindex_lsn_iterator_next(ctl->instance->wal_logindex_snapshot,
																ctl->lsn_iter);

		/* If no more item in logindex, exit and retry next run */
		if (!ctl->replay_page)
			break;

		POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(ctl->replay_page->lsn));

		/* If current xlog record is beyond replayed lsn, just break */
		if (ctl->replay_page->lsn > polar_get_last_replayed_read_ptr())
			break;

		/* inject fault */
#ifdef FAULT_INJECTOR
		if (!XLogRecPtrIsInvalid(prev_replay_lsn) &&
			ctl->replay_page->lsn < prev_replay_lsn)
			elog(PANIC, "current replay lsn:%X/%X is smaller than previous replay lsn:%X/%X",
				 LSN_FORMAT_ARGS(ctl->replay_page->lsn), LSN_FORMAT_ARGS(prev_replay_lsn));

		if (!XLogRecPtrIsInvalid(prev_replay_lsn) &&
			(ctl->replay_page->lsn - prev_replay_lsn) > wal_segment_size / 2 &&
			SIMPLE_FAULT_INJECTOR("polar_inject_panic") == FaultInjectorTypeEnable)
		{
			RequestCheckpoint(CHECKPOINT_FLUSH_ALL | CHECKPOINT_INCREMENTAL | CHECKPOINT_WAIT);
			elog(PANIC, "test inject panic after flush all buffers, cur replay lsn:%X/%X", LSN_FORMAT_ARGS(ctl->replay_page->lsn));
		}
		prev_replay_lsn = ctl->replay_page->lsn;
#endif
		/* inject fault end */

		tag = ctl->replay_page->tag;
		INIT_BUFFERTAG(node.tag, tag->rnode, tag->forkNum, tag->blockNum);
		node.lsn = ctl->replay_page->lsn;
		node.prev_lsn = ctl->replay_page->prev_lsn;

		dst_node = (parallel_replay_task_node_t *) polar_sched_add_task(sched_ctl, (polar_task_node_t *) &node);

		if (dst_node != NULL)
		{
			int			proc = POLAR_TASK_NODE_PROC((polar_task_node_t *) dst_node);

			ctl->max_dispatched_lsn = node.lsn;
			ctl->replay_page = NULL;

			ereport(polar_trace_logindex(DEBUG2), (errmsg("Dispatch lsn=%lX, " POLAR_LOG_BUFFER_TAG_FORMAT " to proc=%d",
														  dst_node->lsn, POLAR_LOG_BUFFER_TAG(&dst_node->tag),
														  sched_ctl->sub_proc[proc].proc->pid),
												   errhidestmt(true), errhidecontext(true)));
		}
		else
		{
			/*
			 * We can not add this task because task queue is full, so the
			 * caller can hold a while
			 */
			*can_hold = true;
			dispatch_done = false;
			/* Break this loop when fail to dispatch */
			break;
		}
	}
	while (true);

	/*
	 * Set dispatch_done to be true when there's no running task and no new
	 * WAL to dispatch.
	 *
	 * If dispatch done, bg_replayed_lsn need to be forwarded to
	 * XLogRecoveryCtl->lastReplayedEndRecPtr to make it able to do the latest
	 * ckpt.
	 *
	 * Notice: we need to fetch XLogRecoveryCtl->lastReplayedEndRecPtr at the
	 * very beginning, because WALs may be appended into logindex snapshot
	 * during our check. Forwarding to an improper RecPtr may skip the newly
	 * added WALs without replaying.
	 */
	if (dispatch_done)
		dispatch_done = polar_sched_empty_running_task(sched_ctl);

	if (dispatch_done)
	{
		POLAR_ASSERT_PANIC(ctl->instance != NULL);
		polar_bg_redo_set_replayed_lsn(ctl->instance, Max(xlog_replayed_lsn, ctl->max_dispatched_lsn));
		elog(polar_trace_logindex(DEBUG3), "%s: forward bg_replayed_lsn to %lX", PG_FUNCNAME_MACRO, xlog_replayed_lsn);
	}

	bg_replayed_lsn = polar_bg_redo_get_replayed_lsn(ctl->instance);
	pg_read_barrier();

	backend_min_lsn = polar_get_backend_min_replay_lsn();

	/*
	 * The backend_min_lsn is non-linearly increasing, so we must use the
	 * smaller value of bg_replayed_lsn and backend_min_lsn.
	 */
	if (!XLogRecPtrIsInvalid(backend_min_lsn) && backend_min_lsn < bg_replayed_lsn)
		replayed_oldest_lsn = backend_min_lsn;
	else
		replayed_oldest_lsn = bg_replayed_lsn;

	POLAR_ASSERT_PANIC(ctl->instance->replayed_oldest_lsn <= replayed_oldest_lsn);
	ctl->instance->replayed_oldest_lsn = replayed_oldest_lsn;

	return dispatch_done;
}

static bool
polar_logindex_bg_online_promote(polar_logindex_bg_redo_ctl_t *ctl, bool *can_hold)
{
	bool		dispatch_done = polar_logindex_bg_dispatch(ctl, can_hold);
	polar_logindex_redo_ctl_t instance = ctl->instance;

	if (dispatch_done && XLogRecPtrIsInvalid(polar_get_backend_min_replay_lsn()))
	{
		polar_set_bg_redo_state(instance, POLAR_BG_REDO_NOT_START);

		/*
		 * Request an (online) checkpoint now. This isn't required for
		 * consistency, but the last restartpoint might be far back, and in
		 * case of a crash, recovering from it might take a longer than is
		 * appropriate now that we're not in standby mode anymore.
		 */
		RequestCheckpoint(CHECKPOINT_FORCE);

		elog(LOG, "Background process finished replay for online promote, last_replayed_end_lsn=%lX",
			 polar_bg_redo_get_replayed_lsn(instance));
	}

	return dispatch_done;
}

bool
polar_logindex_redo_bg_replay(polar_logindex_bg_redo_ctl_t *ctl, bool *can_hold)
{
	uint32		state;
	bool		replay_done = true;

	POLAR_ASSERT_PANIC(ctl != NULL);

	/* inject fault */
#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("polar_logindex_bg_skip_replay") == FaultInjectorTypeEnable)
		return false;
#endif
	/* inject fault end */

	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	state = polar_get_bg_redo_state(ctl->instance);
	/* Make sure we get background redo state */

	*can_hold = false;

	pg_read_barrier();

	switch (state)
	{
		case POLAR_BG_REPLICA_BUF_REPLAYING:
			replay_done = polar_logindex_apply_xlog_background(ctl);
			break;

		case POLAR_BG_PARALLEL_REPLAYING:
			replay_done = polar_logindex_bg_dispatch(ctl, can_hold);
			break;

		case POLAR_BG_ONLINE_PROMOTE:
			replay_done = polar_logindex_bg_online_promote(ctl, can_hold);
			break;

		default:
			break;
	}

	return replay_done;
}

bool
polar_logindex_bg_promoted(polar_logindex_redo_ctl_t instance)
{
	return polar_get_bg_redo_state(instance) == POLAR_BG_REDO_NOT_START;
}

void
polar_logindex_redo_online_promote(polar_logindex_redo_ctl_t instance)
{
	if (!instance || !instance->wal_logindex_snapshot)
		return;

	if (instance->fullpage_logindex_snapshot)
		polar_logindex_online_promote(instance->fullpage_logindex_snapshot, true);

	polar_logindex_online_promote(instance->wal_logindex_snapshot, false);
}

void
polar_logindex_redo_abort(polar_logindex_redo_ctl_t instance)
{
	if (!instance)
		return;

	polar_logindex_abort_replaying_buffer();

	if (instance->mini_trans)
		polar_logindex_abort_mini_transaction(instance->mini_trans);

	if (wal_page_iter != NULL)
	{
		polar_logindex_release_page_iterator(wal_page_iter);
		wal_page_iter = NULL;
	}

	if (fullpage_page_iter != NULL)
	{
		polar_logindex_release_page_iterator(fullpage_page_iter);
		fullpage_page_iter = NULL;
	}
}

XLogRecPtr
polar_logindex_redo_get_min_replay_from_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr consist_lsn)
{
	XLogRecPtr	result = consist_lsn;
	XLogRecPtr	bg_replayed_lsn;

	if (!instance || !instance->wal_logindex_snapshot)
		return result;

	if (instance->fullpage_logindex_snapshot)
	{
		/* This checkpoint lsn can avoid clear xlog when restore old fullpage */
		XLogRecPtr	last_checkpoint_redo_lsn = GetRedoRecPtr();

		if (!XLogRecPtrIsInvalid(result) &&
			!XLogRecPtrIsInvalid(last_checkpoint_redo_lsn) &&
			last_checkpoint_redo_lsn < result)
			result = last_checkpoint_redo_lsn;
	}

	bg_replayed_lsn = polar_bg_redo_get_replayed_lsn(instance);

	if (!XLogRecPtrIsInvalid(bg_replayed_lsn) && bg_replayed_lsn < result)
		result = bg_replayed_lsn;

	return result;
}

/* Apply xlog for buffer which already acquired buffer io lock */
bool
polar_logindex_io_lock_apply(polar_logindex_redo_ctl_t instance, BufferDesc *buf_hdr,
							 XLogRecPtr replay_from, XLogRecPtr checkpoint_lsn)
{
	Buffer		buffer;
	XLogRecPtr	start_lsn = replay_from;
	bool		lsn_changed = false;

	POLAR_ASSERT_PANIC(buf_hdr != NULL);

	if (unlikely(InRecovery && !reachedConsistency))
	{
		POLAR_LOG_BACKTRACE();

		elog(PANIC, "%s apply buffer before reached consistency %X/%X, " POLAR_LOG_BUFFER_TAG_FORMAT, PG_FUNCNAME_MACRO,
			 LSN_FORMAT_ARGS(replay_from),
			 POLAR_LOG_BUFFER_TAG(&buf_hdr->tag));

		return lsn_changed;
	}

	buffer = BufferDescriptorGetBuffer(buf_hdr);
	POLAR_ASSERT_PANIC(BufferIsValid(buffer));

	/*
	 * If we read a future page, then restore old fullpage version and reset
	 * start_lsn
	 */
	if (polar_is_replica() &&
		polar_logindex_restore_fullpage_snapshot_if_needed(instance, &buf_hdr->tag, &buffer))
	{
		start_lsn = checkpoint_lsn;
		POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(start_lsn));
	}

	lsn_changed = polar_logindex_lock_apply_page_from(instance, start_lsn, &buf_hdr->tag, &buffer);

	/*
	 * If we read buffer from storage and have no xlog to do replay, then it
	 * does not necessary to mark it dirty
	 */
	if (lsn_changed)
		polar_promote_mark_buf_dirty(instance, buffer, start_lsn);

	return lsn_changed;
}

/*
 * POLAR: 1. Read xlog record from xlog queue and save to logindex
 * 2. Remove unnecessary data from xlog queue
 * 3. Flush fullpage active logindex table
 */
void
polar_logindex_primary_save(polar_logindex_redo_ctl_t instance)
{
	DecodedXLogRecord *decoded;
	static XLogReaderState *state = NULL;
	static TimestampTz last_flush_time = 0;

	if (!instance || !instance->xlog_queue)
		return;

	if (state == NULL)
		state = XLogReaderAllocate(wal_segment_size, NULL,
								   XL_ROUTINE(.page_read = NULL,
											  .segment_open = NULL,
											  .segment_close = NULL),
								   NULL);

	while ((decoded = polar_xlog_send_queue_record_pop(instance->xlog_queue, state)))
	{
		if (polar_xlog_remove_payload(&decoded->header))
			polar_logindex_save_lsn(instance, state);
	}

	polar_xlog_send_queue_keep_data(instance->xlog_queue);

	if (instance->fullpage_logindex_snapshot)
	{
		TimestampTz now = GetCurrentTimestamp();

		if (TimestampDifferenceExceeds(last_flush_time, now,
									   polar_write_logindex_active_table_delay))
		{
			polar_logindex_flush_table(instance->fullpage_logindex_snapshot, InvalidXLogRecPtr, true);
			last_flush_time = now;
		}
	}
}

void
polar_set_bg_redo_state(polar_logindex_redo_ctl_t instance, uint32 state)
{
	if (instance)
		pg_atomic_write_u32(&instance->bg_redo_state, state);
}

/*
 * POLAR: Set background replayed lsn.
 */
void
polar_bg_redo_set_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr lsn)
{
	if (instance)
	{
		SpinLockAcquire(&instance->info_lck);
		instance->bg_replayed_lsn = lsn;
		SpinLockRelease(&instance->info_lck);
	}
}

/*
 * POLAR: Get background replayed lsn.
 */
XLogRecPtr
polar_bg_redo_get_replayed_lsn(polar_logindex_redo_ctl_t instance)
{
	XLogRecPtr	lsn = InvalidXLogRecPtr;

	if (instance)
	{
		SpinLockAcquire(&instance->info_lck);
		lsn = instance->bg_replayed_lsn;
		SpinLockRelease(&instance->info_lck);
	}

	return lsn;
}

void
polar_logindex_wakeup_bg_replay(polar_logindex_redo_ctl_t instance, XLogRecPtr replayed_lsn)
{
	if (instance)
	{
		uint32		state = POLAR_BG_REDO_NOT_START;

		SpinLockAcquire(&instance->info_lck);
		instance->bg_replayed_lsn = replayed_lsn;
		SpinLockRelease(&instance->info_lck);

		elog(LOG, "instance bg_replayed_lsn set to %lX", replayed_lsn);

		if (POLAR_ENABLE_PARALLEL_REPLAY_STANDBY_MODE() ||
			(polar_is_replica() && polar_enable_replica_prewarm))
		{
			instance->replayed_oldest_lsn = replayed_lsn;

			state = POLAR_BG_PARALLEL_REPLAYING;
		}
		else if (polar_is_replica())
			state = POLAR_BG_REPLICA_BUF_REPLAYING;

		if (state != POLAR_BG_REDO_NOT_START)
		{
			pg_write_barrier();
			polar_set_bg_redo_state(instance, state);

			NOTIFY_LOGINDEX_BG_WORKER(instance->bg_worker_latch);

			if (instance->fullpage_ctl)
				polar_fullpage_bgworker_wakeup(instance->fullpage_ctl);
		}
	}
}

bool
polar_logindex_require_backend_redo(polar_logindex_redo_ctl_t instance, ForkNumber fork_num, XLogRecPtr *replay_from)
{
	bool		required = true;

	if (!instance)
		return false;

	POLAR_ASSERT_PANIC(replay_from != NULL);
	*replay_from = InvalidXLogRecPtr;

	/*
	 * POLAR: There are three ways to make backends replaying with logindex.
	 * (1) When backends are running in replica node. (2) When backends are
	 * running in standby node with enabled parallel replaying. (3) When
	 * backends are running in new promoted primary node without finishing
	 * online promote work from old replica node.
	 */
	if (polar_is_replica())
	{
		XLogRecPtr	redo_lsn = GetRedoRecPtr();

		/*
		 * POLAR: After replication is terminated, replica doesn't know what
		 * happened to primary. Meanwhile, if primary is in recovery state, it
		 * would reflushed some data block to make page's lsn lower than
		 * consist_lsn. If replica read these old pages and replay them from
		 * consist_lsn, replica will PANIC! So, we don't want replica replay
		 * data block from consist_lsn after replication is not streaming.
		 */
		XLogRecPtr	consist_lsn = polar_get_primary_consistent_lsn();

		*replay_from = (!XLogRecPtrIsInvalid(consist_lsn) && WalRcvStreaming()) ? consist_lsn : redo_lsn;
		POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(redo_lsn));
		POLAR_SET_BACKEND_READ_MIN_LSN(redo_lsn);
	}
	else if (POLAR_IN_PARALLEL_REPLAY_STANDBY_MODE(instance))
	{
		XLogRecPtr	redo_lsn = GetRedoRecPtr();

		/*
		 * POLAR: The replay_from should be set to bg_replayed_lsn in parallel
		 * replaying mode for standby.
		 */
		SpinLockAcquire(&instance->info_lck);
		if (XLogRecPtrIsInvalid(instance->bg_replayed_lsn))
			*replay_from = redo_lsn;
		else
			*replay_from = instance->bg_replayed_lsn;
		POLAR_SET_BACKEND_READ_MIN_LSN(*replay_from);
		SpinLockRelease(&instance->info_lck);
		POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(*replay_from));
	}
	else
	{
		required = false;
		if (polar_get_bg_redo_state(instance) == POLAR_BG_ONLINE_PROMOTE)
		{
			XLogRecPtr	last_replayed_read_ptr = polar_get_last_replayed_read_ptr();

			SpinLockAcquire(&instance->info_lck);

			if (instance->bg_replayed_lsn <= last_replayed_read_ptr)
			{
				*replay_from = instance->bg_replayed_lsn;
				required = true;
				POLAR_SET_BACKEND_READ_MIN_LSN(*replay_from);
			}

			SpinLockRelease(&instance->info_lck);
		}

		if (required)
		{
			XLogRecPtr	consist_lsn = polar_get_primary_consistent_lsn();

			if (!XLogRecPtrIsInvalid(consist_lsn))
				*replay_from = Max(consist_lsn, *replay_from);
		}
	}

	return required;
}

/*
 * POLAR: set valid information to enable logindex parse.
 * set it only when logindex snapshot state is writable in current node.
 * For Primary/Standby node, logindex snapshot is writable
 * For Replica node, logindex snapshot is readonly
 * For Datamax node, logindex won't be enabled
 */
void
polar_logindex_redo_set_valid_info(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn)
{
	POLAR_ASSERT_PANIC(instance != NULL);

	if (polar_logindex_check_state(instance->wal_logindex_snapshot, POLAR_LOGINDEX_STATE_WRITABLE)
		&& XLogRecPtrIsInvalid(polar_logindex_check_valid_start_lsn(instance->wal_logindex_snapshot)))
		polar_logindex_set_start_lsn(instance->wal_logindex_snapshot, start_lsn);

	if (polar_logindex_check_state(instance->fullpage_logindex_snapshot, POLAR_LOGINDEX_STATE_WRITABLE)
		&& XLogRecPtrIsInvalid(polar_logindex_check_valid_start_lsn(instance->fullpage_logindex_snapshot)))
		polar_logindex_set_start_lsn(instance->fullpage_logindex_snapshot, start_lsn);
}

XLogRecPtr
polar_logindex_replayed_oldest_lsn(void)
{
	POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(polar_logindex_redo_instance->replayed_oldest_lsn));

	return polar_logindex_redo_instance->replayed_oldest_lsn;
}

static void
polar_clean_redo_context(int code, Datum arg)
{
	if (polar_redo_context)
	{
		MemoryContextDelete(polar_redo_context);
		polar_redo_context = NULL;
	}
}

static MemoryContext
polar_redo_memory_context(void)
{
	if (polar_redo_context == NULL)
	{
		polar_redo_context = AllocSetContextCreate(TopMemoryContext,
												   "polar working context",
												   ALLOCSET_DEFAULT_SIZES);

		MemoryContextAllowInCriticalSection(polar_redo_context, true);
		before_shmem_exit(polar_clean_redo_context, 0);
	}

	return polar_redo_context;
}

static void *
get_parallel_replay_task_tag(polar_task_node_t *task)
{
	parallel_replay_task_node_t *parallel_task = (parallel_replay_task_node_t *) task;

	return &parallel_task->tag;
}

static void
parallel_replay_task_finished(polar_task_sched_t *sched, polar_task_node_t *task, polar_logindex_redo_ctl_t instance)
{
	parallel_replay_task_node_t *parallel_task = (parallel_replay_task_node_t *) task;
	parallel_replay_task_node_t *parallel_head;
	XLogRecPtr	head_lsn = InvalidXLogRecPtr;

	SpinLockAcquire(&sched->lock);
	parallel_head = (parallel_replay_task_node_t *) POLAR_SCHED_RUNNING_QUEUE_HEAD(sched);

	head_lsn = parallel_head->lsn;
	SpinLockRelease(&sched->lock);

	POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(head_lsn));

	if (head_lsn == parallel_task->lsn)
	{
		SpinLockAcquire(&instance->info_lck);

		if (instance->bg_replayed_lsn < parallel_task->lsn)
			instance->bg_replayed_lsn = parallel_task->lsn;

		SpinLockRelease(&instance->info_lck);
	}
}

static Buffer
polar_xlog_need_replay(polar_logindex_redo_ctl_t instance, BufferTag *tag, XLogRecPtr lsn, buf_replay_stat_t * buf_stat)
{
	Buffer		buffer;
	bool		is_flushed = false;

	buffer = polar_pin_buffer_for_replay(tag, lsn, polar_get_bg_redo_state(instance), &is_flushed);

	if (BufferIsValid(buffer))
	{
		*buf_stat = polar_buffer_need_replay(instance, buffer, lsn);

		if (*buf_stat == BUF_NEED_REPLAY && polar_is_replica())
		{
			XLogRecPtr	consist_lsn = polar_get_primary_consistent_lsn();

			if (lsn < consist_lsn && polar_replica_evict_buffer(buffer))
			{
				elog(polar_trace_logindex(DEBUG3),
					 "Evict buffer=%d when consist_lsn=%lX and xlog_lsn=%lX for " POLAR_LOG_BUFFER_TAG_FORMAT,
					 buffer, consist_lsn, lsn, POLAR_LOG_BUFFER_TAG(tag));

				buffer = InvalidBuffer;
				*buf_stat = BUF_IS_FLUSHED;
			}
		}
	}
	else
	{
		is_flushed = lsn < polar_get_primary_consistent_lsn();

		if (is_flushed)
		{
			*buf_stat = BUF_IS_FLUSHED;
		}
		else
		{
			LWLockAcquire(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache), LW_SHARED);

			/* Don't replay this block if it's truncated or dropped */
			if (polar_check_rel_block_valid_only(instance->rel_size_cache, lsn, tag))
			{
				buffer = XLogReadBufferExtended(tag->rnode, tag->forkNum, tag->blockNum, RBM_NORMAL_NO_LOG, InvalidBuffer);
				*buf_stat = BUF_NEED_REPLAY;
			}
			else
				*buf_stat = BUF_IS_TRUNCATED;

			LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(instance->rel_size_cache));
		}
	}

	return buffer;
}

static bool
handle_parallel_replay_task(polar_task_sched_t *sched, polar_task_node_t *task)
{
	static XLogReaderState *state = NULL;
	polar_logindex_redo_ctl_t instance = (polar_logindex_redo_ctl_t) (sched->run_arg);
	parallel_replay_task_node_t *parallel_task = (parallel_replay_task_node_t *) task;
	MemoryContext oldcontext = MemoryContextSwitchTo(polar_logindex_memory_context());
	buf_replay_stat_t buf_stat = BUF_NEED_REPLAY;
	bool		handled = true;
	Buffer		buffer;

	POLAR_ASSERT_PANIC(instance != NULL && instance->rel_size_cache != NULL);
	Assert(polar_bg_redo_state_is_parallel(instance));

	if (unlikely(state == NULL))
		state = polar_allocate_xlog_reader();

	buffer = polar_xlog_need_replay(instance, &parallel_task->tag, parallel_task->lsn, &buf_stat);

	if (buf_stat == BUF_NEED_REPLAY)
		polar_bg_redo_apply_read_record(instance, state, parallel_task->lsn, &parallel_task->tag, &buffer);
	else if (buf_stat == BUF_IS_REPLAYING)
		handled = false;
	else if (buf_stat == BUF_IS_REPLAYED && polar_bg_redo_state_is_parallel(instance))
	{
		polar_lock_buffer_ext(buffer, BUFFER_LOCK_EXCLUSIVE, false);
		polar_promote_mark_buf_dirty(instance, buffer, parallel_task->lsn);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}

	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);

	MemoryContextSwitchTo(oldcontext);

	if (handled)
		parallel_replay_task_finished(sched, task, instance);

	return handled;
}

static void
parallel_replay_task_startup(void *run_arg)
{
	/* When do parallel replay we will exit process on any error */
	ExitOnAnyError = true;

	polar_bg_replaying_process = POLAR_LOGINDEX_PARALLEL_REPLAY;

	/* Make sure there's room for us to pin buffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);
}

polar_task_sched_ctl_t *
polar_create_parallel_replay_sched_ctl(polar_logindex_bg_redo_ctl_t *bg_ctl)
{
	polar_task_sched_ctl_t *ctl;
	polar_logindex_redo_ctl_t instance = bg_ctl->instance;

	polar_sched_reg_handler(instance->parallel_sched, parallel_replay_task_startup, handle_parallel_replay_task,
							NULL, get_parallel_replay_task_tag);

	ctl = polar_create_task_sched_ctl(instance->parallel_sched, sizeof(BufferTag),
									  NULL, NULL);

	return ctl;
}

bool
polar_need_do_bg_replay(polar_logindex_redo_ctl_t instance)
{
	if (unlikely(!instance))
		return false;

	return polar_is_replica() ||
		polar_bg_redo_state_is_parallel(instance);
}

static void
polar_online_promote_evict_buffer(XLogRecPtr oldest_redo_ptr)
{
	Buffer		buffer;
	BufferDesc *buf_desc;
	uint32		redo_state;
	XLogRecPtr	page_lsn;
	int			i;

	POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(oldest_redo_ptr));

	elog(LOG, "start evict buffers for online promote, buffer_num:%d", NBuffers);

	for (i = 0; i < NBuffers; i++)
	{
		buf_desc = GetBufferDescriptor(i);
		buffer = BufferDescriptorGetBuffer(buf_desc);

		/* skip this buffer if it's not valid */
		if (!polar_pin_buffer(buf_desc, NULL))
		{
			ReleaseBuffer(buffer);
			continue;
		}

		/*
		 * startup process has parsed all wal, there is no other proc will set
		 * outdate flag
		 */
		redo_state = pg_atomic_read_u32(&buf_desc->polar_redo_state);
		if (!(redo_state & POLAR_REDO_OUTDATE))
		{
			ReleaseBuffer(buffer);
			continue;
		}

		LWLockAcquire(BufferDescriptorGetContentLock(buf_desc), LW_SHARED);
		page_lsn = BufferGetLSNAtomic(buffer);
		LWLockRelease(BufferDescriptorGetContentLock(buf_desc));

		/* skip when page lsn is larger than oldest_redo_ptr */
		if (!XLogRecPtrIsInvalid(page_lsn) && page_lsn >= oldest_redo_ptr)
		{
			ReleaseBuffer(buffer);
			continue;
		}

		/*
		 * buffer is evicted after all normal backends exit, evict operation
		 * shouldn't be failed
		 */
		if (!polar_evict_buffer(buffer))
		{
			ReleaseBuffer(buffer);
			elog(PANIC, "failed to evict buffer=%d oldest_redo_ptr:%X/%X page_lsn:%X/%X for " POLAR_LOG_BUFFER_TAG_FORMAT,
				 buffer, LSN_FORMAT_ARGS(oldest_redo_ptr), LSN_FORMAT_ARGS(page_lsn), POLAR_LOG_BUFFER_TAG(&(buf_desc->tag)));
		}
	}

	elog(LOG, "startup evicts all buffers those latest lsn is smaller than oldest_redo_ptr:%X/%X", LSN_FORMAT_ARGS(oldest_redo_ptr));
}

/*
 * POLAR: oldest_redo_ptr is usually be the redo pointer of the last checkpoint or restartpoint.
 * This is the oldest point in WAL that we still need, if we have to restart recovery.
 * Set bg_replayed_lsn as oldest_redo_ptr
 * Set the state of LogindexBgWriter as OnlinePromoting
 */
void
polar_reset_bg_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr oldest_redo_ptr)
{
	XLogRecPtr	bg_replayed_lsn;
	XLogRecPtr	old_consist_lsn;
	XLogRecPtr	oldest_apply_lsn;

	old_consist_lsn = polar_get_primary_consistent_lsn();

	if (XLogRecPtrIsInvalid(old_consist_lsn))
		old_consist_lsn = oldest_redo_ptr;

	POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(old_consist_lsn));

	/*
	 * The oldest_applied_lsn should be InvalidXLogRecPtr when there's no used
	 * slot.
	 */
	polar_compute_and_set_replica_lsn();
	oldest_apply_lsn = polar_get_oldest_apply_lsn();
	if (!XLogRecPtrIsInvalid(oldest_apply_lsn) && oldest_apply_lsn < old_consist_lsn)
		polar_set_oldest_replica_lsn(old_consist_lsn, InvalidXLogRecPtr);

	bg_replayed_lsn = polar_bg_redo_get_replayed_lsn(instance);

	if (XLogRecPtrIsInvalid(bg_replayed_lsn))
		elog(PANIC, "bg_replayed_lsn is invalid");

	/*
	 * always start replay from oldest_redo_ptr after online promote: 1. when
	 * bg_replayed_lsn < oldest_redo_ptr, we need to evict all buffers those
	 * latest lsn is smaller than oldest_redo_ptr. Otherwise, new primary will
	 * flush older buffer to storage, while older redoptr hasn't been updated
	 * to pg_control, which will lead to data error if crash happend at this
	 * time. 2. when bg_replayed_lsn > oldest_redo_ptr, lsn of page on storage
	 * may be smaller than bg_replayed_lsn, so we still need to replay from
	 * oldest_redo_ptr. Don't use old_consist_lsn either, because it may be
	 * larger than the page_lsn on storage if old primary is in crash recovery
	 * before promote replica, replay from old_consist_lsn may also lead to a
	 * replay error.
	 */
	if (bg_replayed_lsn < oldest_redo_ptr)
		polar_online_promote_evict_buffer(oldest_redo_ptr);

	polar_bg_redo_set_replayed_lsn(instance, oldest_redo_ptr);
	instance->replayed_oldest_lsn = oldest_redo_ptr;

	pg_write_barrier();
	polar_set_bg_redo_state(instance, POLAR_BG_ONLINE_PROMOTE);

	SetLatch(instance->bg_worker_latch);

	elog(LOG, "startup complete online promote, and reset bg_replayed_lsn to oldest_redo_lsn=%lX, old_consist_lsn=%lX",
		 oldest_redo_ptr, old_consist_lsn);
}

void
polar_logindex_promote_xlog_queue(polar_logindex_redo_ctl_t instance)
{
	POLAR_ASSERT_PANIC(instance != NULL);

	/*
	 * release reference in xlog_queue ringbuf, which is allocated during
	 * recovery
	 */
	polar_xlog_send_queue_release_data_ref();

	/* Clear ringbuf, otherwise left WALs will be inserted into logindex twice */
	polar_ringbuf_reset(instance->xlog_queue);
	polar_prs_stat_reset(instance->xlog_queue);
}

void
polar_online_promote_data(polar_logindex_redo_ctl_t instance)
{
	XLogRecPtr	last_replayed_lsn = GetXLogReplayRecPtr(NULL);

	elog(LOG, "Begin function %s", PG_FUNCNAME_MACRO);

	/*
	 * POLAR: promote clog/commit_ts/multixact/csn
	 */
	polar_promote_clog();
	polar_promote_commit_ts();

	polar_promote_multixact_offset();
	polar_promote_multixact_member();

	if (POLAR_RSC_REPLICA_ENABLED())
		polar_rsc_promote();

	/* reload persisted slot from shared storage */
	polar_reload_replication_slots_from_shared_storage();

	polar_logindex_promote_xlog_queue(instance);

	polar_logindex_update_promoted_info(instance->wal_logindex_snapshot, last_replayed_lsn);

	if (instance->fullpage_logindex_snapshot)
	{
		char	   *dir = (char *) polar_get_logindex_snapshot_dir(instance->fullpage_logindex_snapshot);

		/*
		 * If you upgrade from an old version that does not support FPSI to a
		 * new version, the polar_fullpage directory does not exist during the
		 * online promote process, so it needs to be created during the online
		 * promote process.
		 */
		polar_validate_dir(dir);
		polar_logindex_update_promoted_info(instance->fullpage_logindex_snapshot, last_replayed_lsn);
	}

	elog(LOG, "End function %s", PG_FUNCNAME_MACRO);
}

void
polar_standby_promote_data(polar_logindex_redo_ctl_t instance)
{
	XLogRecPtr	last_replayed_lsn = GetXLogReplayRecPtr(NULL);

	elog(LOG, "Begin function %s", PG_FUNCNAME_MACRO);

	POLAR_ASSERT_PANIC(instance != NULL);

	if (POLAR_RSC_STANDBY_ENABLED())
		polar_rsc_promote();

	polar_logindex_promote_xlog_queue(instance);

	polar_logindex_update_promoted_info(instance->wal_logindex_snapshot, last_replayed_lsn);

	if (instance->fullpage_logindex_snapshot)
		polar_logindex_update_promoted_info(instance->fullpage_logindex_snapshot, last_replayed_lsn);

	elog(LOG, "End function %s", PG_FUNCNAME_MACRO);
}

void
polar_wait_logindex_bg_stop_replay(polar_logindex_redo_ctl_t instance, Latch *latch)
{
	/* Wait background process handle online promote signal */
	while (polar_get_bg_redo_state(instance) != POLAR_BG_WAITING_RESET)
	{
		HandleStartupProcInterrupts();
		WaitLatch(latch,
				  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  10, WAIT_EVENT_RECOVERY_APPLY_DELAY);

		ResetLatch(latch);
	}

	elog(LOG, "background process handled online promote signal");
}

static void
logindex_replay_specific_pages(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn,
							   log_index_lsn_t_cmp cmp, log_index_lsn_t * target_info)
{
	log_index_lsn_t *replay_info;
	log_index_lsn_iter_t lsn_iter;
	XLogReaderState *state;
	MemoryContext oldcontext;
	XLogRecPtr	end_lsn = InvalidXLogRecPtr;

	POLAR_ASSERT_PANIC(cmp && target_info);
	elog(LOG, "%s start with lsn %X/%X", PG_FUNCNAME_MACRO, LSN_FORMAT_ARGS(start_lsn));
	POLAR_LOG_LOGINDEX_LSN_INFO(target_info);
	oldcontext = MemoryContextSwitchTo(polar_logindex_memory_context());
	state = polar_allocate_xlog_reader();
	lsn_iter = polar_logindex_create_lsn_iterator(instance->wal_logindex_snapshot, start_lsn);

	while ((replay_info = polar_logindex_lsn_iterator_next(instance->wal_logindex_snapshot, lsn_iter)))
	{
		Buffer		buffer;
		buf_replay_stat_t buf_stat = BUF_NEED_REPLAY;

		end_lsn = replay_info->lsn;
		if (cmp(replay_info, target_info) == false)
			continue;
		buffer = polar_xlog_need_replay(instance, replay_info->tag, replay_info->lsn, &buf_stat);
		if (buf_stat == BUF_NEED_REPLAY)
			polar_bg_redo_apply_read_record(instance, state, replay_info->lsn, replay_info->tag, &buffer);
		if (BufferIsValid(buffer))
			ReleaseBuffer(buffer);
	}

	polar_logindex_release_lsn_iterator(lsn_iter);
	XLogReaderFree(state);
	MemoryContextSwitchTo(oldcontext);
	elog(LOG, "%s end with lsn %X/%X", PG_FUNCNAME_MACRO, LSN_FORMAT_ARGS(end_lsn));
}

static bool
dbnode_cmp(log_index_lsn_t * info_a, log_index_lsn_t * info_b)
{
	if (info_a->tag->rnode.dbNode == info_b->tag->rnode.dbNode)
		return true;
	return false;
}

void
polar_logindex_replay_db(polar_logindex_redo_ctl_t instance, Oid dbnode)
{
	BufferTag	target_tag = {0};
	log_index_lsn_t target_info = {0};
	XLogRecPtr	start_lsn = polar_bg_redo_get_replayed_lsn(instance);

	target_tag.rnode.dbNode = dbnode;
	target_info.tag = &target_tag;

	logindex_replay_specific_pages(instance, start_lsn, dbnode_cmp, &target_info);
}

/*
 * POLAR; if we read a future page, then call this function to restore old version page
 */
bool
polar_logindex_restore_fullpage_snapshot_if_needed(polar_logindex_redo_ctl_t instance, BufferTag *tag, Buffer *buffer)
{
	static XLogReaderState *state = NULL;
	log_index_lsn_t *lsn_info = NULL;
	MemoryContext oldcontext = NULL;
	XLogRecPtr	replayed_lsn = InvalidXLogRecPtr;
	XLogRecPtr	end_lsn = PG_INT64_MAX;
	Page		page;
	bool		success = false;
	TimestampTz start_time = GetCurrentTimestamp();

	POLAR_ASSERT_PANIC(BufferIsValid(*buffer));
	page = BufferGetPage(*buffer);

retry:

	/*
	 * POLAR: If page lsn is larger than replayed xlog lsn, then this replica
	 * read a future page
	 */
	if (!polar_is_future_page(GetBufferDescriptor(*buffer - 1)))
		return false;

	pgstat_report_wait_start(WAIT_EVENT_LOGINDEX_WAIT_FULLPAGE);

	replayed_lsn = GetXLogReplayRecPtr(&ThisTimeLineID);

	POLAR_ASSERT_PANIC(fullpage_page_iter == NULL);

	/*
	 * Logindex record the start position of XLOG and we search LSN between
	 * [replayed_lsn, end_lsn]. And end_lsn points to the end position of the
	 * last xlog, so we should subtract 1 here .
	 */
	fullpage_page_iter = polar_logindex_create_page_iterator(instance->fullpage_logindex_snapshot, tag,
															 replayed_lsn, end_lsn - 1, polar_get_bg_redo_state(instance) == POLAR_BG_ONLINE_PROMOTE);

	if (polar_logindex_page_iterator_state(fullpage_page_iter) != ITERATE_STATE_FINISHED)
	{
		elog(PANIC, "Failed to iterate data for " POLAR_LOG_BUFFER_TAG_FORMAT ", which replayed_lsn=%X/%X and end_lsn=%X/%X",
			 POLAR_LOG_BUFFER_TAG(tag),
			 LSN_FORMAT_ARGS(replayed_lsn),
			 LSN_FORMAT_ARGS(end_lsn));
	}

	oldcontext = MemoryContextSwitchTo(polar_logindex_memory_context());

	if (state == NULL)
	{
		state = XLogReaderAllocate(wal_segment_size, NULL,
								   XL_ROUTINE(.page_read = &read_local_xlog_page,
											  .segment_open = &wal_segment_open,
											  .segment_close = &wal_segment_close),
								   NULL);

		if (!state)
		{
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed while allocating a WAL reading processor.")));
		}
	}

	lsn_info = polar_logindex_page_iterator_next(fullpage_page_iter);

	if (lsn_info != NULL)
	{
		XLogRecPtr	lsn = lsn_info->lsn;

		polar_logindex_read_xlog(state, lsn);
		polar_logindex_apply_one_record(instance, state, tag, buffer);
		success = true;
	}

	MemoryContextSwitchTo(oldcontext);

	polar_logindex_release_page_iterator(fullpage_page_iter);
	fullpage_page_iter = NULL;

	if (!success)
	{
		CHECK_FOR_INTERRUPTS();

		/* Force FATAL future page if we wait fullpage logindex too long */
		if (TimestampDifferenceExceeds(start_time, GetCurrentTimestamp(),
									   polar_wait_old_version_page_timeout))
			elog(FATAL, "Read a future page due to timeout, page lsn = %lX, replayed_lsn = %lX, page_tag=" POLAR_LOG_BUFFER_TAG_FORMAT,
				 PageGetLSN(page), replayed_lsn, POLAR_LOG_BUFFER_TAG(tag));

		pg_usleep(100);			/* 0.1ms */
		pgstat_report_wait_end();
		goto retry;
	}

	pgstat_report_wait_end();

	elog(polar_trace_logindex(DEBUG3), "%s restore page " POLAR_LOG_BUFFER_TAG_FORMAT " LSN=%X/%X", PG_FUNCNAME_MACRO,
		 POLAR_LOG_BUFFER_TAG(tag), LSN_FORMAT_ARGS(PageGetLSN(page)));

	return true;
}
