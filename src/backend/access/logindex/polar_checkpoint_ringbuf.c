/*-------------------------------------------------------------------------
 *
 * polar_checkpoint_ringbuf.c
 *	  checkpoint ringbuf for parallel replay in standby mode.
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
 *	  src/backend/access/logindex/polar_checkpoint_ringbuf.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/pg_control.h"

#include "access/polar_logindex_redo.h"
#include "utils/elog.h"

#define POLAR_CHECKPOINT_RINGBUF_EMPTY(rbuf) ((rbuf)->head == (rbuf)->tail)
#define POLAR_CHECKPOINT_RINGBUF_FULL(rbuf) ((rbuf)->head == (((rbuf)->tail + 1) % (rbuf)->size))

#define CHECKPOINT_RINGBUF_FORMAT "checkpoint ringbuf size:%d, pos:[%d,%d), stats:(found %ld, unfound %ld, add %ld, evict %ld)"
#define CHECKPOINT_RINGBUF_ARGS(rbuf) (rbuf)->size, (rbuf)->head, (rbuf)->tail, (rbuf)->found_count, \
									  (rbuf)->unfound_count, (rbuf)->add_count, (rbuf)->evict_count

#define POLAR_CHECKPOINT_RINGBUF_DEFAULT_SIZE (1024)

static polar_checkpoint_ringbuf_t checkpoint_rbuf = (polar_checkpoint_ringbuf_t)
{
	.size = 0
};

void
polar_checkpoint_ringbuf_init(polar_checkpoint_ringbuf checkpoint_rbuf, int size)
{
	checkpoint_rbuf->recptrs = palloc0(sizeof(XLogRecPtr) * size);
	checkpoint_rbuf->endptrs = palloc0(sizeof(XLogRecPtr) * size);
	checkpoint_rbuf->checkpoints = palloc0(sizeof(CheckPoint) * size);
	checkpoint_rbuf->head = 0;
	checkpoint_rbuf->tail = 0;
	checkpoint_rbuf->size = size;
	checkpoint_rbuf->found_count = 0;
	checkpoint_rbuf->unfound_count = 0;
	checkpoint_rbuf->add_count = 0;
	checkpoint_rbuf->evict_count = 0;
}

void
polar_checkpoint_ringbuf_free(polar_checkpoint_ringbuf checkpoint_rbuf)
{
	if (checkpoint_rbuf->recptrs != NULL)
		pfree(checkpoint_rbuf->recptrs);

	if (checkpoint_rbuf->endptrs != NULL)
		pfree(checkpoint_rbuf->endptrs);

	if (checkpoint_rbuf->checkpoints != NULL)
		pfree(checkpoint_rbuf->checkpoints);

	if (checkpoint_rbuf->size > 0)
		MemSet(checkpoint_rbuf, 0, sizeof(polar_checkpoint_ringbuf_t));
}

/*
 * Pop a checkpoint from the head of the stash checkpoint array.
 *
 * It will remove the ckpt array head utill there is only one ckpt whose redo_lsn
 * is less than bg_replayed_lsn and return it. It will return InvalidXLogRecPtr and
 * zero CheckPoint if there's no one ckpt whose redo_lsn is less than bg_replayed_lsn.
 */
void
polar_checkpoint_ringbuf_pop(polar_checkpoint_ringbuf checkpoint_rbuf,
							 XLogRecPtr *checkpointRecPtr,
							 XLogRecPtr *checkpointEndPtr,
							 CheckPoint *checkpoint)
{
	XLogRecPtr	bg_replayed_lsn;

	Assert(checkpoint_rbuf != NULL);

	*checkpointRecPtr = InvalidXLogRecPtr;
	*checkpointEndPtr = InvalidXLogRecPtr;
	MemSet(checkpoint, 0, sizeof(CheckPoint));

	bg_replayed_lsn = polar_bg_redo_get_replayed_lsn(polar_logindex_redo_instance);
	while (!POLAR_CHECKPOINT_RINGBUF_EMPTY(checkpoint_rbuf))
	{
		if (checkpoint_rbuf->checkpoints[checkpoint_rbuf->head].redo > bg_replayed_lsn)
			break;

		*checkpointRecPtr = checkpoint_rbuf->recptrs[checkpoint_rbuf->head];
		*checkpointEndPtr = checkpoint_rbuf->endptrs[checkpoint_rbuf->head];
		memcpy(checkpoint, &checkpoint_rbuf->checkpoints[checkpoint_rbuf->head], sizeof(CheckPoint));

		checkpoint_rbuf->head = (checkpoint_rbuf->head + 1) % checkpoint_rbuf->size;
	}

	if (XLogRecPtrIsInvalid(*checkpointRecPtr))
		checkpoint_rbuf->unfound_count++;
	else
		checkpoint_rbuf->found_count++;
}

/*
 * Push a checkpoint at the tail of the stash checkpoint array.
 * The redo of checkpoints in the ringbuf must be incremental.
 *
 * It will evict a ckpt if the array is full.
 */
bool
polar_checkpoint_ringbuf_push(polar_checkpoint_ringbuf checkpoint_rbuf,
							  XLogRecPtr checkpointRecPtr,
							  XLogRecPtr checkpointEndPtr,
							  const CheckPoint *checkpoint)
{
	int			target;

	Assert(checkpoint_rbuf != NULL);

	/* check incoming redo value only when ringbuf is not empty. */
	if (!POLAR_CHECKPOINT_RINGBUF_EMPTY(checkpoint_rbuf))
	{
		int			prev = checkpoint_rbuf->tail;

		prev = (prev > 0) ? (prev - 1) : (checkpoint_rbuf->size - 1);
		if (checkpoint_rbuf->recptrs[prev] >= checkpointRecPtr ||
			checkpoint_rbuf->checkpoints[prev].redo > checkpoint->redo)
		{
			ereport(LOG,
					errmsg("%s: skip CheckPoint whose redo(0%X/%X) or RecPtr(%X/%X) is less" \
						   " than or equal to the tail one's redo(%X/%X) or RecPtr(%X/%X). " \
						   CHECKPOINT_RINGBUF_FORMAT,
						   PG_FUNCNAME_MACRO, LSN_FORMAT_ARGS(checkpoint->redo),
						   LSN_FORMAT_ARGS(checkpointRecPtr),
						   LSN_FORMAT_ARGS(checkpoint_rbuf->checkpoints[prev].redo),
						   LSN_FORMAT_ARGS(checkpoint_rbuf->recptrs[prev]),
						   CHECKPOINT_RINGBUF_ARGS(checkpoint_rbuf)));
			return false;
		}
	}

	/*
	 * Evict a checkpoint if the array is full. Current eviction strategy is
	 * to evict the last ckpt.
	 *
	 * TODO: A better strategy is to evict a ckpt to keep the redo distance
	 * between every two adjacent ckpts balanced. But it also incurs extra
	 * ckpt copy overhead. It's a tradeoff here.
	 */
	target = checkpoint_rbuf->tail;
	if (!POLAR_CHECKPOINT_RINGBUF_FULL(checkpoint_rbuf))
		checkpoint_rbuf->tail = (checkpoint_rbuf->tail + 1) % checkpoint_rbuf->size;
	else
	{
		target = (target > 0) ? (target - 1) : (checkpoint_rbuf->size - 1);
		checkpoint_rbuf->evict_count++;
	}

	/* Assign the checkpoint to the target posistion */
	checkpoint_rbuf->recptrs[target] = checkpointRecPtr;
	checkpoint_rbuf->endptrs[target] = checkpointEndPtr;
	memcpy(&checkpoint_rbuf->checkpoints[target], checkpoint, sizeof(CheckPoint));
	checkpoint_rbuf->add_count++;
	return true;
}

void
polar_checkpoint_ringbuf_check(XLogRecPtr *checkpointRecPtr, XLogRecPtr *checkpointEndPtr,
							   CheckPoint *checkpoint)
{
	bool		pushed;

	if (XLogRecPtrIsInvalid(*checkpointRecPtr) || XLogRecPtrIsInvalid(*checkpointEndPtr))
		return;
	POLAR_ASSERT_PANIC(checkpoint != NULL && !XLogRecPtrIsInvalid(checkpoint->redo));

	if (unlikely(checkpoint_rbuf.size == 0))
		polar_checkpoint_ringbuf_init(&checkpoint_rbuf, POLAR_CHECKPOINT_RINGBUF_DEFAULT_SIZE);

	pushed = polar_checkpoint_ringbuf_push(&checkpoint_rbuf, *checkpointRecPtr, *checkpointEndPtr, checkpoint);
	polar_checkpoint_ringbuf_pop(&checkpoint_rbuf, checkpointRecPtr, checkpointEndPtr, checkpoint);

	if ((pushed && ((checkpoint_rbuf.add_count % 16) == 0)) ||
		unlikely(polar_trace_logindex_messages <= DEBUG1))
		ereport(LOG, errmsg("Add CheckPoint [%X/%X, %X/%X) with redo %X/%X. " CHECKPOINT_RINGBUF_FORMAT,
							LSN_FORMAT_ARGS(*checkpointRecPtr), LSN_FORMAT_ARGS(*checkpointEndPtr),
							LSN_FORMAT_ARGS(checkpoint->redo), CHECKPOINT_RINGBUF_ARGS(&checkpoint_rbuf)));
}
