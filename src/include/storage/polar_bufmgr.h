/*-------------------------------------------------------------------------
 *
 * polar_bufmgr.h
 *	  PolarDB buffer manager definitions.
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
 *	  src/include/storage/polar_bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_BUFMGR_H
#define POLAR_BUFMGR_H

#include "storage/buf.h"
#include "storage/bufpage.h"

typedef struct BufferDesc BufferDesc;
typedef struct WritebackContext WritebackContext;

#define polar_buffer_get_oldest_lsn(buf_hdr) \
	(pg_atomic_read_u64((pg_atomic_uint64 *) &buf_hdr->oldest_lsn))

#define polar_incremental_checkpoint_is_allowed() \
	(polar_flush_list_enabled() && polar_enable_incremental_checkpoint && (!fullPageWrites))

#define polar_incremental_checkpoint_enabled() \
	(polar_incremental_checkpoint_is_allowed() && (!RecoveryInProgress()))

#define polar_inc_end_of_recovery_checkpoint_enabled() \
	(polar_incremental_checkpoint_is_allowed() && polar_enable_inc_end_of_recovery_checkpoint && polar_is_primary())

#define LRU_BUFFER_BEHIND		(-1)

extern void polar_redo_set_buffer_oldest_lsn(Buffer buffer, XLogRecPtr lsn);
extern void polar_set_buffer_fake_oldest_lsn(BufferDesc *buf_hdr);
extern void polar_reset_buffer_oldest_lsn(BufferDesc *buf_hdr);
extern bool polar_buffer_can_be_flushed(BufferDesc *buf_hdr, XLogRecPtr oldest_apply_lsn, bool use_cbuf);
extern bool polar_buffer_can_be_flushed_by_checkpoint(BufferDesc *buf_hdr, XLogRecPtr oldest_apply_lsn, int flags);
extern bool polar_bg_buffer_sync(WritebackContext *wb_context, int flags);
extern bool polar_buffer_sync(WritebackContext *wb_context, XLogRecPtr *consistent_lsn, bool is_normal_bgwriter, int flags);
extern void polar_set_reg_buffer_oldest_lsn(Buffer buffer);
extern bool polar_check_incremental_checkpoint(bool shutdown, int *flags, XLogRecPtr *inc_redo);
extern XLogRecPtr polar_cal_cur_consistent_lsn(void);
extern void polar_lru_sync_buffer(WritebackContext *wb_context, int flags);
extern int	polar_calculate_lru_lap(void);
extern XLogRecPtr polar_calculate_consistent_lsn_lag(void);
extern void polar_adjust_parallel_bgwriters(uint64 consistent_lag, int lru_ahead_lap);
extern bool polar_pin_buffer(BufferDesc *buf, BufferAccessStrategy strategy);
extern Buffer polar_bulk_read_buffer(Relation reln, BlockNumber blockNum, BlockNumber maxBlockCount);
extern Buffer polar_bulk_read_buffer_extended(Relation reln, ForkNumber forkNum, BlockNumber blockNum,
											  ReadBufferMode mode, BufferAccessStrategy strategy,
											  BlockNumber maxBlockCount);
extern bool polar_is_future_page(BufferDesc *buf_hdr);
extern bool polar_buffer_need_fullpage_snapshot(BufferDesc *buf_hdr, XLogRecPtr oldest_apply_lsn);
#endif							/* POLAR_BUFMGR_H */
