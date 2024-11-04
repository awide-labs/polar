/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

/* POLAR */
#include "common/file_utils.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_fd.h"
#include "storage/polar_flush.h"
#include "utils/guc.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;


/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 *
 *
 * Synchronization/Locking:
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if an individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 */

static Size
polar_zero_buffer_shmem_size()
{
	/* -1 indicates a request for auto-tune. */
	if (polar_zero_buffers == -1)
	{
		/* Request according to NBuffers, which is in [16, INT_MAX / 2) */
		polar_zero_buffers = 4;

		if (NBuffers >= 1024)
			polar_zero_buffers = 32;

		if (NBuffers >= 16384)
			polar_zero_buffers = 512;
	}

	/* 0 disables the zero buffer. */
	if (polar_zero_buffers == 0)
		return 0;

	polar_zero_buffer_size = polar_zero_buffers * BLCKSZ;

	return polar_zero_buffer_size + PG_IO_ALIGN_SIZE;
}

static void
polar_zero_buffer_init()
{
	bool		found;

	if (polar_zero_buffer_size == 0)
		return;

	polar_zero_buffer = (char *)
		TYPEALIGN(PG_IO_ALIGN_SIZE,
				  ShmemInitStruct("Zero Buffer Blocks",
								  polar_zero_buffer_size + PG_IO_ALIGN_SIZE,
								  &found));

	if (!found)
		MemSet(polar_zero_buffer, 0, polar_zero_buffer_size);
}

/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
void
InitBufferPool(void)
{
	bool		foundBufs,
				foundDescs,
				foundIOCV,
				foundBufCkpt;

	/* Align descriptors to a cacheline boundary. */
	BufferDescriptors = (BufferDescPadded *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDescPadded),
						&foundDescs);

	/* Align buffer pool on IO page size boundary. */
	BufferBlocks = (char *)
		TYPEALIGN(PG_IO_ALIGN_SIZE,
				  ShmemInitStruct("Buffer Blocks",
								  NBuffers * (Size) BLCKSZ + PG_IO_ALIGN_SIZE,
								  &foundBufs));

	/* Align condition variables to cacheline boundary. */
	BufferIOCVArray = (ConditionVariableMinimallyPadded *)
		ShmemInitStruct("Buffer IO Condition Variables",
						NBuffers * sizeof(ConditionVariableMinimallyPadded),
						&foundIOCV);

	/*
	 * The array used to sort to-be-checkpointed buffer ids is located in
	 * shared memory, to avoid having to allocate significant amounts of
	 * memory at runtime. As that'd be in the middle of a checkpoint, or when
	 * the checkpointer is restarted, memory allocation failures would be
	 * painful.
	 */
	CkptBufferIds = (CkptSortItem *)
		ShmemInitStruct("Checkpoint BufferIds",
						NBuffers * sizeof(CkptSortItem), &foundBufCkpt);

	if (foundDescs || foundBufs || foundIOCV || foundBufCkpt)
	{
		/* should find all of these, or none of them */
		Assert(foundDescs && foundBufs && foundIOCV && foundBufCkpt);
		/* note: this path is only taken in EXEC_BACKEND case */
	}
	else
	{
		int			i;

		/*
		 * Initialize all the buffer headers.
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);

			CLEAR_BUFFERTAG(buf->tag);

			pg_atomic_init_u32(&buf->state, 0);
			pg_atomic_init_u32(&buf->polar_redo_state, 0);
			buf->wait_backend_pgprocno = INVALID_PGPROCNO;

			buf->buf_id = i;

			/*
			 * Initially link all the buffers together as unused. Subsequent
			 * management of this list is done by freelist.c.
			 */
			buf->freeNext = i + 1;

#ifdef LOCKBUFHDR_DEBUG
			buf->lockbufhdr_pid = 0;
#endif

			LWLockInitialize(BufferDescriptorGetContentLock(buf),
							 LWTRANCHE_BUFFER_CONTENT);

			ConditionVariableInit(BufferDescriptorGetIOCV(buf));

			/* POLAR */
			buf->oldest_lsn = InvalidXLogRecPtr;
			buf->flush_next = POLAR_FLUSHNEXT_NOT_IN_LIST;
			buf->flush_prev = POLAR_FLUSHNEXT_NOT_IN_LIST;
			buf->copy_buffer = NULL;
			buf->recently_modified_count = 0;
			buf->polar_flags = 0;
		}

		/* Correct last entry of linked list */
		GetBufferDescriptor(NBuffers - 1)->freeNext = FREENEXT_END_OF_LIST;
	}

	/* Init other shared buffer-management stuff */
	StrategyInitialize(!foundDescs);

	/* POLAR: init flush list */
	polar_init_flush_list_ctl(!foundDescs);

	/* POLAR: init copy buffer pool */
	polar_init_copy_buffer_pool();

	/* POLAR: init global zero buffer */
	polar_zero_buffer_init();

	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 */
Size
BufferShmemSize(void)
{
	Size		size = 0;

	/* size of buffer descriptors */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDescPadded)));
	/* to allow aligning buffer descriptors */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of data pages */
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* size of stuff controlled by freelist.c */
	size = add_size(size, StrategyShmemSize());

	/* size of I/O condition variables */
	size = add_size(size, mul_size(NBuffers,
								   sizeof(ConditionVariableMinimallyPadded)));
	/* to allow aligning the above */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of checkpoint sort array in bufmgr.c */
	size = add_size(size, mul_size(NBuffers, sizeof(CkptSortItem)));

	/* POLAR: size of flush list */
	size = add_size(size, polar_flush_list_ctl_shmem_size());

	/* POLAR: add copy buffer shared memory size */
	size = add_size(size, polar_copy_buffer_shmem_size());

	/* POLAR: size of global zero buffer */
	size = add_size(size, polar_zero_buffer_shmem_size());

	return size;
}
