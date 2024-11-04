/*-------------------------------------------------------------------------
 *
 * polar_bufmgr.c
 *	  PolarDB buffer manager interface routines
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
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
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/polar_bufmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "access/xlogrecovery.h"
#include "access/xlog.h"
#include "postmaster/polar_parallel_bgwriter.h"
#include "storage/polar_bufmgr.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_fd.h"
#include "storage/polar_flush.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/polar_log.h"
#include "utils/timestamp.h"

#define BUF_WRITTEN             0x01
#define STOP_PARALLEL_BGWRITER_DELAY_FACTOR 10

#define polar_buffer_first_touch_after_copy(buf_hdr) \
	(buf_hdr->copy_buffer && !(buf_hdr->polar_flags & POLAR_BUF_FIRST_TOUCHED_AFTER_COPY))

extern void ResourceOwnerEnlargeBuffers(ResourceOwner owner);

static int	prev_sync_count = 0;
static int64 consistent_lsn_delta = 0;

static void polar_sync_buffer_from_copy_buffer(WritebackContext *wb_context, int flags);
static int	evaluate_sync_buffer_num(uint64 lag, int bgwriter_flush_batch_size);
static XLogRecPtr update_consistent_lsn_delta(XLogRecPtr cur_consistent_lsn);
static uint64 polar_consistent_lsn_lag(XLogRecPtr cur_consistent_lsn);
static int	polar_get_lru_batch(int *next_flush_buf_id);

/* POLAR: bulk io */
static Buffer polar_bulk_read_buffer_common(Relation reln, char relpersistence, ForkNumber forkNum,
											BlockNumber firstBlockNum, ReadBufferMode mode,
											BufferAccessStrategy strategy, bool *hit,
											BlockNumber maxBlockCount);

/* POLAR end */

/* Reset oldest lsn to invalid and remove it from flush list. */
void
polar_reset_buffer_oldest_lsn(BufferDesc *buf_hdr)
{
	XLogRecPtr	oldest_lsn = polar_buffer_get_oldest_lsn(buf_hdr);
	XLogRecPtr	consistent_lsn = polar_get_consistent_lsn();

	if (XLogRecPtrIsInvalid(oldest_lsn))
	{
		Assert(buf_hdr->recently_modified_count == 0);

		/*
		 * In primary, we should check all flush related flag clear, so we
		 * assert each flags.
		 */
		Assert((buf_hdr->polar_flags & POLAR_BUF_OLDEST_LSN_IS_FAKE) == 0);
		Assert((buf_hdr->polar_flags & POLAR_BUF_FIRST_TOUCHED_AFTER_COPY) == 0);
	}
	else
	{
		/* This should not happen */
		if (!XLogRecPtrIsInvalid(consistent_lsn) && oldest_lsn < consistent_lsn)
		{
			elog(WARNING, "Buffer ([%u,%u,%u], %u, %d) oldest lsn %X/%X is less than consistent lsn %X/%X",
				 buf_hdr->tag.rnode.spcNode,
				 buf_hdr->tag.rnode.dbNode,
				 buf_hdr->tag.rnode.relNode,
				 buf_hdr->tag.blockNum,
				 buf_hdr->tag.forkNum,
				 LSN_FORMAT_ARGS(oldest_lsn),
				 LSN_FORMAT_ARGS(consistent_lsn));
		}

		buf_hdr->recently_modified_count = 0;
		buf_hdr->polar_flags = 0;

		polar_remove_buffer_from_flush_list(buf_hdr);
	}
}

/*
 * polar_bg_buffer_sync - Write out some dirty buffers in the flush list.
 *
 * This is called periodically by the background writer process.
 *
 * Returns true if it's appropriate for the bgwriter process to go into
 * low-power hibernation mode.
 */
bool
polar_bg_buffer_sync(WritebackContext *wb_context, int flags)
{
	bool		res = false;
	XLogRecPtr	consistent_lsn = InvalidXLogRecPtr;

	/* Use normal BgBufferSync */
	if (polar_normal_buffer_sync_enabled())
		return BgBufferSync(wb_context, flags);

	/*
	 * Now, flush list is enabled, for PolarDB with shared storage, it should
	 * enable the flush list, otherwise, the consistent lsn will be invalid.
	 */
	if (polar_enable_normal_bgwriter)
		res = BgBufferSync(wb_context, flags);
	res = polar_buffer_sync(wb_context, &consistent_lsn, true, flags) || res;
	polar_set_consistent_lsn(consistent_lsn);

	return res;
}

/*
 * Calculate current consistent lsn, it is the minimum between first buffer of
 * flush list and all copy buffers. If oldest lsn of all buffers are invalid,
 * we use polar_max_valid_lsn(), which is: 1) current insert RecPtr when node is
 * not in recovery, because some commands generate wal records but do not set
 * any buffers oldest lsn. 2) current start RecPtr of the record being replayed
 * when node is in recovery, it should be noted that lastReplayedEndRecPtr is not
 * used here, because there may be a dirty buffer whose oldest_lsn is
 * polar_replay_read_recptr and has been flushed to the disk, thus updating the
 * consistent_lsn as polar_replay_read_recptr, use lastReplayedEndRecPtr here may
 * lead to the fallback of calculated consistent_lsn.
 */
XLogRecPtr
polar_cal_cur_consistent_lsn(void)
{
	BufferDesc *buf;
	XLogRecPtr	clsn;
	XLogRecPtr	lsn;

	Assert(polar_flush_list_enabled());

	SpinLockAcquire(&polar_flush_ctl->flushlist_lock);
	if (polar_flush_list_is_empty())
	{
		if (unlikely(polar_bg_redo_state_is_parallel(polar_logindex_redo_instance)))
			lsn = polar_logindex_replayed_oldest_lsn();
		else if (unlikely(polar_should_launch_standby_instant_recovery()))
			lsn = polar_bg_redo_get_replayed_lsn(polar_logindex_redo_instance);
		else
			lsn = polar_max_valid_lsn();
		SpinLockRelease(&polar_flush_ctl->flushlist_lock);

		if (unlikely(polar_enable_debug))
			elog(DEBUG1,
				 "The flush list is empty, so use current insert lsn %X/%X as consistent lsn.",
				 LSN_FORMAT_ARGS(lsn));

		return lsn;
	}

	Assert(polar_flush_ctl->first_flush_buffer >= 0);

	buf = GetBufferDescriptor(polar_flush_ctl->first_flush_buffer);
	lsn = pg_atomic_read_u64((pg_atomic_uint64 *) &buf->oldest_lsn);

	SpinLockRelease(&polar_flush_ctl->flushlist_lock);

	clsn = polar_copy_buffers_get_oldest_lsn();
	if (!XLogRecPtrIsInvalid(clsn))
		lsn = Min(lsn, clsn);

	return lsn;
}

bool
polar_buffer_can_be_flushed(BufferDesc *buf_hdr,
							XLogRecPtr oldest_apply_lsn,
							bool use_cbuf)
{
	uint32		buf_state;
	bool		res = false;
	XLogRecPtr	latest_lsn = BufferGetLSN(buf_hdr);
	CopyBufferDesc *cbuf = NULL;

#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("test_ignore_ro_latency") == FaultInjectorTypeEnable)
		return true;
#endif

	if (!polar_enable_shared_storage_mode ||
		XLogRecPtrIsInvalid(oldest_apply_lsn) ||
		(latest_lsn <= oldest_apply_lsn) ||
		(buf_hdr->tag.forkNum == FSM_FORKNUM) ||
		(buf_hdr->tag.forkNum == INIT_FORKNUM) ||
		(buf_hdr->tag.forkNum == VISIBILITYMAP_FORKNUM &&
		 !polar_enable_control_vm_flush) ||
		polar_ignore_ro_latency)
		return true;
	else if (use_cbuf)
	{
		buf_state = LockBufHdr(buf_hdr);
		cbuf = buf_hdr->copy_buffer;

		if (cbuf)
			res = polar_copy_buffer_get_lsn(cbuf) <= oldest_apply_lsn;

		UnlockBufHdr(buf_hdr, buf_state);

		if (cbuf && !res)
			pg_atomic_add_fetch_u32(&cbuf->pass_count, 1);
	}

	return res;
}

/*
 * For incremental checkpoint, we only flush buffers that background writer does
 * not flush; for normal checkpoint, we check whether the buffer can be flushed,
 * buffers that can be flushed will be flushed by checkpoint.
 */
bool
polar_buffer_can_be_flushed_by_checkpoint(BufferDesc *buf_hdr,
										  XLogRecPtr oldest_apply_lsn,
										  int flags)
{
	if (flags & CHECKPOINT_INCREMENTAL)
	{
		if (buf_hdr->tag.forkNum == FSM_FORKNUM ||
			buf_hdr->tag.forkNum == INIT_FORKNUM)
			return true;
		else
			return false;
	}
	else
		return polar_buffer_can_be_flushed(buf_hdr, oldest_apply_lsn, false);
}

static int
polar_get_lru_batch(int *next_flush_buf_id)
{
	int			strategy_buf_id;
	uint32		next_to_passes;
	uint32		strategy_passes;
	int			lru_buf_id;
	uint32		lru_strategy_passes;
	int			bufs_to_lap;

	strategy_buf_id = StrategySyncStart(&strategy_passes, NULL);

	SpinLockAcquire(&polar_flush_ctl->lru_lock);
	lru_buf_id = polar_flush_ctl->lru_buffer_id;
	lru_strategy_passes = polar_flush_ctl->lru_complete_passes;

	if ((int32) (lru_strategy_passes - strategy_passes) > 1)
	{
		SpinLockRelease(&polar_flush_ctl->lru_lock);
		return 0;
	}

	if (lru_strategy_passes > strategy_passes)
	{
		/* we're one pass ahead of the strategy point */
		bufs_to_lap = strategy_buf_id - lru_buf_id;
		*next_flush_buf_id = lru_buf_id;
		next_to_passes = lru_strategy_passes;
	}
	else if (strategy_passes == lru_strategy_passes &&
			 lru_buf_id > strategy_buf_id)
	{
		/* on same pass, but ahead or at least not behind */
		bufs_to_lap = NBuffers - (lru_buf_id - strategy_buf_id);
		*next_flush_buf_id = lru_buf_id;
		next_to_passes = lru_strategy_passes;
	}
	else
	{
		/*
		 * We're behind.
		 */
		*next_flush_buf_id = strategy_buf_id;
		next_to_passes = strategy_passes;
		bufs_to_lap = NBuffers;
	}

	bufs_to_lap = bufs_to_lap > polar_lru_batch_pages ? polar_lru_batch_pages : bufs_to_lap;

	if (bufs_to_lap <= 0)
	{
		SpinLockRelease(&polar_flush_ctl->lru_lock);
		return 0;
	}

	/*
	 * Current lru writer needs to scan bufs_to_lap of pages. Meanwhile,
	 * update positions of buffer id and passes for other lru writers.
	 */
	polar_flush_ctl->lru_buffer_id = *next_flush_buf_id + bufs_to_lap;
	polar_flush_ctl->lru_complete_passes = next_to_passes;

	if (polar_flush_ctl->lru_buffer_id >= NBuffers)
	{
		polar_flush_ctl->lru_buffer_id = polar_flush_ctl->lru_buffer_id % NBuffers;
		polar_flush_ctl->lru_complete_passes += 1;
	}
	SpinLockRelease(&polar_flush_ctl->lru_lock);

	return bufs_to_lap;
}

void
polar_lru_sync_buffer(WritebackContext *wb_context, int flags)
{
	int			sync_count = 0;
	int			num_written = 0;
	int			sync_state;
	int			strategy_buf_id;
	uint32		strategy_passes;
	int			shared_nbuffers = NBuffers;

	/* Make sure we can handle the pin inside SyncOneBuffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	while (sync_count < polar_lru_bgwriter_max_pages)
	{
		int			num;
		int			next_flush_buf_id;
		int			i = 0;

		/* Get buffers from lru */
		num = polar_get_lru_batch(&next_flush_buf_id);

		if (num == 0)
			break;

		while (i < num)
		{
			if (next_flush_buf_id >= shared_nbuffers)
			{
				strategy_buf_id = StrategySyncStart(&strategy_passes, NULL);

				SpinLockAcquire(&polar_flush_ctl->lru_lock);
				polar_flush_ctl->lru_buffer_id = strategy_buf_id;
				polar_flush_ctl->lru_complete_passes = strategy_passes;
				SpinLockRelease(&polar_flush_ctl->lru_lock);

				sync_count = polar_lru_bgwriter_max_pages;
				break;
			}

			sync_state = SyncOneBuffer(next_flush_buf_id, true, wb_context, flags);

			if (++next_flush_buf_id >= NBuffers)
				next_flush_buf_id = 0;

			if (sync_state & BUF_WRITTEN)
				++num_written;

			i++;
		}

		sync_count += num;
	}

	pg_atomic_fetch_add_u64(&polar_flush_ctl->flush_buffer_io.bgwriter_flush, num_written);
	PendingBgWriterStats.buf_written_clean += num_written;
}

int
polar_calculate_lru_lap(void)
{
	int			strategy_buf_id;
	uint32		strategy_passes;
	int			lru_buf_id;
	uint32		lru_passes;
	int			ahead_lap;

	strategy_buf_id = StrategySyncStart(&strategy_passes, NULL);

	SpinLockAcquire(&polar_flush_ctl->lru_lock);
	lru_buf_id = polar_flush_ctl->lru_buffer_id;
	lru_passes = polar_flush_ctl->lru_complete_passes;
	SpinLockRelease(&polar_flush_ctl->lru_lock);

	/* only use freelist */
	if (strategy_buf_id == 0 && strategy_passes == 0)
	{
		ahead_lap = NBuffers;

		if (unlikely(polar_enable_lru_log))
			elog(DEBUG1, "LRUsync No Buffer Alloc, Use Freelist");

		return ahead_lap;
	}

	if ((int32) (lru_passes - strategy_passes) > 1)
	{
		ahead_lap = NBuffers;
		return ahead_lap;
	}

	if (lru_passes > strategy_passes)
	{
		/* we're one pass ahead of the strategy point */
		ahead_lap = NBuffers - (strategy_buf_id - lru_buf_id);

		if (unlikely(polar_enable_lru_log))
			elog(DEBUG1, "LRUsync ahead: lru %u-%u strategy %u-%u ahead lap=%d",
				 lru_passes, lru_buf_id,
				 strategy_passes, strategy_buf_id,
				 ahead_lap);
	}
	else if (strategy_passes == lru_passes &&
			 lru_buf_id > strategy_buf_id)
	{
		/* on same pass, but ahead or at least not behind */
		ahead_lap = lru_buf_id - strategy_buf_id;

		if (unlikely(polar_enable_lru_log))
			elog(DEBUG1, "LRUsync ahead: lru %u-%u strategy %u-%u ahead lap=%d",
				 lru_passes, lru_buf_id,
				 strategy_passes, strategy_buf_id,
				 ahead_lap);
	}
	else
	{
		if (unlikely(polar_enable_lru_log))
			elog(DEBUG1, "LRUsync behind: lru %u-%u strategy %u-%u",
				 lru_passes, lru_buf_id,
				 strategy_passes, strategy_buf_id);

		/*
		 * We're behind.
		 */
		ahead_lap = LRU_BUFFER_BEHIND;
	}

	return ahead_lap;
}

static XLogRecPtr
polar_consistent_lsn_lag(XLogRecPtr cur_consistent_lsn)
{
	XLogRecPtr	oldest_apply_lsn;
	XLogRecPtr	lag;

	oldest_apply_lsn = polar_get_oldest_apply_lsn();

	if (XLogRecPtrIsInvalid(oldest_apply_lsn) || polar_ignore_ro_latency)
		lag = polar_max_valid_lsn() - cur_consistent_lsn;

	/*
	 * POLAR: when enable fullpage snapshot, we don't care about
	 * oledest_apply_lsn anymore
	 */
	else if (POLAR_LOGINDEX_ENABLE_FULLPAGE())
	{
		XLogRecPtr	consistent_lsn_lag = polar_max_valid_lsn() - cur_consistent_lsn;

		/* If consistent_lsn is too old, start to trigger to write fullpage */
		if (consistent_lsn_lag > polar_fullpage_snapshot_oldest_lsn_delay_threshold)
			lag = consistent_lsn_lag - polar_fullpage_snapshot_oldest_lsn_delay_threshold;
		else
			lag = 0;
	}
	else
		lag = oldest_apply_lsn > cur_consistent_lsn ? oldest_apply_lsn - cur_consistent_lsn : 0;

	return lag;
}

XLogRecPtr
polar_calculate_consistent_lsn_lag(void)
{
	XLogRecPtr	cur_consistent_lsn = InvalidXLogRecPtr;
	XLogRecPtr	update_consistent_lsn = InvalidXLogRecPtr;
	XLogRecPtr	lag;

	cur_consistent_lsn = polar_get_consistent_lsn();
	update_consistent_lsn = update_consistent_lsn_delta(cur_consistent_lsn);

	lag = polar_consistent_lsn_lag(update_consistent_lsn);

	/* update lsn */
	polar_set_consistent_lsn(update_consistent_lsn);

	return lag;
}

/*
 * polar_buffer_sync - Sync buffers that are getting from flush list.
 *
 * Returns true if it's appropriate for the bgwriter process to go into
 * low-power hibernation mode.
 */
bool
polar_buffer_sync(WritebackContext *wb_context,
				  XLogRecPtr *consistent_lsn,
				  bool is_normal_bgwriter,
				  int flags)
{
	static int	batch_buf_size = 0;
	static int *batch_buf = NULL;
	XLogRecPtr	cur_consistent_lsn;
	int			num_written = 0;
	int			num_to_sync;
	int			sync_count = 0;
	uint64		sleep_lag;
	uint64		lag;

	if (batch_buf_size != polar_bgwriter_flush_batch_size)
	{
		if (batch_buf)
			free(batch_buf);

		batch_buf = (int *) malloc(polar_bgwriter_flush_batch_size * sizeof(int));
		batch_buf_size = polar_bgwriter_flush_batch_size;
	}

	cur_consistent_lsn = polar_get_consistent_lsn();
	sleep_lag = polar_bgwriter_sleep_lsn_lag * 1024 * 1024L;
	lag = polar_consistent_lsn_lag(cur_consistent_lsn);

#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("test_ignore_ro_latency") == FaultInjectorTypeEnable)
		lag = polar_max_valid_lsn() - cur_consistent_lsn;
#endif

	if (!polar_enable_flush_dispatcher && is_normal_bgwriter)
		polar_adjust_parallel_bgwriters(lag, 0);

	/* Evaluate the number of buffers to sync. */
	num_to_sync = evaluate_sync_buffer_num(lag, batch_buf_size);

	/*
	 * Only parallel bgwriter who acuqire lock, can sync buffers from the copy
	 * buffer.
	 */
	if (polar_enable_flush_dispatcher)
	{
		if (LWLockConditionalAcquire(&polar_flush_ctl->cbuflock, LW_EXCLUSIVE))
		{
			polar_sync_buffer_from_copy_buffer(wb_context, flags);
			LWLockRelease(&polar_flush_ctl->cbuflock);
		}
	}
	else if (is_normal_bgwriter)
		polar_sync_buffer_from_copy_buffer(wb_context, flags);

	if (unlikely(polar_enable_debug))
		elog(DEBUG1, "Try to get %d buffers to flush from flush list", num_to_sync);

	/* Make sure we can handle the pin inside SyncOneBuffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	while (sync_count < num_to_sync)
	{
		int			num;
		int			i = 0;

		/* Get buffers from flush list */
		num = polar_get_batch_buffer(batch_buf, batch_buf_size);
		if (num == 0 || batch_buf == NULL)
			break;

		/* Sync buffers */
		while (i < num)
		{
			BufferDesc *bufHdr = GetBufferDescriptor(batch_buf[i]);
			int			sync_state;
			uint32		buf_state;

			buf_state = LockBufHdr(bufHdr);
			if (buf_state & BM_IO_IN_PROGRESS)
			{
				UnlockBufHdr(bufHdr, buf_state);
				i++;
				continue;
			}
			UnlockBufHdr(bufHdr, buf_state);

			sync_state = SyncOneBuffer(batch_buf[i], false, wb_context, flags);

			i++;
			if (sync_state & BUF_WRITTEN)
				num_written++;
		}

		sync_count += num;
	}

	pg_atomic_fetch_add_u64(&polar_flush_ctl->flush_buffer_io.bgwriter_flush, num_written);

	PendingBgWriterStats.buf_written_clean += num_written;

	/*
	 * Reset StrategyControl->numBufferAllocs and report buffer alloc counts
	 * to pgstat.
	 *
	 * If BgBufferSync() is enabled, we can't do it. We should let
	 * BgBufferSync() handle it. Because BgBufferSync() use
	 * StrategyControl->numBufferAllocs as an important parameter to calculate
	 * the counter of pages to be scanned/flushed.
	 */
	if (!polar_enable_normal_bgwriter)
	{
		uint32		recent_alloc = 0;

		StrategySyncStart(NULL, &recent_alloc);
		PendingBgWriterStats.buf_alloc += recent_alloc;
	}

	*consistent_lsn = update_consistent_lsn_delta(cur_consistent_lsn);
	return lag < sleep_lag;
}

static XLogRecPtr
update_consistent_lsn_delta(XLogRecPtr cur_consistent_lsn)
{
	XLogRecPtr	next_consistent_lsn;

	if (polar_logindex_redo_instance)
	{
		if (unlikely(polar_get_bg_redo_state(polar_logindex_redo_instance) == POLAR_BG_WAITING_RESET))
		{
			/*
			 * During online promote when node is in primary, the logindex
			 * background state could be POLAR_BG_WAITING_RESET or
			 * POLAR_BG_ONLINE_PROMOTE. If logindex background state is
			 * POLAR_BG_WAITING_RESET then we can not accept connection from
			 * the client and the dirty buffer flush list is empty. So we
			 * don't update consistent lsn when the state is
			 * POLAR_BG_WAITING_RESET.
			 */
			return cur_consistent_lsn;
		}
	}

	next_consistent_lsn = polar_cal_cur_consistent_lsn();
	consistent_lsn_delta = next_consistent_lsn - cur_consistent_lsn;

	if (consistent_lsn_delta < 0)
	{
		if (polar_logindex_redo_instance
			&& (XLogRecPtrIsInvalid(polar_logindex_redo_instance->xlog_replay_from)
				|| next_consistent_lsn < polar_logindex_redo_instance->xlog_replay_from))
		{
			/*
			 * BgWriter may start before logindex is initialized, and then it
			 * set consistent lsn as the checkpoint redo position. When
			 * logindex is initialized, it may change to replay from previsiou
			 * lsn, which is smaller than checkpoint redo lsn. Before
			 * xlog_replay_from we only insert record to logindex table and
			 * don't replay page buffer, so there's no dirty buffer in flush
			 * list. But we may set lastReplayedEndRecPtr as next consistent
			 * lsn, which is smaller than previous calculated consistent lsn.
			 * We ignore new calculated value in this case.
			 */
			consistent_lsn_delta = 0;
			return cur_consistent_lsn;
		}

		elog(PANIC,
			 "Current consistent lsn %X/%X is great than next consistent lsn %X/%X",
			 LSN_FORMAT_ARGS(cur_consistent_lsn),
			 LSN_FORMAT_ARGS(next_consistent_lsn));
	}

	return next_consistent_lsn;
}

/* Evaluate the number of buffers to sync. */
static int
evaluate_sync_buffer_num(uint64 lag, int bgwriter_flush_batch_size)
{
	int			num_to_sync;
	double		sync_per_lsn;
	uint64		max_lag;
	double		cons_delta_per_worker;
	int			current_workers = 0;

	if (polar_parallel_bgwriter_enabled())
		current_workers = CURRENT_PARALLEL_WORKERS;

	cons_delta_per_worker = consistent_lsn_delta / (current_workers + 1);
	sync_per_lsn = (double) prev_sync_count / (cons_delta_per_worker + 1);

	/* Avoid overflow */
	max_lag = polar_bgwriter_batch_size / (sync_per_lsn + 1);

	if (lag > max_lag)
		num_to_sync = polar_bgwriter_batch_size;
	else
		num_to_sync = lag * sync_per_lsn;

	Assert(num_to_sync >= 0);

	if (num_to_sync == 0)
		num_to_sync = bgwriter_flush_batch_size / 10 + 1;
	prev_sync_count = num_to_sync;

	return num_to_sync;
}

/* Normal background writer can start or stop some parallel background workers. */
void
polar_adjust_parallel_bgwriters(uint64 consistent_lag, int lru_ahead_lap)
{
	static TimestampTz last_check_start_tz = 0;
	static TimestampTz last_check_stop_tz = 0;

	bool		ok;
	int			at_most_workers;
	int			current_workers;

	if (RecoveryInProgress() || consistent_lag <= 0 ||
		!polar_parallel_bgwriter_enabled() ||
		!polar_enable_dynamic_parallel_bgwriter)
		return;

	Assert(MyBackendType == B_BG_WRITER);

	/*
	 * The consistent lsn does not be updated. It may be unhelpful to add or
	 * stop parallel writers, so we do nothing.
	 */
	if (consistent_lsn_delta == 0)
	{
		if (polar_enable_debug)
		{
			XLogRecPtr	consistent_lsn = polar_get_consistent_lsn();

			elog(LOG,
				 "Consistent lsn does not update, current consistent lsn is %X/%X",
				 LSN_FORMAT_ARGS(consistent_lsn));
		}

		return;
	}

	Assert(consistent_lsn_delta > 0);
	current_workers = CURRENT_PARALLEL_WORKERS;
	at_most_workers = POLAR_MAX_BGWRITER_WORKERS;

	/* Try to start one parallel background writer */
	if (consistent_lag > ((uint64) polar_parallel_new_bgwriter_threshold_lag * 1024 * 1024L) ||
		(polar_lru_works_threshold > 0 && lru_ahead_lap == LRU_BUFFER_BEHIND))
	{
		/* Reset the last check stop timestamp */
		last_check_stop_tz = 0;

		if (current_workers >= at_most_workers)
		{
			if (polar_enable_debug)
				elog(LOG,
					 "Can not start new parallel background workers, current workers: %d, at most workers %d",
					 current_workers, at_most_workers);

			return;
		}

		if (last_check_start_tz == 0)
		{
			last_check_start_tz = GetCurrentTimestamp();
			return;
		}

		ok = TimestampDifferenceExceeds(last_check_start_tz,
										GetCurrentTimestamp(),
										polar_parallel_new_bgwriter_threshold_time * 1000L);

		/* Once start one */
		if (ok)
		{
			if (polar_new_parallel_bgwriter_useful())
				polar_register_parallel_bgwriter_workers(1);
			last_check_start_tz = 0;
		}
	}

	/* Try to stop one parallel background writer */
	else if (current_workers > polar_parallel_flush_workers)
	{
		/* Reset the last check start timestamp */
		last_check_start_tz = 0;

		if (last_check_stop_tz == 0)
		{
			last_check_stop_tz = GetCurrentTimestamp();
			return;
		}

		/* Stop a background writer slowly, so wait more time. */
		ok = TimestampDifferenceExceeds(last_check_stop_tz,
										GetCurrentTimestamp(),
										polar_parallel_new_bgwriter_threshold_time *
										1000L * STOP_PARALLEL_BGWRITER_DELAY_FACTOR);

		/* Once stop one */
		if (ok)
		{
			polar_shutdown_parallel_bgwriter_workers(1);
			last_check_stop_tz = 0;
		}
	}
}

/*
 * polar_redo_set_buffer_oldest_lsn - Set the buffer oldest lsn when redo.
 *
 * When redo, we have a valid record, so we set buffer oldest lsn to
 * record->ReadRecPtr. Do not use record->EndRecPtr, because oldest lsn
 * is always the start of wal not the end.
 */
void
polar_redo_set_buffer_oldest_lsn(Buffer buffer, XLogRecPtr lsn)
{
	BufferDesc *buf_hdr;
	XLogRecPtr	oldest_lsn;

	if (!polar_flush_list_enabled())
		return;

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	buf_hdr = GetBufferDescriptor(buffer - 1);

	if (XLogRecPtrIsInvalid(lsn))
	{
		POLAR_LOG_BUFFER_DESC(buf_hdr);
		elog(PANIC, "Set invalid oldest lsn.");
	}

	Assert(RecoveryInProgress());
	Assert(BufferIsPinned(buffer));

	/*
	 * POLAR: We will mark buffer dirty after reading buffer from storage and
	 * replaying xlog during startup process does not do the real relaying
	 * work. In some ReadBufferModes, there is no need to hold buffer's
	 * content lock, but BM_IO_IN_PROGRESS must be set.
	 */
	Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(buf_hdr), LW_EXCLUSIVE) ||
		   (pg_atomic_read_u32(&buf_hdr->state) & BM_IO_IN_PROGRESS));

	oldest_lsn = polar_buffer_get_oldest_lsn(buf_hdr);

	/*
	 * For standby or primary in recovery, it always calls this function to
	 * set a valid oldest lsn.
	 */
	if (XLogRecPtrIsInvalid(oldest_lsn))
		polar_put_buffer_to_flush_list(buf_hdr, lsn);

	buf_hdr->recently_modified_count++;
}

/*
 * polar_set_buffer_fake_oldest_lsn - Set a fake oldest_lsn for the buffer.
 *
 * Since generating lsn and inserting buffer to flush list is not atomic, when
 * call MarkBufferDirty, we set a fake lsn for buffer and put it into flush list.
 * The fake lsn is from GetXLogInsertRecPtr, it is incremental.
 */
void
polar_set_buffer_fake_oldest_lsn(BufferDesc *buf_hdr)
{
	XLogRecPtr	oldest_lsn;
	bool		set = false;

	/*
	 * If server is in recovery, the buffer should be set a real lsn, so do
	 * not need to set a fake.
	 */
	if (!polar_flush_list_enabled() ||
		(RecoveryInProgress() &&
		 !polar_bg_redo_state_is_parallel(polar_logindex_redo_instance)))
		return;

	Assert(BufferIsPinned(BufferDescriptorGetBuffer(buf_hdr)));

	/*
	 * POLAR: We will mark buffer dirty after reading buffer from storage and
	 * replaying xlog because startup process does not do the real relaying
	 * work. In some ReadBufferModes, there is no need to hold buffer's
	 * content lock, but BM_IO_IN_PROGRESS must be set.
	 */
	Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(buf_hdr), LW_EXCLUSIVE) ||
		   (pg_atomic_read_u32(&buf_hdr->state) & BM_IO_IN_PROGRESS));

	oldest_lsn = polar_buffer_get_oldest_lsn(buf_hdr);

	/*
	 * If oldest lsn is invalid, set a fake one and put buffer into flush
	 * list. If already has a valid oldest lsn, and it is first touched after
	 * copy, we also set a new fake oldest lsn.
	 */
	if (XLogRecPtrIsInvalid(oldest_lsn))
	{
		polar_put_buffer_to_flush_list(buf_hdr, InvalidXLogRecPtr);
		buf_hdr->polar_flags |= POLAR_BUF_OLDEST_LSN_IS_FAKE;
		set = true;
	}
	else if (polar_buffer_first_touch_after_copy(buf_hdr))
	{
		/* Adjust its position in the flush list */
		polar_adjust_position_in_flush_list(buf_hdr);

		/* First touch after copy done */
		buf_hdr->polar_flags |= POLAR_BUF_FIRST_TOUCHED_AFTER_COPY;
		set = true;
	}

	if (polar_enable_debug && set)
		POLAR_LOG_BUFFER_DESC(buf_hdr);

	buf_hdr->recently_modified_count++;
}

static void
polar_sync_buffer_from_copy_buffer(WritebackContext *wb_context, int flags)
{
	int			i;
	CopyBufferDesc *cbuf;
	int32		buf_id;

	if (!polar_copy_buffer_enabled())
		return;

	/* Make sure we can handle the pin inside SyncOneBuffer */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	for (i = 0; i < polar_copy_buffers; i++)
	{
		cbuf = polar_get_copy_buffer_descriptor(i);
		buf_id = pg_atomic_read_u32((pg_atomic_uint32 *) &cbuf->origin_buffer) - 1;

		if (buf_id < 0)
			continue;

		/*
		 * SyncOneBuffer try to flush the original buffer, if it can be
		 * flushed, flush it and free its copy buffer, otherwise, flush its
		 * copy buffer.
		 */
		SyncOneBuffer(buf_id,
					  false,
					  wb_context,
					  flags);
	}
}

/*
 * In most scenarios, we always call MarkBufferDirty first, then call XLogInsert
 * to generate WAL and real lsn, so the buffer's oldest lsn will be set by
 * MarkBufferDirty. But there are special cases that call XLogInsert first
 * to generate WAL, then call MarkBufferDirty. The later MarkBufferDirty will
 * set an oldest lsn that greater than the real lsn, that will cause a wrong
 * consistent lsn.
 *
 * So if we call XLogInsert first, we will call this function to set a fake
 * oldest lsn like MarkBufferDirty. Generally speaking, these cases should
 * be treated as bugs, and we retain this function mainly to provide cover
 * for some unknown scenarios.
 */
void
polar_set_reg_buffer_oldest_lsn(Buffer buffer)
{
	BufferDesc *buf_hdr;
	XLogRecPtr	oldest_lsn;

	if (BufferIsInvalid(buffer))
		return;

	/*
	 * If server is in recovery, the buffer should be set a real lsn, so do
	 * not need to set a fake.
	 */
	if (!polar_flush_list_enabled() ||
		(RecoveryInProgress() &&
		 !polar_bg_redo_state_is_parallel(polar_logindex_redo_instance)))
		return;

	buf_hdr = GetBufferDescriptor(buffer - 1);
	Assert(BufferIsPinned(buffer));

	/*
	 * Not all registered buffers hold the content exclusive lock, like
	 * hashbucketcleanup, if buffer does not hold the lock, we do not set its
	 * oldest lsn.
	 */
	if (!LWLockHeldByMeInMode(BufferDescriptorGetContentLock(buf_hdr),
							  LW_EXCLUSIVE))
		return;

	/*
	 * If buffer oldest lsn is not set or it is first touched after copy, we
	 * believe that the MarkBufferDirty is not called, it's time to set buffer
	 * oldest lsn and mark it dirty.
	 */
	oldest_lsn = polar_buffer_get_oldest_lsn(buf_hdr);

	if (XLogRecPtrIsInvalid(oldest_lsn) ||
		polar_buffer_first_touch_after_copy(buf_hdr))
		MarkBufferDirty(buffer);
}

/*
 * Check whether we can use incremental checkpoint, if yes, set flag and
 * set consistent lsn as incremental checkpoint redo lsn.
 */
bool
polar_check_incremental_checkpoint(bool shutdown, int *flags,
								   XLogRecPtr *inc_redo)
{
	bool		incremental = false;
	XLogRecPtr	consistent_lsn;

	if (!polar_incremental_checkpoint_enabled())
	{
		*flags &= ~CHECKPOINT_INCREMENTAL;
		return false;
	}

	/*
	 * PolarDB can use incremental checkpoint for end of recovery, if we use
	 * incremental checkpoint, the end of recovery checkpoint will be ignored,
	 * we will create a normal incremental checkpoint, we set the flag at
	 * StartupXLOG.
	 */
	if (*flags & CHECKPOINT_INCREMENTAL)
		incremental = true;

	/*
	 * for CHECKPOINT_CAUSE_XLOG or CHECKPOINT_CAUSE_TIME, we always use
	 * incremental checkpoint.
	 */
	else if (!shutdown && (*flags & (CHECKPOINT_CAUSE_TIME | CHECKPOINT_CAUSE_XLOG)))
		incremental = true;

#ifdef FAULT_INJECTOR
	/* force to use lazy_checkpoint here. */
	if (SIMPLE_FAULT_INJECTOR("test_use_incremental_checkpoint") == FaultInjectorTypeEnable)
		incremental = true;
#endif

	if (!incremental)
		return false;

	/*
	 * shutdown checkpoint must not be incremental. For incremental
	 * checkpoint, we use the consistent lsn as its redo lsn, so we should
	 * check whether the consistent lsn is valid, if not, reset the lazy flag
	 * and make a normal checkpoint.
	 */
	Assert(!shutdown || !incremental);

	consistent_lsn = polar_get_consistent_lsn();
	/* We have a valid consistent lsn, use it */
	if (!XLogRecPtrIsInvalid(consistent_lsn))
		*inc_redo = consistent_lsn;
	else
	{
		/*
		 * Try to calculate the consistent lsn by myself, maybe bgwriter is
		 * not started, nobody will update the consistent lsn.
		 */
		consistent_lsn = polar_cal_cur_consistent_lsn();
		if (!XLogRecPtrIsInvalid(consistent_lsn))
		{
			*inc_redo = consistent_lsn;
			polar_set_consistent_lsn(consistent_lsn);
		}
		else
			incremental = false;
	}

	/*
	 * Do not allow incremental checkpoint when *inc_redo is at previous
	 * timeline.
	 */
	if (incremental && !XLogRecPtrIsInvalid(*inc_redo))
	{
		TimeLineID	current_tli = GetWALInsertionTimeLine();
		List	   *expected_tles = readTimeLineHistory(current_tli);
		TimeLineID	inc_redo_tlid = tliOfPointInHistory(*inc_redo, expected_tles);

		if (current_tli != inc_redo_tlid)
		{
			incremental = false;
			elog(LOG, "Do not allow incremental checkpoint whose redo lsn is at different timeline, " \
				 "redo lsn is %X/%X at timeline %u, current timeline is %u",
				 LSN_FORMAT_ARGS(*inc_redo), inc_redo_tlid, current_tli);
		}
		list_free_deep(expected_tles);
	}

	if (incremental)
	{
		*flags |= CHECKPOINT_INCREMENTAL;
		elog(LOG,
			 "Try to create incremental checkpoint, redo lsn is %X/%X",
			 LSN_FORMAT_ARGS(*inc_redo));
	}
	else
		*flags &= ~CHECKPOINT_INCREMENTAL;

	return incremental;
}

bool
polar_pin_buffer(BufferDesc *buf_desc, BufferAccessStrategy strategy)
{
	return PinBuffer(buf_desc, strategy);
}

Buffer
polar_bulk_read_buffer_extended(Relation reln, ForkNumber forkNum, BlockNumber blockNum,
								ReadBufferMode mode, BufferAccessStrategy strategy,
								BlockNumber maxBlockCount)
{
	bool		hit;
	Buffer		buf;

	/* Open it at the smgr level if not already done */
	RelationGetSmgr(reln);

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(reln))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/*
	 * Read the buffer, and update pgstat counters to reflect a cache hit or
	 * miss.
	 */
	pgstat_count_buffer_read(reln);
	buf = polar_bulk_read_buffer_common(reln, reln->rd_rel->relpersistence,
										forkNum, blockNum, mode, strategy, &hit, maxBlockCount);
	if (hit)
		pgstat_count_buffer_hit(reln);
	return buf;
}

/*
 * polar_bulk_read_buffer_common -- common logic for bulk read multi buffer one time.
 *
 * Try to read as many buffers as possible, up to maxBlockCount.
 * If first block hits shared_buffers, return immediately.
 * If first block doesn't hit shared_buffer,
 * read buffers until up to maxBlockCount or
 * up to first block which hit shared_buffer.
 *
 * Returns: the buffer number for the first block blockNum.
 *		The returned buffer has been pinned.
 *		Does not return on error --- elog's instead.
 *
 * *hit is set to true if the first blockNum was satisfied from shared buffer cache.
 *
 * Deadlock avoidance：Only ascending bulk read are allowed
 * to avoid dead lock on io_in_progress_lock.
 */
static Buffer
polar_bulk_read_buffer_common(Relation reln, char relpersistence, ForkNumber forkNum,
							  BlockNumber firstBlockNum, ReadBufferMode mode,
							  BufferAccessStrategy strategy, bool *hit,
							  BlockNumber maxBlockCount)
{
	SMgrRelation smgr = reln->rd_smgr;
	BufferDesc *bufHdr;
	Block		bufBlock;
	bool		found;
	bool		isLocalBuf = SmgrIsTemp(smgr);
	int			actual_bulk_io_count;
	int			index;
	char	   *buf_read;
	BufferDesc *buffers[MAX_BUFFERS_TO_READ_BY];
	bool		checksum_fail[MAX_BUFFERS_TO_READ_BY] = {false};

	/* POLAR: start lsn to do replay */
	XLogRecPtr	checkpoint_redo_lsn = InvalidXLogRecPtr;
	XLogRecPtr	replay_from;
	polar_redo_action redo_action;
	uint32		repeat_read_times = 0;

	polar_pgstat_count_bulk_read_calls(reln);

	maxBlockCount = Min(maxBlockCount, polar_bulk_read_size);

	if (firstBlockNum == P_NEW ||
		maxBlockCount <= 1 ||
		mode != RBM_NORMAL)
	{
		return ReadBuffer_common(smgr, relpersistence, forkNum, firstBlockNum,
								 mode, strategy, hit);
	}

	*hit = false;

	/* Make sure we will have room to remember the buffer pin */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	TRACE_POSTGRESQL_BUFFER_READ_START(forkNum, firstBlockNum,
									   smgr->smgr_rnode.node.spcNode,
									   smgr->smgr_rnode.node.dbNode,
									   smgr->smgr_rnode.node.relNode,
									   smgr->smgr_rnode.backend,
									   false);	/* false == isExtend */

	if (isLocalBuf)
	{
		bufHdr = LocalBufferAlloc(smgr, forkNum, firstBlockNum, &found);
		if (found)
			pgBufferUsage.local_blks_hit++;
		else
			pgBufferUsage.local_blks_read++;
	}
	else
	{
		bufHdr = BufferAlloc(smgr, relpersistence, forkNum, firstBlockNum,
							 strategy, &found);
		if (found)
			pgBufferUsage.shared_blks_hit++;
		else
			pgBufferUsage.shared_blks_read++;
	}
	/* At this point we do NOT hold any locks. */

	/* if it was already in the buffer pool, we're done */
	if (found)
	{
		/* Just need to update stats before we exit */
		*hit = true;
		VacuumPageHit++;

		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageHit;

		TRACE_POSTGRESQL_BUFFER_READ_DONE(forkNum, firstBlockNum,
										  smgr->smgr_rnode.node.spcNode,
										  smgr->smgr_rnode.node.dbNode,
										  smgr->smgr_rnode.node.relNode,
										  smgr->smgr_rnode.backend,
										  false,
										  found);

		return BufferDescriptorGetBuffer(bufHdr);
	}

	/*
	 * if we have gotten to this point, we have allocated a buffer for the
	 * page but its contents are not yet valid.  IO_IN_PROGRESS is set for it,
	 * if it's a shared buffer.
	 *
	 * Note: if smgrextend fails, we will end up with a buffer that is
	 * allocated but not marked BM_VALID.  P_NEW will still select the same
	 * block number (because the relation didn't get any longer on disk) and
	 * so future attempts to extend the relation will find the same buffer (if
	 * it's not been recycled) but come right back here to try smgrextend
	 * again.
	 */
	Assert(!(pg_atomic_read_u32(&bufHdr->state) & BM_VALID));	/* spinlock not needed */

	buffers[0] = bufHdr;

	/*
	 * Make sure than single bulk_read will not read blocks across files.
	 *
	 * (BlockNumber) RELSEG_SIZE - (blockNum % (BlockNumber) RELSEG_SIZE)
	 * always >= 1, and maxBlockCount always >= 1, so finaly maxBlockCount
	 * always >= 1.
	 */
	maxBlockCount = Min(maxBlockCount, (BlockNumber) RELSEG_SIZE - (firstBlockNum % (BlockNumber) RELSEG_SIZE));

	for (index = 1; index < maxBlockCount; index++)
	{
		BlockNumber blockNum = firstBlockNum + index;

		/* Make sure we will have room to remember the buffer pin */
		ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

		/*
		 * lookup the buffer.  IO_IN_PROGRESS is set if the requested block is
		 * not currently in memory.
		 */
		if (isLocalBuf)
			bufHdr = LocalBufferAlloc(smgr, forkNum, blockNum, &found);
		else
			bufHdr = BufferAlloc(smgr, relpersistence, forkNum, blockNum,
								 strategy, &found);

		Assert(bufHdr);

		/*
		 * For extra block, don't update pgBufferUsage.shared_blks_hit or
		 * pgBufferUsage.shared_blks_read, also the blocks are not used now.
		 */
		if (found)
		{
			/*
			 * important: this buffer is the upper boundary, it should be
			 * excluded.
			 */
			ReleaseBuffer(BufferDescriptorGetBuffer(bufHdr));
			break;
		}
		Assert(!(pg_atomic_read_u32(&bufHdr->state) & BM_VALID));	/* spinlock not needed */
		buffers[index] = bufHdr;
	}

	actual_bulk_io_count = index;

	/*
	 * Until now, as to {blockNum + [0, actual_bulk_io_count)} block buffers,
	 * IO_IN_PROGRESS flag is set and io_in_progress_lock is holded.
	 *
	 * Other proc(include backend sql exec、start xlog replay) which read
	 * there buffers, would be blocked on io_in_progress_lock.
	 *
	 * Deadlock avoidance：Only ascending bulk read are allowed to avoid dead
	 * lock on io_in_progress_lock.
	 */

	buf_read = (char *) palloc_aligned(actual_bulk_io_count * BLCKSZ, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO);

repeat_read:

	/*
	 * POLAR: page-replay.
	 *
	 * Get consistent lsn which used by replay.
	 *
	 * Note: All modifications about reply-page must be applied to both
	 * polar_bulk_read_buffer_common() and ReadBuffer_common().
	 */
	redo_action = polar_require_backend_redo(isLocalBuf, mode, forkNum, &replay_from);
	if (redo_action != POLAR_REDO_NO_ACTION)
	{
		checkpoint_redo_lsn = GetRedoRecPtr();
		Assert(!XLogRecPtrIsInvalid(checkpoint_redo_lsn));
	}
	/* POLAR end */

	/*
	 * Read in the page, unless the caller intends to overwrite it and just
	 * wants us to allocate a buffer.
	 */
	{
		instr_time	io_start,
					io_time;

		if (track_io_timing)
			INSTR_TIME_SET_CURRENT(io_start);

		polar_smgrbulkread(smgr, forkNum, firstBlockNum, actual_bulk_io_count, buf_read);

		polar_pgstat_count_bulk_read_calls_IO(reln);
		polar_pgstat_count_bulk_read_blocks_IO(reln, actual_bulk_io_count);

		if (track_io_timing)
		{
			INSTR_TIME_SET_CURRENT(io_time);
			INSTR_TIME_SUBTRACT(io_time, io_start);
			pgstat_count_buffer_read_time(INSTR_TIME_GET_MICROSEC(io_time));
			INSTR_TIME_ADD(pgBufferUsage.blk_read_time, io_time);
		}

		for (index = 0; index < actual_bulk_io_count; index++)
		{
			BlockNumber blockNum = firstBlockNum + index;

			bufBlock = (Block) (buf_read + index * BLCKSZ);
			/* check for garbage data */
			if (!PageIsVerifiedExtended((Page) bufBlock, blockNum,
										PIV_LOG_WARNING | PIV_REPORT_STAT))
			{
				polar_checksum_err_action err_act;

				err_act = polar_handle_read_error_block(bufBlock, smgr, forkNum, blockNum,
														mode, redo_action, &repeat_read_times);

				if (err_act == POLAR_CHECKSUM_ERR_REPEAT_READ)
					goto repeat_read;
				else if (err_act == POLAR_CHECKSUM_ERR_CHECKPOINT_REDO)
				{
					/*
					 * We must set checksum_fail is true. During checksum_fail
					 * is true, we replay buffer from checkpoint.redo.
					 */
					checksum_fail[index] = true;

					ereport(WARNING,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("Replay from lastcheckpoint which is %X/%X",
									LSN_FORMAT_ARGS(checkpoint_redo_lsn))));
				}
			}
		}
	}

	for (index = actual_bulk_io_count - 1; index >= 0; index--)
	{
		BlockNumber blockNum = firstBlockNum + index;

		bufHdr = buffers[index];
		bufBlock = isLocalBuf ? LocalBufHdrGetBlock(bufHdr) : BufHdrGetBlock(bufHdr);

		/* need copy page content from aligned_buf_read to block shared_buffer */
		memcpy((char *) bufBlock, buf_read + index * BLCKSZ, BLCKSZ);

		if (unlikely(polar_trace_logindex_messages <= DEBUG3))
		{
			BufferTag  *tag = &bufHdr->tag;
			Buffer		buf = BufferDescriptorGetBuffer(bufHdr);
			Page		page = BufferGetPage(buf);

			ereport(LOG, (errmsg("read buffer lsn=%lX, redo_act=%d buf=%d, " POLAR_LOG_BUFFER_TAG_FORMAT,
								 PageGetLSN(page),
								 redo_action,
								 buf,
								 POLAR_LOG_BUFFER_TAG(tag)),
						  errhidestmt(true),
						  errhidecontext(true)));
		}

		/*
		 * POLAR: page-replay.
		 *
		 * Apply logs to this old page when read from disk.
		 *
		 * Note: All modifications about replay-page must be applied to both
		 * polar_bulk_read_buffer_common() and ReadBuffer_common().
		 */
		if (redo_action == POLAR_REDO_REPLAY_XLOG)
		{
			XLogRecPtr	final_replay_from = checksum_fail[index] ? checkpoint_redo_lsn : replay_from;

			/*
			 * POLAR: we want to do record replay on page only in non-Startup
			 * process and consistency reached Startup process. This judge is
			 * *ONLY* valid in replica mode, so we set replica check above
			 * with polar_is_replica().
			 */
			polar_apply_io_locked_page(bufHdr, final_replay_from, checkpoint_redo_lsn, smgr, forkNum, blockNum);
		}
		else if (redo_action == POLAR_REDO_MARK_OUTDATE)
		{
			uint32		redo_state = polar_lock_redo_state(bufHdr);

			redo_state |= POLAR_REDO_OUTDATE;
			polar_unlock_redo_state(bufHdr, redo_state);
		}

		/* POLAR end */

		/*
		 * After TerminateBufferIO, polar_bulk_io_in_progress_buf[index] can
		 * not be used any more.
		 */
		/* Set BM_VALID, terminate IO, and wake up any waiters */
		if (isLocalBuf)
		{
			/* Only need to adjust flags */
			uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);

			buf_state |= BM_VALID;
			pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
		}
		else
			TerminateBufferIO(bufHdr, false, BM_VALID);


		/* important: buffers except firstBlockNum should release pin */
		if (index != 0)
			ReleaseBuffer(BufferDescriptorGetBuffer(bufHdr));

		VacuumPageMiss++;
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageMiss;
	}

	/*
	 * POLAR: page-replay.
	 *
	 * Reset consistent lsn which used by replay.
	 *
	 * Note: All modifications about replay-page must be applied to both
	 * polar_bulk_read_buffer_common() and ReadBuffer_common().
	 */
	if (redo_action != POLAR_REDO_NO_ACTION)
		POLAR_RESET_BACKEND_READ_MIN_LSN();

	/* POLAR end */

	TRACE_POSTGRESQL_BUFFER_READ_DONE(forkNum, firstBlockNum,
									  smgr->smgr_rnode.node.spcNode,
									  smgr->smgr_rnode.node.dbNode,
									  smgr->smgr_rnode.node.relNode,
									  smgr->smgr_rnode.backend,
									  false,
									  found);

	pfree(buf_read);

	return BufferDescriptorGetBuffer(buffers[0]);
}
bool
polar_is_future_page(BufferDesc *buf_hdr)
{
	XLogRecPtr	replayed_lsn = polar_get_xlog_replay_recptr_nolock();
	Page		page = BufferGetPage(BufferDescriptorGetBuffer(buf_hdr));

	if (!XLogRecPtrIsInvalid(replayed_lsn) &&
		PageGetLSN(page) > replayed_lsn &&
		buf_hdr->tag.forkNum == MAIN_FORKNUM)
	{
		if (!POLAR_LOGINDEX_ENABLE_FULLPAGE())
			elog(FATAL, "Read a future page, page lsn = %lx, replayed_lsn = %lx, page_tag = '([%u, %u, %u]), %u, %u'",
				 PageGetLSN(page), replayed_lsn, buf_hdr->tag.rnode.spcNode, buf_hdr->tag.rnode.dbNode,
				 buf_hdr->tag.rnode.relNode, buf_hdr->tag.forkNum, buf_hdr->tag.blockNum);
		return true;
	}

	return false;
}

/*
 * POLAR: check buffer need write fullpage snapshot image
 */
bool
polar_buffer_need_fullpage_snapshot(BufferDesc *buf_hdr, XLogRecPtr oldest_apply_lsn)
{
	XLogRecPtr	cur_insert_lsn = InvalidXLogRecPtr;
	XLogRecPtr	buf_oldest_lsn = pg_atomic_read_u64((pg_atomic_uint64 *) &buf_hdr->oldest_lsn);
	XLogRecPtr	page_latest_lsn = BufferGetLSN(buf_hdr);
	CopyBufferDesc *copy_buf = NULL;
	uint32		buf_state = 0;
	int			replay_threshold = polar_fullpage_snapshot_replay_delay_threshold;
	int			oldest_lsn_threshold = polar_fullpage_snapshot_oldest_lsn_delay_threshold;
	int			min_modified_count = polar_fullpage_snapshot_min_modified_count;
	uint64		estimate_total_segment_size = 0;

	if (unlikely(polar_bg_redo_state_is_parallel(polar_logindex_redo_instance)))
		return false;

	/*
	 * 1. if it's not a future page, quick check 2. only support main fork
	 * data page
	 */
	if (PageIsNew(BufHdrGetBlock(buf_hdr)) ||
		page_latest_lsn <= oldest_apply_lsn ||
		buf_hdr->tag.forkNum != MAIN_FORKNUM)
		return false;

	buf_state = LockBufHdr(buf_hdr);
	copy_buf = buf_hdr->copy_buffer;
	if (copy_buf)
	{
		Assert(!XLogRecPtrIsInvalid(buf_oldest_lsn));
		Assert(!XLogRecPtrIsInvalid(polar_copy_buffer_get_lsn(copy_buf)));
		buf_oldest_lsn = Min(pg_atomic_read_u64((pg_atomic_uint64 *) &copy_buf->oldest_lsn), buf_oldest_lsn);
	}
	UnlockBufHdr(buf_hdr, buf_state);

	if (!POLAR_LOGINDEX_ENABLE_FULLPAGE() ||
		RecoveryInProgress() || /* Standby don't support fullpage snapshot */
		XLogRecPtrIsInvalid(oldest_apply_lsn) ||
		XLogRecPtrIsInvalid(buf_oldest_lsn))
		return false;

#define ONE_MB (1024 * 1024L)
	cur_insert_lsn = polar_get_xlog_insert_ptr_nolock();

	/*
	 * In following case togather, we need to write fullpage 1. buf_oldest_lsn
	 * is too old, block to advance consist_lsn 2. replica is too slow or is
	 * hot page 3. fullpage segment size less then
	 * polar_fullpage_max_segment_size
	 */
	if (cur_insert_lsn < buf_oldest_lsn + oldest_lsn_threshold * ONE_MB)
		return false;

	if ((page_latest_lsn < oldest_apply_lsn + replay_threshold * ONE_MB &&
		 buf_hdr->recently_modified_count < min_modified_count))
		return false;

	estimate_total_segment_size =
		polar_get_estimate_fullpage_segment_size(polar_logindex_redo_instance->fullpage_logindex_snapshot);
	if (estimate_total_segment_size > polar_fullpage_max_segment_size)
		return false;

	return true;
}
