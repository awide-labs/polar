/*-------------------------------------------------------------------------
 *
 * polar_monitor_buf.c
 *	  display some information of PolarDB buffer.
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    external/polar_monitor/polar_monitor_buf.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_flush.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"

#define NUM_NORMAL_BUFFERCACHE_PAGES_ELEM   17
#define NUM_COPY_BUFFERCACHE_PAGES_ELEM 12

/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{
	uint32		bufferid;
	Oid			relfilenode;
	Oid			reltablespace;
	Oid			reldatabase;
	ForkNumber	forknum;
	BlockNumber blocknum;
	bool		isvalid;
	bool		isdirty;
	uint16		usagecount;
	XLogRecPtr	oldest_lsn;
	XLogRecPtr	newest_lsn;
	int			flush_next;
	int			flush_prev;
	bool		incopybuffer;
	bool		first_touched_after_copy;
	uint16		recently_modified_count;
	bool		oldest_lsn_is_fake;

	/*
	 * An int32 is sufficiently large, as MAX_BACKENDS prevents a buffer from
	 * being pinned by too many backends and each backend will only pin once
	 * because of bufmgr.c's PrivateRefCount infrastructure.
	 */
	int32		pinning_backends;
} NormalBufferCachePagesRec;

/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{
	TupleDesc	tupdesc;
	NormalBufferCachePagesRec *record;
} NormalBufferCachePagesContext;

/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{
	uint32		bufferid;
	Oid			relfilenode;
	Oid			reltablespace;
	Oid			reldatabase;
	ForkNumber	forknum;
	BlockNumber blocknum;
	int			free_next;
	uint32		pass_count;
	int			state;
	XLogRecPtr	oldest_lsn;
	XLogRecPtr	newest_lsn;
	bool		is_flushed;
} CopyBufferCachePagesRec;

/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{
	TupleDesc	tupdesc;
	CopyBufferCachePagesRec *record;
} CopyBufferCachePagesContext;

/*
 * Function returning data from the shared buffer cache - buffer number,
 * relation node/tablespace/database/blocknum and dirty indicator.
 */
PG_FUNCTION_INFO_V1(polar_get_normal_buffercache_pages);

Datum
polar_get_normal_buffercache_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	NormalBufferCachePagesContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;

	if (SRF_IS_FIRSTCALL())
	{
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (NormalBufferCachePagesContext *) palloc(sizeof(NormalBufferCachePagesContext));

		/*
		 * To smoothly support upgrades from version 1.0 of this extension
		 * transparently handle the (non-)existence of the pinning_backends
		 * column. We unfortunately have to get the result type for that... -
		 * we can't use the result type determined by the function definition
		 * without potentially crashing when somebody uses the old (or even
		 * wrong) function definition though.
		 */
		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts != NUM_NORMAL_BUFFERCACHE_PAGES_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows. */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relforknumber",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 7, "isdirty",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 8, "usage_count",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 9, "oldest_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 10, "newest_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 11, "flush_next",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 12, "flush_prev",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 13, "incopybuf",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 14, "first_touched_after_copy",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 15, "pinning_backends",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 16, "recently_modified_count",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 17, "oldest_lsn_is_fake",
						   BOOLOID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/* Allocate NBuffers worth of NormapBufferCachePagesRec records. */
		fctx->record = (NormalBufferCachePagesRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(NormalBufferCachePagesRec) * NBuffers);

		/* Set max calls and remember the user function context. */
		funcctx->max_calls = NBuffers;
		funcctx->user_fctx = fctx;

		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf_hdr;
			uint32		buf_state;

			buf_hdr = GetBufferDescriptor(i);
			/* Lock each buffer header before inspecting. */
			buf_state = LockBufHdr(buf_hdr);

			fctx->record[i].bufferid = BufferDescriptorGetBuffer(buf_hdr);
			fctx->record[i].relfilenode = buf_hdr->tag.rnode.relNode;
			fctx->record[i].reltablespace = buf_hdr->tag.rnode.spcNode;
			fctx->record[i].reldatabase = buf_hdr->tag.rnode.dbNode;
			fctx->record[i].forknum = buf_hdr->tag.forkNum;
			fctx->record[i].blocknum = buf_hdr->tag.blockNum;
			fctx->record[i].usagecount = BUF_STATE_GET_USAGECOUNT(buf_state);
			fctx->record[i].pinning_backends = BUF_STATE_GET_REFCOUNT(buf_state);

			/*
			 * We don't lock buffer content, maybe these status is not very
			 * correct
			 */
			fctx->record[i].oldest_lsn = buf_hdr->oldest_lsn;
			fctx->record[i].newest_lsn = BufferGetLSN(buf_hdr);
			fctx->record[i].flush_next = buf_hdr->flush_next;
			fctx->record[i].flush_prev = buf_hdr->flush_prev;
			fctx->record[i].incopybuffer = (buf_hdr->copy_buffer ? true : false);
			fctx->record[i].first_touched_after_copy = buf_hdr->polar_flags & POLAR_BUF_FIRST_TOUCHED_AFTER_COPY;
			fctx->record[i].oldest_lsn_is_fake = buf_hdr->polar_flags & POLAR_BUF_OLDEST_LSN_IS_FAKE;
			fctx->record[i].recently_modified_count = buf_hdr->recently_modified_count;
			fctx->record[i].isdirty = (buf_state & BM_DIRTY);
			fctx->record[i].isvalid = (buf_state & BM_VALID) && (buf_state & BM_TAG_VALID);

			UnlockBufHdr(buf_hdr, buf_state);
		}
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_NORMAL_BUFFERCACHE_PAGES_ELEM];
		bool		nulls[NUM_NORMAL_BUFFERCACHE_PAGES_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		MemSet(nulls, 0, sizeof(nulls));

		/*
		 * Set all fields except the bufferid to null if the buffer is unused
		 * or not valid.
		 */
		if (fctx->record[i].blocknum == InvalidBlockNumber ||
			fctx->record[i].isvalid == false)
			MemSet(&nulls[1], 1, NUM_NORMAL_BUFFERCACHE_PAGES_ELEM - 1);
		else
		{
			values[1] = ObjectIdGetDatum(fctx->record[i].relfilenode);
			values[2] = ObjectIdGetDatum(fctx->record[i].reltablespace);
			values[3] = ObjectIdGetDatum(fctx->record[i].reldatabase);
			values[4] = ObjectIdGetDatum(fctx->record[i].forknum);
			values[5] = Int64GetDatum((int64) fctx->record[i].blocknum);
			values[6] = BoolGetDatum(fctx->record[i].isdirty);
			values[7] = Int16GetDatum(fctx->record[i].usagecount);
			values[8] = LSNGetDatum(fctx->record[i].oldest_lsn);
			values[9] = LSNGetDatum(fctx->record[i].newest_lsn);
			values[10] = Int32GetDatum(fctx->record[i].flush_next);
			values[11] = Int32GetDatum(fctx->record[i].flush_prev);
			values[12] = BoolGetDatum(fctx->record[i].incopybuffer);
			values[13] = BoolGetDatum(fctx->record[i].first_touched_after_copy);
			values[14] = Int32GetDatum(fctx->record[i].pinning_backends);
			values[15] = UInt16GetDatum(fctx->record[i].recently_modified_count);
			values[16] = BoolGetDatum(fctx->record[i].oldest_lsn_is_fake);
		}

		/* Build and return the tuple. */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

/*
 * Function returning data from the shared copy buffer cache.
 */
PG_FUNCTION_INFO_V1(polar_get_copy_buffercache_pages);

Datum
polar_get_copy_buffercache_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	CopyBufferCachePagesContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;

	if (!polar_copy_buffer_enabled())
		PG_RETURN_NULL();

	if (SRF_IS_FIRSTCALL())
	{
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (CopyBufferCachePagesContext *) palloc(sizeof(CopyBufferCachePagesContext));

		/*
		 * To smoothly support upgrades from version 1.0 of this extension
		 * transparently handle the (non-)existence of the pinning_backends
		 * column. We unfortunately have to get the result type for that... -
		 * we can't use the result type determined by the function definition
		 * without potentially crashing when somebody uses the old (or even
		 * wrong) function definition though.
		 */
		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts != NUM_COPY_BUFFERCACHE_PAGES_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows. */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relforknumber",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 7, "free_next",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 8, "pass_count",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 9, "state",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 10, "oldest_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 11, "newest_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 12, "is_flushed",
						   BOOLOID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/* Allocate NBuffers worth of CopyBufferCachePagesRec records. */
		fctx->record = (CopyBufferCachePagesRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(CopyBufferCachePagesRec) * polar_copy_buffers);

		/* Set max calls and remember the user function context. */
		funcctx->max_calls = polar_copy_buffers;
		funcctx->user_fctx = fctx;

		/* Return to original context when allocating transient memory */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 */
		for (i = 0; i < polar_copy_buffers; i++)
		{
			CopyBufferDesc *cbufHdr = polar_get_copy_buffer_descriptor(i);

			/* Copy buffer without header lock */
			fctx->record[i].bufferid = BufferDescriptorGetBuffer(cbufHdr);
			fctx->record[i].relfilenode = cbufHdr->tag.rnode.relNode;
			fctx->record[i].reltablespace = cbufHdr->tag.rnode.spcNode;
			fctx->record[i].reldatabase = cbufHdr->tag.rnode.dbNode;
			fctx->record[i].forknum = cbufHdr->tag.forkNum;
			fctx->record[i].blocknum = cbufHdr->tag.blockNum;

			/*
			 * We don't lock buffer content, maybe these status is not very
			 * correct
			 */
			fctx->record[i].free_next = cbufHdr->free_next;
			fctx->record[i].pass_count = pg_atomic_read_u32(&cbufHdr->pass_count);
			fctx->record[i].state = (int) cbufHdr->state;
			fctx->record[i].oldest_lsn = cbufHdr->oldest_lsn;
			fctx->record[i].newest_lsn = cbufHdr->state == 0 ? InvalidXLogRecPtr : polar_copy_buffer_get_lsn(cbufHdr);
			fctx->record[i].is_flushed = cbufHdr->is_flushed;
		}
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_COPY_BUFFERCACHE_PAGES_ELEM];
		bool		nulls[NUM_COPY_BUFFERCACHE_PAGES_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		MemSet(nulls, 0, sizeof(nulls));

		/*
		 * Set all fields except the bufferid to null if the buffer is unused
		 * or not valid.
		 */
		if (fctx->record[i].blocknum == InvalidBlockNumber)
			MemSet(&nulls[1], 1, NUM_COPY_BUFFERCACHE_PAGES_ELEM - 1);
		else
		{
			values[1] = ObjectIdGetDatum(fctx->record[i].relfilenode);
			values[2] = ObjectIdGetDatum(fctx->record[i].reltablespace);
			values[3] = ObjectIdGetDatum(fctx->record[i].reldatabase);
			values[4] = ObjectIdGetDatum(fctx->record[i].forknum);
			values[5] = Int64GetDatum((int64) fctx->record[i].blocknum);
			values[6] = Int32GetDatum(fctx->record[i].free_next);
			values[7] = UInt32GetDatum(fctx->record[i].pass_count);
			values[8] = Int16GetDatum(fctx->record[i].state);
			values[9] = LSNGetDatum(fctx->record[i].oldest_lsn);
			values[10] = LSNGetDatum(fctx->record[i].newest_lsn);
			values[11] = BoolGetDatum(fctx->record[i].is_flushed);
		}

		/* Build and return the tuple. */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(polar_flushlist);

Datum
polar_flushlist(PG_FUNCTION_ARGS)
{
#define FLUSH_LIST_COLUMN_SIZE 8

	TupleDesc	tupdesc;
	Datum		values[FLUSH_LIST_COLUMN_SIZE];
	bool		nulls[FLUSH_LIST_COLUMN_SIZE];

	if (!polar_flush_list_enabled())
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(FLUSH_LIST_COLUMN_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "put", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "remove", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "find", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "batchread", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "cbuf", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "vm_put", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "vm_remove", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(pg_atomic_read_u32(&polar_flush_ctl->count));
	values[1] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->insert));
	values[2] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->remove));
	values[3] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->find));
	values[4] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->batch_read));
	values[5] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->cbuf));
	values[6] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->vm_insert));
	values[7] = UInt64GetDatum(pg_atomic_read_u64(&polar_flush_ctl->vm_remove));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(polar_cbuf);

Datum
polar_cbuf(PG_FUNCTION_ARGS)
{
#define CBUF_COLUMN_SIZE 5

	TupleDesc	tupdesc;
	Datum		values[CBUF_COLUMN_SIZE];
	bool		nulls[CBUF_COLUMN_SIZE];

	if (!polar_copy_buffer_enabled())
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(CBUF_COLUMN_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "flush", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "copy", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "unavailable", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "full", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "release", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	values[0] = UInt64GetDatum(pg_atomic_read_u64(&polar_copy_buffer_ctl->flushed_count));
	values[1] = UInt64GetDatum(pg_atomic_read_u64(&polar_copy_buffer_ctl->copied_count));
	values[2] = UInt64GetDatum(pg_atomic_read_u64(&polar_copy_buffer_ctl->unavailable_count));
	values[3] = UInt64GetDatum(pg_atomic_read_u64(&polar_copy_buffer_ctl->full_count));
	values[4] = UInt64GetDatum(pg_atomic_read_u64(&polar_copy_buffer_ctl->release_count));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(polar_backend_flush);
Datum
polar_backend_flush(PG_FUNCTION_ARGS)
{
	if (!polar_flush_list_enabled())
		PG_RETURN_NULL();

	PG_RETURN_UINT64(pg_atomic_read_u64(&polar_flush_ctl->backend_flush));
}

PG_FUNCTION_INFO_V1(polar_lru_flush_info);

Datum
polar_lru_flush_info(PG_FUNCTION_ARGS)
{
#define LRU_COLUMN_SIZE 4

	TupleDesc	tupdesc;
	Datum		values[LRU_COLUMN_SIZE];
	bool		nulls[LRU_COLUMN_SIZE];
	int			strategy_buf_id;
	uint32		strategy_passes;
	int			lru_buf_id;
	uint32		lru_passes;

	if (!polar_flush_list_enabled())
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(LRU_COLUMN_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "lru_complete_passes", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "lru_buffer_id", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "strategy_passes", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "strategy_buf_id", INT4OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	strategy_buf_id = StrategySyncStart(&strategy_passes, NULL);

	SpinLockAcquire(&polar_flush_ctl->lru_lock);
	lru_buf_id = polar_flush_ctl->lru_buffer_id;
	lru_passes = polar_flush_ctl->lru_complete_passes;
	SpinLockRelease(&polar_flush_ctl->lru_lock);

	values[0] = UInt32GetDatum(lru_passes);
	values[1] = UInt32GetDatum(lru_buf_id);
	values[2] = UInt32GetDatum(strategy_passes);
	values[3] = UInt32GetDatum(strategy_buf_id);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
