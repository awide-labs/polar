/*-------------------------------------------------------------------------
 *
 * polar_logindex_redo.h
 *
 * Copyright (c) 2024, Alibaba Group Holding Limited
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
 *	  src/include/access/polar_logindex_redo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_LOGINDEX_REDO_H
#define POLAR_LOGINDEX_REDO_H

#include "access/polar_fullpage.h"
#include "access/polar_logindex.h"
#include "access/polar_mini_transaction.h"
#include "access/polar_queue_manager.h"
#include "access/polar_rel_size_cache.h"
#include "access/xlogreader.h"
#include "c.h"
#include "catalog/pg_control.h"
#include "storage/buf_internals.h"
#include "storage/polar_procpool.h"

extern PGDLLIMPORT int polar_logindex_mem_size;
extern PGDLLIMPORT bool polar_enable_parallel_replay_standby_mode;
extern PGDLLIMPORT bool polar_enable_replica_prewarm;
extern PGDLLIMPORT int polar_parallel_replay_task_queue_depth;
extern PGDLLIMPORT int polar_parallel_replay_proc_num;
extern PGDLLIMPORT int polar_bg_replay_batch_size;
extern PGDLLIMPORT bool polar_enable_ro_resolve_conflict;
extern PGDLLIMPORT int polar_logindex_bloom_blocks;
extern PGDLLIMPORT int polar_rel_size_cache_blocks;
extern PGDLLIMPORT bool polar_force_change_checkpoint;
extern PGDLLIMPORT int polar_startup_replay_delay_size;
extern PGDLLIMPORT int polar_logindex_replay_delay_threshold;
extern PGDLLIMPORT bool polar_enable_standby_instant_recovery;
extern PGDLLIMPORT bool polar_wake_logindex_saver;
extern PGDLLIMPORT int polar_logindex_max_local_cache_segments;
extern PGDLLIMPORT int polar_write_logindex_active_table_delay;
extern PGDLLIMPORT int polar_wait_old_version_page_timeout;
extern PGDLLIMPORT bool polar_enable_fullpage_snapshot;
extern PGDLLIMPORT int polar_invalid_page_forget_queue_size;

typedef struct polar_logindex_promote_replay_t
{
	XLogRecPtr	consist_lsn;
	XLogRecPtr	min_recovery_lsn;
	XLogRecPtr	mark_replayed_lsn;
} polar_logindex_promote_replay_t;

typedef struct polar_invalid_page_forget_req_t
{
	BufferTag	buftag;
	XLogRecPtr	lsn;
} polar_invalid_page_forget_req_t;

typedef struct polar_invalid_page_forget_queue_t
{
	uint64		total_size;
	pg_atomic_uint64 start;
	pg_atomic_uint64 end;
	polar_invalid_page_forget_req_t reqs[FLEXIBLE_ARRAY_MEMBER];
} polar_invalid_page_forget_queue_t;

typedef struct polar_logindex_redo_ctl_data_t
{
	mini_trans_t mini_trans;
	polar_fullpage_ctl_t fullpage_ctl;
	log_index_snapshot wal_logindex_snapshot;
	log_index_snapshot fullpage_logindex_snapshot;
	polar_ringbuf_t xlog_queue;
	polar_rel_size_cache_t rel_size_cache;

	XLogRecPtr	xlog_replay_from;	/* Record the start lsn we replayed from. */

	/*
	 * POLAR: set background process redo state
	 */
	pg_atomic_uint32 bg_redo_state;

	/*
	 * POLAR: The xlog which lsn < bg_replayed_lsn is replayed by background
	 * process. The xlog which lsn < oldest_replayed_lsn is replayed by all
	 * kinds of processes. The info_lck protects bg_replayed_lsn and
	 * oldest_replayed_lsn.
	 */
	XLogRecPtr	bg_replayed_lsn;
	pg_atomic_uint64 oldest_replayed_lsn;
	slock_t		info_lck;

	struct Latch *bg_worker_latch;
	struct Latch *logindex_saver_latch;

	polar_task_sched_t *parallel_sched;

	/* startup's status is shown at the following. */
	pg_atomic_uint32 startup_process_status;
	polar_invalid_page_forget_queue_t *forget_queue;
} polar_logindex_redo_ctl_data_t;

typedef polar_logindex_redo_ctl_data_t *polar_logindex_redo_ctl_t;

typedef struct log_index_redo_t
{
	const char *rm_name;
	/* Used by primary node to save logindex */
	void		(*rm_polar_idx_save) (polar_logindex_redo_ctl_t instance, XLogReaderState *record);
	/* Used by replica node to parse and save logindex */
	bool		(*rm_polar_idx_parse) (polar_logindex_redo_ctl_t instance, XLogReaderState *record);
	/* Used to replay XLOG */
	XLogRedoAction (*rm_polar_idx_redo) (polar_logindex_redo_ctl_t instance, XLogReaderState *record, BufferTag *tag, Buffer *buffer);
} log_index_redo_t;

typedef enum
{
	POLAR_TASK_REPLAY_BUFFER,
	POLAR_TASK_FORGET_INVALID_PAGE,
	POLAR_TASK_CHECK_INVALID_PAGES
} parallel_task_kind;

typedef struct parallel_replay_task_node_t
{
	polar_task_node_t task;
	BufferTag	tag;
	XLogRecPtr	lsn;
	XLogRecPtr	prev_lsn;
	parallel_task_kind kind;
} parallel_replay_task_node_t;

typedef struct polar_logindex_bg_redo_ctl_t
{
	polar_logindex_redo_ctl_t instance;
	log_index_lsn_iter lsn_iter;	/* the iterator for lsn */
	log_index_lsn_info replay_page; /* current iterator page need to do buffer
									 * only replay */
	int			replay_batch_size;	/* The maxium number of page to replay in
									 * one loop */
	XLogReaderState *state;
	XLogRecPtr	max_dispatched_lsn; /* The max lsn value which dispatched to
									 * replay */
	polar_task_sched_ctl_t *sched_ctl;
} polar_logindex_bg_redo_ctl_t;

extern polar_logindex_redo_ctl_t polar_logindex_redo_instance;

typedef enum
{
	POLAR_NOT_LOGINDEX_BG_PROC = 0, /* Indicate it's not related to logindex
									 * background process */
	POLAR_LOGINDEX_DISPATCHER,	/* Indicate it's logindex background
								 * dispatcher process */
	POLAR_LOGINDEX_PARALLEL_REPLAY, /* Indicate it's logindex background
									 * process to do parallel replay */
} polar_logindex_bg_proc_t;

typedef struct polar_checkpoint_ringbuf_t
{
	XLogRecPtr *recptrs;		/* checkpoint start lsn array */
	XLogRecPtr *endptrs;		/* checkpoint end lsn array */
	CheckPoint *checkpoints;	/* checkpoint object array */
	int			head;			/* head index of ringbuf */
	int			tail;			/* tail index of ringbuf */
	int			size;			/* size of ringbuf */
	uint64_t	found_count;
	uint64_t	unfound_count;
	uint64_t	evict_count;
	uint64_t	add_count;
} polar_checkpoint_ringbuf_t;

typedef polar_checkpoint_ringbuf_t *polar_checkpoint_ringbuf;

/* POLAR: Flag set when in replaying and marking buffer dirty process */
extern polar_logindex_bg_proc_t polar_bg_replaying_process;

#define POLAR_IS_PARALLEL_REPLAY_WORKER() (polar_bg_replaying_process == POLAR_LOGINDEX_PARALLEL_REPLAY)

/*
 * POLAR: parallel replay may only be enabled or disabled in some
 * special node status and in some special node types.
 *
 * POLAR_ENABLE_IN_RECOVERY: enable parallel replay after startup
 * process reached consistency.
 *
 * POLAR_ENABLE_IN_INSTANT_RECOVERY: enable parallel replay before
 * startup process reached consistency.
 *
 * POLAR_ENABLE_IN_ASYNC_INSTANT_RECOVERY: just like the mode
 * POLAR_ENABLE_IN_INSTANT_RECOVERY, but startup process won't
 * wait for parallel replay to finish before primary node accept
 * read-write connections and the delta between replay_lsn and
 * bg_replayed_lsn won't be limited by guc any more.
 */
#define POLAR_DISABLE_PARALLEL_REPLAY			0x00000000
#define POLAR_ENABLE_IN_RECOVERY				0x00000001
#define POLAR_ENABLE_IN_INSTANT_RECOVERY		0x00000002
#define POLAR_ENABLE_IN_ASYNC_INSTANT_RECOVERY	0x00000004
#define POLAR_ENABLE_STATUS_MASK				0x00000007

#define POLAR_PARALLEL_REPLAY_DEFAULT_MODE	(POLAR_ENABLE_IN_RECOVERY | POLAR_ENABLE_IN_INSTANT_RECOVERY)

typedef struct polar_node_status_t
{
	int			primary_status;
	int			pitr_status;
	int			standby_status;
} polar_node_status_t;

extern polar_node_status_t polar_parallel_replay_mode;

#define POLAR_ENABLE_IN_PRIMARY(cmd, status)	((cmd)->primary_status & (status))
#define POLAR_ENABLE_IN_PITR(cmd, status)		((cmd)->pitr_status & (status))
#define POLAR_ENABLE_IN_STANDBY(cmd, status)	((cmd)->standby_status & (status))

/* enable parallel replay during normal recovery in stanndby node and PITR */
#define POLAR_PARALLEL_REPLAY_IN_STANDBY_RECOVERY() (polar_logindex_redo_instance && polar_is_standby() && \
	POLAR_ENABLE_IN_STANDBY(&polar_parallel_replay_mode, POLAR_ENABLE_IN_RECOVERY))

#define POLAR_PARALLEL_REPLAY_IN_PITR_RECOVERY() (polar_logindex_redo_instance && polar_is_pitr_primary() && \
	POLAR_ENABLE_IN_PITR(&polar_parallel_replay_mode, POLAR_ENABLE_IN_RECOVERY))

#define POLAR_PARALLEL_REPLAY_IN_RECOVERY() \
	(POLAR_PARALLEL_REPLAY_IN_STANDBY_RECOVERY() | POLAR_PARALLEL_REPLAY_IN_PITR_RECOVERY())

/* enable parallel replay during crash recovery in primary node and standby node */
#define POLAR_PARALLEL_REPLAY_IN_PRIMARY_INSTANT_RECOVERY() (polar_logindex_redo_instance && \
	((polar_is_primary() && POLAR_ENABLE_IN_PRIMARY(&polar_parallel_replay_mode, POLAR_ENABLE_IN_INSTANT_RECOVERY)) || \
	 (polar_is_pitr_primary() && POLAR_ENABLE_IN_PITR(&polar_parallel_replay_mode, POLAR_ENABLE_IN_INSTANT_RECOVERY))))

#define POLAR_PARALLEL_REPLAY_IN_STANDBY_INSTANT_RECOVERY() (polar_logindex_redo_instance && polar_is_standby() && \
	POLAR_ENABLE_IN_STANDBY(&polar_parallel_replay_mode, POLAR_ENABLE_IN_INSTANT_RECOVERY))

#define POLAR_PARALLEL_REPLAY_IN_INSTANT_RECOVERY() \
	(POLAR_PARALLEL_REPLAY_IN_PRIMARY_INSTANT_RECOVERY() || POLAR_PARALLEL_REPLAY_IN_STANDBY_INSTANT_RECOVERY())

/* enable parallel replay in async mode during crash recovery in primary node and standby node */
#define POLAR_PARALLEL_REPLAY_IN_PRIMARY_ASYNC_INSTANT_RECOVERY() (polar_logindex_redo_instance && \
	((polar_is_primary() && POLAR_ENABLE_IN_PRIMARY(&polar_parallel_replay_mode, POLAR_ENABLE_IN_ASYNC_INSTANT_RECOVERY)) || \
	 (polar_is_pitr_primary() && POLAR_ENABLE_IN_PITR(&polar_parallel_replay_mode, POLAR_ENABLE_IN_ASYNC_INSTANT_RECOVERY))))

#define POLAR_PARALLEL_REPLAY_IN_STANDBY_ASYNC_INSTANT_RECOVERY() (polar_logindex_redo_instance && polar_is_standby() && \
	POLAR_ENABLE_IN_PRIMARY(&polar_parallel_replay_mode, POLAR_ENABLE_IN_ASYNC_INSTANT_RECOVERY))

#define POLAR_PARALLEL_REPLAY_IN_ASYNC_INSTANT_RECOVERY() \
	(POLAR_PARALLEL_REPLAY_IN_PRIMARY_ASYNC_INSTANT_RECOVERY() || POLAR_PARALLEL_REPLAY_IN_STANDBY_ASYNC_INSTANT_RECOVERY())

/*
 * 1. If it's replica mode and logindex is enabled, we parse XLOG and save it
 * into logindex.
 * 2. If it's in parallel replay state and logindex is enabled, we also parse
 * XLOG and save it into logindex.
 */
#define POLAR_ENABLE_LOGINDEX_PARSE() \
	((polar_logindex_redo_instance && polar_is_replica()) || \
	 (polar_get_bg_redo_state(polar_logindex_redo_instance) == POLAR_BG_PARALLEL_REPLAYING))

#define POLAR_LOGINDEX_FULLPAGE_CTL_EXIST()	(polar_logindex_redo_instance && polar_logindex_redo_instance->fullpage_ctl)
#define POLAR_LOGINDEX_ENABLE_FULLPAGE() (polar_enable_fullpage_snapshot && POLAR_LOGINDEX_FULLPAGE_CTL_EXIST())
#define POLAR_LOGINDEX_ENABLE_ONLINE_PROMOTE() (polar_enable_shared_storage_mode && polar_logindex_redo_instance && polar_is_replica())

/* POLAR: Enable resolve conflict in non-replica nodes or in replica node with polar_enable_ro_resolve_conflict */
#define POLAR_LOGINDEX_ENABLE_RESOLVE_CONFLICT() (!polar_is_replica() || polar_enable_ro_resolve_conflict)

extern void polar_wal_logindex_init_lwlock_array(log_index_snapshot logindex_snapshot);
extern Size polar_logindex_redo_shmem_size(void);
extern void polar_logindex_redo_shmem_init(void);

extern XLogRecPtr polar_logindex_redo_init(polar_logindex_redo_ctl_t instance, XLogRecPtr checkpoint_lsn,
										   TimeLineID checkpoint_tli, bool read_only);
extern void polar_logindex_redo_flush_data(polar_logindex_redo_ctl_t instance, XLogRecPtr checkpoint_lsn);
extern bool polar_logindex_redo_bg_flush_data(polar_logindex_redo_ctl_t instance);
extern bool polar_logindex_redo_bg_replay(polar_logindex_bg_redo_ctl_t *ctl, bool *can_hold, Latch *latch);
extern XLogRecPtr polar_logindex_redo_get_min_replay_from_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr consist_lsn);
extern bool polar_logindex_parse_xlog(polar_logindex_redo_ctl_t instance, RmgrId rmid, XLogReaderState *state, XLogRecPtr redo_start_lsn, XLogRecPtr *mini_trans_lsn);
extern void polar_logindex_primary_save(polar_logindex_redo_ctl_t instance);
extern void polar_advance_bg_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr lsn);
extern void polar_reset_bg_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr lsn);
extern XLogRecPtr polar_get_bg_replayed_lsn(polar_logindex_redo_ctl_t instance);
extern void polar_reset_oldest_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr lsn);
extern void polar_advance_oldest_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr lsn);
extern XLogRecPtr polar_get_oldest_replayed_lsn(polar_logindex_redo_ctl_t instance);
extern bool polar_logindex_require_backend_redo(polar_logindex_redo_ctl_t instance, ForkNumber fork_num, XLogRecPtr *replay_from);
extern void polar_logindex_redo_set_valid_info(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn);
extern XLogRecPtr polar_logindex_redo_parse_start_lsn(polar_logindex_redo_ctl_t instance);

extern XLogRecord *polar_logindex_read_xlog(XLogReaderState *state, XLogRecPtr lsn);

extern void polar_logindex_remove_old_files(polar_logindex_redo_ctl_t instance);
extern void polar_logindex_remove_all(void);
extern XLogRecPtr polar_logindex_redo_start_lsn(polar_logindex_redo_ctl_t instance);
extern void polar_logindex_redo_abort(polar_logindex_redo_ctl_t instance);

extern bool polar_logindex_io_lock_apply(polar_logindex_redo_ctl_t instance, BufferDesc *buf_hdr, XLogRecPtr replay_from, XLogRecPtr checkpoint_lsn);

extern XLogRecPtr polar_logindex_apply_page(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn, XLogRecPtr end_lsn, BufferTag *tag, Buffer *buffer);
extern void polar_logindex_lock_apply_buffer(polar_logindex_redo_ctl_t instance, Buffer *buffer);
extern bool polar_logindex_lock_apply_page_from(polar_logindex_redo_ctl_t instance, XLogRecPtr start_lsn, BufferTag *tag, Buffer *buffer);
extern bool polar_logindex_restore_fullpage_snapshot_if_needed(polar_logindex_redo_ctl_t instance, BufferTag *tag, Buffer *buffer);

extern void polar_bg_redo_mark_logindex_ready(void);
extern XLogRecPtr polar_max_xlog_rec_ptr(XLogRecPtr a, XLogRecPtr b);
extern XLogRecPtr polar_min_xlog_rec_ptr(XLogRecPtr a, XLogRecPtr b);
extern void polar_log_page_iter_context(void);

extern void polar_logindex_apply_one_record(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);

extern void polar_logindex_save_lsn(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_logindex_save_vm_block(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 heap_block);
extern void polar_logindex_save_block(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id);

extern void polar_fullpage_logindex_save_block(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id);

extern Buffer polar_logindex_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, bool get_cleanup_lock, polar_page_lock_t * page_lock);

extern Buffer polar_logindex_outdate_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, bool get_cleanup_lock, polar_page_lock_t * page_lock, bool vm_parse);

extern void polar_logindex_redo_vm_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record, uint8 heap_block);
extern void polar_logindex_redo_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id);
extern void polar_bitmap_logindex_redo_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag tag);
extern void polar_logindex_cleanup_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state, uint8 block_id);
extern bool polar_xlog_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_xlog_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_xlog_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_storage_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record);
extern bool polar_heap2_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_heap2_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_heap2_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_heap_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_heap_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_heap_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_btree_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_btree_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_btree_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_hash_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_hash_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_hash_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_bitmap_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_bitmap_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_bitmap_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);


extern bool polar_gin_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_gin_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_gin_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_gist_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_gist_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_gist_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_seq_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_seq_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_seq_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_spg_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_spg_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_spg_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_brin_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_brin_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_brin_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);
extern bool polar_generic_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern void polar_generic_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *state);
extern XLogRedoAction polar_generic_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *state, BufferTag *tag, Buffer *buffer);

extern void polar_set_bg_redo_state(polar_logindex_redo_ctl_t instance, uint32 state);

extern void polar_logindex_wakeup_bg_replay(polar_logindex_redo_ctl_t instance, XLogRecPtr replayed_lsn);
extern void polar_logindex_bg_worker_main(char *startup_data, size_t startup_data_len) pg_attribute_noreturn();
extern polar_task_sched_ctl_t *polar_create_parallel_replay_sched_ctl(polar_logindex_bg_redo_ctl_t *bg_ctl);
extern void polar_logindex_saver_main(Datum main_args);
extern void polar_register_logindex_primary_saver(void);

extern bool polar_need_do_bg_replay(polar_logindex_redo_ctl_t instance);
extern bool polar_logindex_finished_bg_replay(polar_logindex_redo_ctl_t instance);
extern polar_logindex_bg_redo_ctl_t *polar_create_bg_redo_ctl(polar_logindex_redo_ctl_t instance, bool enable_processes_pool);
extern void polar_release_bg_redo_ctl(polar_logindex_bg_redo_ctl_t *ctl);
extern void polar_promote_reset_bg_replayed_lsn(polar_logindex_redo_ctl_t instance, XLogRecPtr oldest_redo_ptr);
extern void polar_logindex_promote_xlog_queue(polar_logindex_redo_ctl_t instance);
extern void polar_online_promote_data(polar_logindex_redo_ctl_t instance, TransactionId oldest_active_xid);
extern void polar_standby_promote_data(polar_logindex_redo_ctl_t instance);
extern void polar_primary_promote_data(polar_logindex_redo_ctl_t instance);
extern void polar_wait_logindex_worker_handle_promote(polar_logindex_redo_ctl_t instance);
extern void polar_wait_logindex_worker_finish_parallel_replay(polar_logindex_redo_ctl_t instance);
extern void polar_logindex_replay_db(polar_logindex_redo_ctl_t instance, Oid db_oid);

extern void polar_checkpoint_ringbuf_init(polar_checkpoint_ringbuf checkpoint_rbuf, int size);
extern void polar_checkpoint_ringbuf_pop(polar_checkpoint_ringbuf checkpoint_rbuf,
										 XLogRecPtr *checkpointRecPtr,
										 XLogRecPtr *checkpointEndPtr,
										 CheckPoint *checkpoint);
extern bool polar_checkpoint_ringbuf_push(polar_checkpoint_ringbuf checkpoint_rbuf,
										  XLogRecPtr checkpointRecPtr,
										  XLogRecPtr checkpointEndPtr,
										  const CheckPoint *checkpoint);
extern void polar_checkpoint_ringbuf_check(XLogRecPtr *checkpointRecPtr,
										   XLogRecPtr *checkpointEndPtr,
										   CheckPoint *checkpoint);
extern void polar_checkpoint_ringbuf_free(polar_checkpoint_ringbuf checkpoint_rbuf);

extern void polar_parallel_replay_check_invalid_pages(void);
extern void polar_parallel_replay_forget_invalid_pages(RelFileLocator locator, ForkNumber forkno, BlockNumber minblkno);
extern void polar_parallel_replay_forget_invalid_pages_db(Oid dbid);
extern void polar_replica_set_cascade_recovery(void);

#define POLAR_MAX_REPLAY_END_CALLBACK XLR_MAX_BLOCK_ID

#define POLAR_READ_MODE(buffer) (BufferIsValid(buffer) ? RBM_NORMAL_VALID : RBM_NORMAL)

#define POLAR_READ_BUFFER_FOR_REDO(record, block_id, buffer) \
	XLogReadBufferForRedoExtended((record), (block_id),\
								  POLAR_READ_MODE(*(buffer)), false, (buffer))

#define POLAR_INIT_BUFFER_FOR_REDO(record, block_id, buffer) \
	do { \
		*(buffer) = (!BufferIsValid(*buffer)) ? XLogInitBufferForRedo(record, block_id) : *(buffer); \
	} while (0)

#define POLAR_MINI_TRANS_REDO_PARSE(instance, record, block_id, tag, lock, buf) \
	do { \
		POLAR_GET_LOG_TAG((record), (tag), (block_id)); \
		lock = polar_logindex_mini_trans_lock((instance)->mini_trans, &(tag), LW_EXCLUSIVE, NULL); \
		buf = polar_logindex_parse((instance), (record), &(tag), false, &(lock)); \
	} while (0)

#define POLAR_MINI_TRANS_CLEANUP_PARSE(instance, record, block_id, tag, lock, buf) \
	do { \
		POLAR_GET_LOG_TAG((record), (tag), (block_id)); \
		lock = polar_logindex_mini_trans_lock((instance)->mini_trans, &(tag), LW_EXCLUSIVE, NULL); \
		buf = polar_logindex_parse((instance), (record), &(tag), true, &(lock)); \
	} while (0)

#define POLAR_REL_FILE_LOCATOR(tag) \
	((RelFileLocator){ \
		.spcOid = (tag)->spcOid, \
		.dbOid = (tag)->dbOid, \
		.relNumber = (tag)->relNumber \
	})

#define POLAR_GET_VM_TAG(vm_tag, heap_tag) \
	do \
	{ \
		(vm_tag)->spcOid = (heap_tag)->spcOid; \
		(vm_tag)->dbOid = (heap_tag)->dbOid; \
		(vm_tag)->relNumber = (heap_tag)->relNumber; \
		(vm_tag)->forkNum = VISIBILITYMAP_FORKNUM; \
		(vm_tag)->blockNum = HEAPBLK_TO_MAPBLOCK((heap_tag)->blockNum); \
	} while (0)

#define POLAR_INIT_BUFFER_TAG(dst_tag, src_tag) \
	do \
	{ \
		(dst_tag)->spcOid = (src_tag)->spcOid; \
		(dst_tag)->dbOid = (src_tag)->dbOid; \
		(dst_tag)->relNumber = (src_tag)->relNumber; \
		(dst_tag)->forkNum = (src_tag)->forkNum; \
		(dst_tag)->blockNum = (src_tag)->blockNum; \
	} while (0)

/*
 * POLAR: Flags for buffer redo state
 * Note: Must update POLAR_BUF_REDO_FLAG_MASK when delete or add more state flags
 */
#define POLAR_REDO_LOCKED               (1U << 1)	/* redo state is locked */
#define POLAR_REDO_READ_IO_END          (1U << 2)	/* Finish to read buffer
													 * content from storage */
#define POLAR_REDO_REPLAYING            (1U << 3)	/* It's replaying buffer
													 * content */
#define POLAR_REDO_OUTDATE              (1U << 4)	/* The buffer content is
													 * outdated */
#define POLAR_REDO_INVALIDATE           (1U << 5)	/* This buffer is
													 * invalidating */
/* POLAR: Mask for redo flag */
#define POLAR_BUF_REDO_FLAG_MASK (0x003EU)

/*
 * POLAR: Background redo state
 */
#define POLAR_BG_REDO_NOT_START         (0) /* Background process is not doing
											 * replaying */
#define POLAR_BG_REPLICA_BUF_REPLAYING  (1) /* Background process is replaying
											 * buffer which exists in buffer
											 * pool */
#define POLAR_BG_PARALLEL_REPLAYING     (2) /* Background process is replaying
											 * in parallel and marking all
											 * buffer dirty */
#define POLAR_BG_WAITING_RESET          (3) /* Background process is waiting
											 * for startup process to reset
											 * replay lsn for online promote */
#define POLAR_BG_ONLINE_PROMOTE         (4) /* Background process is replaying
											 * and marking all buffer dirty in
											 * parallel from last consistent
											 * lsn */

/* POLAR: startup process's status */
#define POLAR_STARTUP_WAIT_FINISH_REPLAY				(0x00000001)
#define POLAR_STARTUP_WAIT_FORGET_INVALID_PAGES			(0x00000002)
#define POLAR_STARTUP_WAIT_CHECK_INVALID_PAGES			(0x00000004)
#define POLAR_STARTUP_REACHED_CONSISTENCY				(0x00000008)
#define POLAR_STARTUP_RUNNING							(0x00000010)
#define POLAR_STARTUP_IN_CASCADE_REPLICA				(0x00000020)
#define POLAR_STARTUP_STATUS_MASK						(0x0000003F)

#define POLAR_STARTUP_IN_STATUS(ins, status) \
	(pg_atomic_read_u32(&(ins)->startup_process_status) & ((status) & POLAR_STARTUP_STATUS_MASK))

#define POLAR_STARTUP_SET_STATUS(ins, status) \
	pg_atomic_fetch_or_u32(&(ins)->startup_process_status, ((status) & POLAR_STARTUP_STATUS_MASK))

#define POLAR_STARTUP_REMOVE_STATUS(ins, status) \
	pg_atomic_fetch_and_u32(&(ins)->startup_process_status, (~((status) & POLAR_STARTUP_STATUS_MASK)))

/*
 * Functions for acquiring/releasing a shared buffer redo state's spinlock.
 */
extern uint32 polar_lock_redo_state(BufferDesc *desc);
inline static void
polar_unlock_redo_state(BufferDesc *desc, uint32 state)
{
	do
	{
		pg_write_barrier();
		pg_atomic_write_u32(&(desc->polar_redo_state), state & (~POLAR_REDO_LOCKED));
	}
	while (0);
}

inline static bool
polar_redo_check_state(BufferDesc *desc, uint32 state)
{
	pg_read_barrier();
	return pg_atomic_read_u32(&(desc->polar_redo_state)) & state;
}

#define POLAR_RECORD_DB_STATE(nspcs, spcs, db, state) \
	do {    \
		XLogRecPtr prds_lsn; \
		int prds_i; \
		if (polar_logindex_redo_instance) \
		{ \
			LWLockAcquire(POLAR_REL_SIZE_CACHE_LOCK(polar_logindex_redo_instance->rel_size_cache), LW_EXCLUSIVE); \
			prds_lsn = InRecovery ? GetCurrentReplayRecPtr(NULL) : GetXLogInsertRecPtr(); \
			POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(prds_lsn)); \
			for (prds_i = 0; prds_i < nspcs; prds_i ++) \
				polar_record_db_state(polar_logindex_redo_instance->rel_size_cache, (prds_lsn), (((Oid *)spcs)[prds_i]), (db), (state)); \
			LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(polar_logindex_redo_instance->rel_size_cache)); \
		} \
	} while (0)

#define POLAR_RECORD_REL_SIZE(node, nforks, forks, nblocks) \
	do { \
		XLogRecPtr prrs_lsn; \
		int prrs_i; \
		if (polar_logindex_redo_instance) \
		{ \
			LWLockAcquire(POLAR_REL_SIZE_CACHE_LOCK(polar_logindex_redo_instance->rel_size_cache), LW_EXCLUSIVE); \
			prrs_lsn = InRecovery ? GetCurrentReplayRecPtr(NULL) : GetXLogInsertRecPtr(); \
			POLAR_ASSERT_PANIC(!XLogRecPtrIsInvalid(prrs_lsn)); \
			for (prrs_i = 0; prrs_i < nforks; prrs_i ++) \
				polar_record_rel_size(polar_logindex_redo_instance->rel_size_cache, (prrs_lsn), (node), (((ForkNumber *)forks)[prrs_i]), (((BlockNumber *)nblocks)[prrs_i])); \
			LWLockRelease(POLAR_REL_SIZE_CACHE_LOCK(polar_logindex_redo_instance->rel_size_cache)); \
		} \
	} while (0)

static inline uint32
polar_get_bg_redo_state(polar_logindex_redo_ctl_t instance)
{
	if (instance)
		return pg_atomic_read_u32(&instance->bg_redo_state);

	return POLAR_BG_REDO_NOT_START;
}

/*
 * In the future, if possible, it might be better to change bg_redo_state to a
 * flag-format state, where online promote state is independent from parallel
 * replay state and there are four possibilities for combining the two states.
 *
 * But for now, adding a state called POLAR_BG_PARALLEL_REPLAYING standing for
 * standby parallel replay and replica prewarm (in the future) affects the original
 * code least, and safety comes first.
 */
inline static bool
polar_bg_redo_state_is_parallel(polar_logindex_redo_ctl_t instance)
{
	uint32		state = polar_get_bg_redo_state(instance);

	return state == POLAR_BG_ONLINE_PROMOTE ||
		state == POLAR_BG_PARALLEL_REPLAYING;
}

/*
 * Remember that we want to wakeup logindex saver later
 *
 * This is separated from doing the actual wakeup because the
 * writeout is done while holding contended locks.
 */
#define polar_logindex_saver_wakeup_req() \
	do { polar_wake_logindex_saver = true; } while (0)

/*
 * wakeup logindex saver if there is work to be done
 */
static inline void
polar_logindex_saver_process_wakeup_reqs(void)
{
	if (polar_wake_logindex_saver)
	{
		polar_wake_logindex_saver = false;
		if (polar_logindex_redo_instance &&
			polar_logindex_redo_instance->logindex_saver_latch)
			SetLatch(polar_logindex_redo_instance->logindex_saver_latch);
	}
}

#endif
