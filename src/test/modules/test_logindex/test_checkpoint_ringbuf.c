/*-------------------------------------------------------------------------
 *
 * test_checkpoint_ringbuf.c
 *	  unit test for checkpoint ring buffer.
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
 *	  src/test/modules/test_logindex/test_checkpoint_ringbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "fmgr.h"
#include "catalog/pg_control.h"
#include "access/polar_logindex_redo.h"

#include "test_module_init.h"

PG_FUNCTION_INFO_V1(test_checkpoint_ringbuf);

static void
checkpoint_ringbuf_stat_assert(polar_checkpoint_ringbuf rbuf, uint64_t add,
							   uint64_t evict, uint64_t found, uint64_t unfound)
{
	Assert(rbuf->add_count == add);
	Assert(rbuf->evict_count == evict);
	Assert(rbuf->found_count == found);
	Assert(rbuf->unfound_count == unfound);
}

static void
checkpoint_ringbuf_lsn_assert(polar_checkpoint_ringbuf rbuf, int index,
							  XLogRecPtr start, XLogRecPtr end, XLogRecPtr redo)
{
	Assert(rbuf->recptrs[index] == start);
	Assert(rbuf->endptrs[index] == end);
	Assert(rbuf->checkpoints[index].redo == redo);
}

Datum
test_checkpoint_ringbuf(PG_FUNCTION_ARGS)
{
	polar_checkpoint_ringbuf_t ringbuf;
	int			i,
				total = 0;
	XLogRecPtr	first_lsn = 1;
	XLogRecPtr	start_lsn = first_lsn,
				end_lsn = first_lsn,
				gap = 1;
	CheckPoint	checkpoint = {.redo = first_lsn};
	XLogRecPtr	pop_start_lsn,
				pop_end_lsn;
	CheckPoint	pop_checkpoint;
	XLogRecPtr	old_bg_replayed_lsn = polar_bg_redo_get_replayed_lsn(polar_logindex_redo_instance);
	int			used,
				ret;

	polar_checkpoint_ringbuf_init(&ringbuf, 1024);

	Assert(ringbuf.size == 1024);
	Assert(ringbuf.recptrs && ringbuf.endptrs && ringbuf.checkpoints);
	Assert(ringbuf.add_count == 0 && ringbuf.head == ringbuf.tail);
	Assert(polar_is_primary());

	polar_checkpoint_ringbuf_pop(&ringbuf, &pop_start_lsn, &pop_end_lsn, &pop_checkpoint);
	Assert(XLogRecPtrIsInvalid(pop_start_lsn));
	Assert(XLogRecPtrIsInvalid(pop_end_lsn));
	Assert(XLogRecPtrIsInvalid(pop_checkpoint.redo));
	checkpoint_ringbuf_stat_assert(&ringbuf, 0, 0, 0, 1);

	/*
	 * set bg_replayed_lsn to make sure the result of pop from checkpoint
	 * ringbuf
	 */
	polar_bg_redo_set_replayed_lsn(polar_logindex_redo_instance, 0);

	for (i = 0; i < ringbuf.size / 3; i++)
	{
		polar_checkpoint_ringbuf_push(&ringbuf, start_lsn, end_lsn, &checkpoint);
		start_lsn += gap;
		end_lsn += gap;
		checkpoint.redo += gap;
		total++;
	}
	checkpoint_ringbuf_stat_assert(&ringbuf, total, 0, 0, 1);

	for (i = 0; i < ringbuf.size; i++)
	{
		polar_checkpoint_ringbuf_push(&ringbuf, start_lsn, end_lsn, &checkpoint);
		start_lsn += gap;
		end_lsn += gap;
		checkpoint.redo += gap;
		total++;
	}
	checkpoint_ringbuf_stat_assert(&ringbuf, total, ringbuf.size / 3 + 1, 0, 1);
	Assert(ringbuf.head == 0 && ringbuf.tail == ringbuf.size - 1);
	/* check head CheckPoint */
	checkpoint_ringbuf_lsn_assert(&ringbuf, ringbuf.head, first_lsn, first_lsn, first_lsn);
	/* check tail CheckPoint */
	checkpoint_ringbuf_lsn_assert(&ringbuf, ringbuf.tail - 1, start_lsn - gap,
								  end_lsn - gap, checkpoint.redo - gap);
	/* check previous CheckPoint before tail */
	checkpoint_ringbuf_lsn_assert(&ringbuf, ringbuf.tail - 2, first_lsn + (ringbuf.size - 3) * gap,
								  first_lsn + (ringbuf.size - 3) * gap, first_lsn + (ringbuf.size - 3) * gap);

	/* bg_replayed_lsn is too small */
	for (i = 0; i < ringbuf.size / 3; i++)
	{
		polar_checkpoint_ringbuf_pop(&ringbuf, &pop_start_lsn, &pop_end_lsn, &pop_checkpoint);
		Assert(XLogRecPtrIsInvalid(pop_start_lsn));
		Assert(XLogRecPtrIsInvalid(pop_end_lsn));
		Assert(XLogRecPtrIsInvalid(pop_checkpoint.redo));
	}
	checkpoint_ringbuf_stat_assert(&ringbuf, total, ringbuf.size / 3 + 1, 0, ringbuf.size / 3 + 1);

	polar_bg_redo_set_replayed_lsn(polar_logindex_redo_instance, first_lsn + ringbuf.size / 3);

	for (i = 0; i < ringbuf.size; i++)
	{
		polar_checkpoint_ringbuf_pop(&ringbuf, &pop_start_lsn, &pop_end_lsn, &pop_checkpoint);
		if (i == 0)
		{
			Assert(pop_start_lsn == first_lsn + ringbuf.size / 3);
			Assert(pop_end_lsn == first_lsn + ringbuf.size / 3);
			Assert(pop_checkpoint.redo == first_lsn + ringbuf.size / 3);
		}
		else
		{
			Assert(XLogRecPtrIsInvalid(pop_start_lsn));
			Assert(XLogRecPtrIsInvalid(pop_end_lsn));
			Assert(XLogRecPtrIsInvalid(pop_checkpoint.redo));
		}
	}
	Assert(ringbuf.tail - ringbuf.head == ringbuf.size - 2 - ringbuf.size / 3);
	checkpoint_ringbuf_stat_assert(&ringbuf, total, ringbuf.size / 3 + 1, 1, ringbuf.size + ringbuf.size / 3);

	elog(LOG, "%d, %d", ringbuf.head, ringbuf.tail);

	used = ringbuf.tail - ringbuf.head;
	for (i = 0; i < ringbuf.size; i++)
	{
		polar_checkpoint_ringbuf_push(&ringbuf, start_lsn, end_lsn, &checkpoint);
		start_lsn += gap;
		end_lsn += gap;
		checkpoint.redo += gap;
		total++;
	}
	/* full checkpoint ringbuf */
	Assert(ringbuf.tail + 1 == ringbuf.head);
	checkpoint_ringbuf_stat_assert(&ringbuf, total, ringbuf.size / 3 + 2 + used, 1, ringbuf.size + ringbuf.size / 3);

	/* insert repeated CheckPoint */
	start_lsn -= gap;
	end_lsn -= gap;
	checkpoint.redo -= gap;
	ret = polar_checkpoint_ringbuf_push(&ringbuf, start_lsn, end_lsn, &checkpoint);
	Assert(ret == false);
	checkpoint_ringbuf_stat_assert(&ringbuf, total, ringbuf.size / 3 + 2 + used, 1, ringbuf.size + ringbuf.size / 3);

	/* make bg_replayed_lsn advanced */
	polar_bg_redo_set_replayed_lsn(polar_logindex_redo_instance, checkpoint.redo);

	for (i = 0; i < ringbuf.size; i++)
	{
		polar_checkpoint_ringbuf_pop(&ringbuf, &pop_start_lsn, &pop_end_lsn, &pop_checkpoint);
		if (i == 0)
		{
			Assert(pop_start_lsn == start_lsn);
			Assert(pop_end_lsn == end_lsn);
			Assert(pop_checkpoint.redo == checkpoint.redo);
		}
		else
		{
			Assert(XLogRecPtrIsInvalid(pop_start_lsn));
			Assert(XLogRecPtrIsInvalid(pop_end_lsn));
			Assert(XLogRecPtrIsInvalid(pop_checkpoint.redo));
		}
	}
	/* empty checkpoint ringbuf */
	Assert(ringbuf.tail == ringbuf.head);
	checkpoint_ringbuf_stat_assert(&ringbuf, total, ringbuf.size / 3 + 2 + used, 2, ringbuf.size * 2 + ringbuf.size / 3 - 1);

	/* rollback bg_replayed_lsn */
	polar_bg_redo_set_replayed_lsn(polar_logindex_redo_instance, old_bg_replayed_lsn);
	polar_checkpoint_ringbuf_free(&ringbuf);

	PG_RETURN_INT32(0);
}
