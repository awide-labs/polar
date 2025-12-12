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
#include "access/relation.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_flush.h"
#include "storage/polar_io_stat.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/varlena.h"

#define NUM_NORMAL_BUFFERCACHE_PAGES_ELEM   22
#define NUM_COPY_BUFFERCACHE_PAGES_ELEM 12

/*
 * Record structure holding the to be exposed cache data.
 */
typedef struct
{
	uint32		bufferid;
	RelFileNumber relfilenode;
	Oid			reltablespace;
	Oid			reldatabase;
	ForkNumber	forknum;
	BlockNumber blocknum;
	bool		istagvalid;
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
	uint32		buffer_state;
	uint32		redo_state;
	uint32		content_lock_owner;
	int32		wait_backend_pid;
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
	RelFileNumber relfilenode;
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
	int			buf_id = PG_GETARG_INT32(0);

	/* return all buffer info when buf_id is -1 */
	bool		get_all_buffers = (buf_id == -1);
	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	NormalBufferCachePagesContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;

	if (buf_id < -1 || buf_id >= NBuffers)
		elog(ERROR, "buffer_id:%d is invalid, should be in [%d,%d), -1 means get information of all buffers.", buf_id, -1, NBuffers);

	if (SRF_IS_FIRSTCALL())
	{
		int			i;
		int			record_num = get_all_buffers ? NBuffers : 1;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (NormalBufferCachePagesContext *) palloc(sizeof(NormalBufferCachePagesContext));

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
		TupleDescInitEntry(tupledesc, (AttrNumber) 7, "buffer_state",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 8, "isvalid",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 9, "isdirty",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 10, "usage_count",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 11, "pinning_backends",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 12, "oldest_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 13, "newest_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 14, "flush_next",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 15, "flush_prev",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 16, "in_copybuf",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 17, "first_touched_after_copy",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 18, "recently_modified_count",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 19, "oldest_lsn_is_fake",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 20, "redo_state",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 21, "content_lock_owner",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 22, "wait_backend_pid",
						   INT4OID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/* Allocate record_num worth of NormalBufferCachePagesRec records. */
		fctx->record = (NormalBufferCachePagesRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(NormalBufferCachePagesRec) * record_num);

		/* Set max calls and remember the user function context. */
		funcctx->max_calls = record_num;
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
			uint8		polar_flags;
			int			wait_backend_pgprocno;

			buf_hdr = get_all_buffers ? GetBufferDescriptor(i) : GetBufferDescriptor(buf_id);
			/* Lock each buffer header before inspecting. */
			buf_state = LockBufHdr(buf_hdr);

			fctx->record[i].bufferid = buf_hdr->buf_id;
			fctx->record[i].relfilenode = buf_hdr->tag.relNumber;
			fctx->record[i].reltablespace = buf_hdr->tag.spcOid;
			fctx->record[i].reldatabase = buf_hdr->tag.dbOid;
			fctx->record[i].forknum = buf_hdr->tag.forkNum;
			fctx->record[i].blocknum = buf_hdr->tag.blockNum;

			/*
			 * We don't lock buffer content, maybe these status is not very
			 * correct
			 */
			polar_flags = buf_hdr->polar_flags;
			fctx->record[i].oldest_lsn = buf_hdr->oldest_lsn;
			fctx->record[i].newest_lsn = BufferGetLSN(buf_hdr);
			fctx->record[i].flush_next = buf_hdr->flush_next;
			fctx->record[i].flush_prev = buf_hdr->flush_prev;
			fctx->record[i].incopybuffer = (buf_hdr->copy_buffer ? true : false);
			fctx->record[i].recently_modified_count = buf_hdr->recently_modified_count;
			fctx->record[i].redo_state = pg_atomic_read_u32(&buf_hdr->polar_redo_state);
			fctx->record[i].content_lock_owner = pg_atomic_read_u32(&buf_hdr->content_lock.owner_pid);
			wait_backend_pgprocno = buf_hdr->wait_backend_pgprocno;

			UnlockBufHdr(buf_hdr, buf_state);

			fctx->record[i].buffer_state = (buf_state & (~BM_LOCKED));
			fctx->record[i].usagecount = BUF_STATE_GET_USAGECOUNT(buf_state);
			fctx->record[i].pinning_backends = BUF_STATE_GET_REFCOUNT(buf_state);
			fctx->record[i].istagvalid = (buf_state & BM_TAG_VALID);
			fctx->record[i].isvalid = (buf_state & BM_VALID) && (buf_state & BM_TAG_VALID);
			fctx->record[i].isdirty = (buf_state & BM_DIRTY);
			fctx->record[i].first_touched_after_copy = polar_flags & POLAR_BUF_FIRST_TOUCHED_AFTER_COPY;
			fctx->record[i].oldest_lsn_is_fake = polar_flags & POLAR_BUF_OLDEST_LSN_IS_FAKE;

			if (!(buf_state & BM_PIN_COUNT_WAITER) ||
				wait_backend_pgprocno < 0 ||
				wait_backend_pgprocno >= ProcGlobal->allProcCount)
				fctx->record[i].wait_backend_pid = -1;
			else
				fctx->record[i].wait_backend_pid = ProcGlobal->allProcs[wait_backend_pgprocno].pid;

			if (!get_all_buffers)
				break;
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
			fctx->record[i].istagvalid == false)
			MemSet(&nulls[1], 1, NUM_NORMAL_BUFFERCACHE_PAGES_ELEM - 1);
		else
		{
			values[1] = ObjectIdGetDatum(fctx->record[i].relfilenode);
			values[2] = ObjectIdGetDatum(fctx->record[i].reltablespace);
			values[3] = ObjectIdGetDatum(fctx->record[i].reldatabase);
			values[4] = ObjectIdGetDatum(fctx->record[i].forknum);
			values[5] = Int64GetDatum((int64) fctx->record[i].blocknum);
			values[6] = Int64GetDatum((int64) fctx->record[i].buffer_state);
			values[7] = BoolGetDatum(fctx->record[i].isvalid);
			values[8] = BoolGetDatum(fctx->record[i].isdirty);
			values[9] = Int16GetDatum(fctx->record[i].usagecount);
			values[10] = Int32GetDatum(fctx->record[i].pinning_backends);
			values[11] = LSNGetDatum(fctx->record[i].oldest_lsn);
			values[12] = LSNGetDatum(fctx->record[i].newest_lsn);
			values[13] = Int32GetDatum(fctx->record[i].flush_next);
			values[14] = Int32GetDatum(fctx->record[i].flush_prev);
			values[15] = BoolGetDatum(fctx->record[i].incopybuffer);
			values[16] = BoolGetDatum(fctx->record[i].first_touched_after_copy);
			values[17] = UInt16GetDatum(fctx->record[i].recently_modified_count);
			values[18] = BoolGetDatum(fctx->record[i].oldest_lsn_is_fake);
			values[19] = Int64GetDatum((int64) fctx->record[i].redo_state);
			values[20] = UInt32GetDatum(fctx->record[i].content_lock_owner);
			values[21] = UInt32GetDatum(fctx->record[i].wait_backend_pid);
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

		/*
		 * Allocate polar_copy_buffers worth of CopyBufferCachePagesRec
		 * records.
		 */
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
			fctx->record[i].bufferid = cbufHdr->buf_id + 1;
			fctx->record[i].relfilenode = cbufHdr->tag.relNumber;
			fctx->record[i].reltablespace = cbufHdr->tag.spcOid;
			fctx->record[i].reldatabase = cbufHdr->tag.dbOid;
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
	uint64		values_u64[FLUSH_LIST_COLUMN_SIZE];
	bool		nulls[FLUSH_LIST_COLUMN_SIZE];
	int			i;

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
	MemSet(values_u64, 0, sizeof(values_u64));

	for (i = 0; i < POLAR_FLUSHLIST_PARTITIONS; i++)
	{
		values_u64[0] += pg_atomic_read_u32(&polar_flush_ctl->lists[i].count);
		values_u64[1] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].insert);
		values_u64[2] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].remove);
		values_u64[3] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].find);
		values_u64[4] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].batch_read);
		values_u64[5] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].cbuf);
		values_u64[6] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].vm_insert);
		values_u64[7] += pg_atomic_read_u64(&polar_flush_ctl->lists[i].vm_remove);

		/*
		 * fake_lsn has been deleted, for compatibility with older versions,
		 * we reserve this column and set it to 0.
		 */
	}

	for (i = 0; i < FLUSH_LIST_COLUMN_SIZE; i++)
	{
		values[i] = UInt64GetDatum(values_u64[i]);
	}

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

PG_FUNCTION_INFO_V1(polar_get_flushlist_head_bufferid);

Datum
polar_get_flushlist_head_bufferid(PG_FUNCTION_ARGS)
{
	int			buffer_id = POLAR_FLUSHNEXT_NOT_IN_LIST;
	int			partition;

	if (!polar_flush_list_enabled())
		PG_RETURN_INT32(POLAR_FLUSHNEXT_NOT_IN_LIST);

	SpinLockAcquire(&polar_flush_ctl->lock);
	if (polar_flush_ctl->heap.size == 0)
	{
		SpinLockRelease(&polar_flush_ctl->lock);
		PG_RETURN_INT32(POLAR_FLUSHNEXT_NOT_IN_LIST);
	}
	partition = polar_flush_ctl->heap.heap[0].id;
	SpinLockRelease(&polar_flush_ctl->lock);

	Assert(partition >= 0 && partition < POLAR_FLUSHLIST_PARTITIONS);

	SpinLockAcquire(&polar_flush_ctl->lists[partition].flushlist_lock);
	buffer_id = polar_flush_ctl->lists[partition].first_flush_buffer;
	SpinLockRelease(&polar_flush_ctl->lists[partition].flushlist_lock);

	if (buffer_id == POLAR_FLUSHNEXT_END_OF_LIST)
		PG_RETURN_INT32(POLAR_FLUSHNEXT_NOT_IN_LIST);

	PG_RETURN_INT32(buffer_id);
}

PG_FUNCTION_INFO_V1(polar_get_startup_bufferpin_wait_bufid);

Datum
polar_get_startup_bufferpin_wait_bufid(PG_FUNCTION_ARGS)
{
	int			buffer_id = GetStartupBufferPinWaitBufId();

	PG_RETURN_INT32(buffer_id);
}

PG_FUNCTION_INFO_V1(polar_mark_buffer_dirty_hint);

/*
 * Try to call MarkBufferDirtyHint for a shared buffer.
 * This function is intended for testing use only.
 */
Datum
polar_mark_buffer_dirty_hint(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	RangeVar   *relrv;
	Relation	rel;
	Buffer		buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use polar_mark_buffer_dirty_hint function")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION && rel->rd_rel->relkind != RELKIND_INDEX)
		elog(ERROR, "only support RELATION and INDEX");

	if (RELATION_IS_OTHER_TEMP(rel))
		elog(ERROR, "cannot access temporary tables of other sessions");

	if (blkno >= RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM))
		elog(ERROR, "block number %u is out of range for relation \"%s\"", blkno, RelationGetRelationName(rel));

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	MarkBufferDirtyHint(buf, true);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
	relation_close(rel, AccessShareLock);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_write_combine_latency);
Datum
polar_write_combine_latency(PG_FUNCTION_ARGS)
{
#define POLAR_WRITE_COMBINE_LATENCY_COLS 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	tupdesc = CreateTemplateTupleDesc(POLAR_WRITE_COMBINE_LATENCY_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "blocks",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "latency",
					   INT4OID, -1, 0);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	for (int i = 0; i < MAX_BUFFERS_TO_WRITE_BY; ++i)
	{
		Datum		values[POLAR_WRITE_COMBINE_LATENCY_COLS];
		bool		nulls[POLAR_WRITE_COMBINE_LATENCY_COLS];

		if (polar_write_combine_stat->counts[i] == 0)
			continue;

		MemSet(nulls, 0, sizeof(nulls));

		values[0] = Int32GetDatum(i + 1);
		values[1] = Int64GetDatum(polar_write_combine_stat->counts[i]);
		values[2] = Int32GetDatum(polar_write_combine_stat->times[i] / polar_write_combine_stat->counts[i]);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(polar_write_combine_stats_reset);
Datum
polar_write_combine_stats_reset(PG_FUNCTION_ARGS)
{
	memset(polar_write_combine_stat, 0, sizeof(PolarWriteCombineStat));

	PG_RETURN_VOID();
}
