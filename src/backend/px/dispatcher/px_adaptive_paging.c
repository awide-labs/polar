/*-------------------------------------------------------------------------
 *
 * px_adaptive_paging.c
 *	  Dynamic paging between PX and QC.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
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
 *	  src/backend/px/dispatcher/px_adaptive_paging.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <assert.h>

#include "access/sdir.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "px/px_conn.h"
#include "px/px_adaptive_paging.h"
#include "px/px_util.h"
#include "px/px_vars.h"
#include "utils/builtins.h"

/* generator random error */
#ifdef FAULT_INJECTOR
#include "px/px_libpq_fault_injection.h"
#endif

/* POLAR px */
#ifdef HEAPDEBUGALL
#define PX_HEAPDEBUG_1 \
	elog(DEBUG5, "px_paging: worker_id: %d, request page. current_page: %u/%u, dir: %d, task_id: %d, round: %ld", \
		 seqReq->worker_id, seqReq->current_page, seqReq->page_count, \
		 seqReq->direction, seqReq->task_id, seqReq->scan_round)
#define PX_HEAPDEBUG_2 \
	elog(DEBUG5, "px_paging: worker_id: %d, get a page. %u:[%u-%u], task_id: %d, round: %ld", \
			seqReq->worker_id, seqReq->table_oid, seqRes.page_start, seqRes.page_end, \
			seqReq->task_id, seqReq->scan_round)
#define PX_HEAPDEBUG_3 \
	elog(DEBUG5, "px_paging: worker_id: %d, paging finish, task_id: %d, round: %ld", \
			seqReq->worker_id, seqReq->task_id, seqReq->scan_round)
#else
#define PX_HEAPDEBUG_1
#define PX_HEAPDEBUG_2
#define PX_HEAPDEBUG_3
#endif							/* !defined(HEAPDEBUGALL) */

/* struct for ip-binding paging */
typedef struct _NodePagingState
{
	bool		finish;
	int64_t		batch_size;		/* The count of unit number which px worker to
								 * read */
	/* header or tail may less than batch_size, so remember it */
	int64_t		header_unit_begin;	/* first slice begin */
	int64_t		header_unit_end;

	/*
	 * first slice end, abs(header_unit_end - header_unit_begin) = one unit
	 * size
	 */
	int64_t		tail_unit_begin;	/* last slice begin */
	int64_t		tail_unit_end;	/* last slice end */
	/* last slice end, abs(tail_unit_end - tail_unit_begin) = one unit size */
	int64_t		current_page;
} NodePagingState;

/* struct for dynamic paging state */
typedef struct _SeqscanPagingState
{
	int			task_id;
	int			direction;		/* 1 or -1 */
	int64_t		current_num;	/* current progress */
	int64_t		scan_start;		/* job`s start count */
	int64_t		scan_count;		/* job`s scan count */
	int64_t		page_count;		/* page count of relation */
	/* internal var */
	int			node_count;
	int64_t		scan_round;		/* may be more than 1 time */
	/* used for analyze */
	int			worker_count;	/* max worker_id + 1 */
	BlockNumber *pages_per_worker;	/* [page_count] */
	NodePagingState *node_states;
} SeqscanPagingState;

/* struct for paging state container */
typedef struct _PagingArray
{
	int			size;
	SeqscanPagingState **pStates;
} PagingArray;

static PagingArray seq_paging_array =
{
	0, NULL
};

static void px_init_adps_state_per_worker(SeqscanPagingState *p_state);

/* Add the state into PagingArray */
static void
px_adps_array_append(PagingArray *array, SeqscanPagingState *state)
{
	array->size += 1;
	array->pStates = (SeqscanPagingState **) realloc(array->pStates,
													 array->size * sizeof(SeqscanPagingState *));
	array->pStates[array->size - 1] = state;
}

void
px_adps_array_free(void)
{
	int			i;

	for (i = 0; i < seq_paging_array.size; i++)
	{
		if (seq_paging_array.pStates[i]->node_states)
			free(seq_paging_array.pStates[i]->node_states);
		/* Used for explain analyze */
		if (seq_paging_array.pStates[i]->pages_per_worker)
			free(seq_paging_array.pStates[i]->pages_per_worker);
		free(seq_paging_array.pStates[i]);
	}
	free(seq_paging_array.pStates);
	seq_paging_array.pStates = NULL;
	seq_paging_array.size = 0;
}

/*
 * Init the adaptive scan state from px's request.
 */
static SeqscanPagingState *
make_adps_state(SeqscanPageRequest *req)
{
	SeqscanPagingState *paging_state = (SeqscanPagingState *) malloc(sizeof(SeqscanPagingState));

	if (!paging_state)
		return NULL;
	paging_state->task_id = req->task_id;
	paging_state->direction = req->direction;
	if (req->direction == ForwardScanDirection) /* forward */
	{
		paging_state->current_num = 0;
		if (req->scan_count != InvalidBlockNumber)	/* part */
			paging_state->current_num = req->scan_start;
	}
	else
	{
		paging_state->current_num = req->page_count - 1;
		if (req->scan_count != InvalidBlockNumber)	/* part */
			paging_state->current_num = req->scan_start;
	}
	paging_state->scan_start = req->scan_start;
	paging_state->scan_count = req->scan_count;
	paging_state->page_count = req->page_count;

	paging_state->scan_round = req->scan_round;
	paging_state->node_count = 0;
	paging_state->node_states = NULL;

	/* analyze */
	paging_state->worker_count = 0;
	paging_state->pages_per_worker = NULL;
	return paging_state;
}

/*
 * Given a seqReq, return the dynamic task which belongs to the request.
 */
static bool
px_check_match_and_update_state(SeqscanPagingState *p_state, SeqscanPageRequest *seqReq, bool *has_finished)
{
	if (p_state->task_id != seqReq->task_id)
		return false;
	/* this round has finished */
	if (p_state->scan_round > seqReq->scan_round)
	{
		*has_finished = true;
		return true;
	}
	*has_finished = false;
	/* upgrade to next round */
	if (p_state->scan_round < seqReq->scan_round)
	{
		int			i;

		for (i = 0; i < p_state->node_count; i++)
		{
			if (!p_state->node_states[i].finish)
			{
				/* maybe error occur in paging */
				write_log("px_paging: error: round %ld has unfinished page", p_state->scan_round);
				break;
			}
		}
		p_state->scan_round++;
		/* must be one round ahead */
		assert(p_state->scan_round == seqReq->scan_round);
		/* reinit the paging state */
		px_init_adps_state_per_worker(p_state);
	}
	return true;
}

/*
 * If the current worker_id is less than worker_count, means this worker has been recorded in p_state,
 * we just need to update the page_count belongs to this worker.
 * Else, this is a new worker, we should realloc the pages_per_worker and init the newer ones.
 */
static void
px_adps_add_page_count(SeqscanPagingState *p_state, int worker_id, BlockNumber page_count)
{
	/* This worker is a new worker. */
	if (p_state->worker_count <= worker_id)
	{
		int			old_count = p_state->worker_count;

		p_state->worker_count = worker_id + 1;
		p_state->pages_per_worker = realloc(p_state->pages_per_worker, p_state->worker_count * sizeof(BlockNumber));

		/* Init the newer worker */
		while (old_count < p_state->worker_count)
			p_state->pages_per_worker[old_count++] = 0;
	}
	p_state->pages_per_worker[worker_id] += page_count;
}

/*
 * Init the adaptive scan state for each px workers
 */
static void
px_init_adps_state_per_worker(SeqscanPagingState *p_state)
{
	int			i;

	for (i = 0; i < p_state->node_count; i++)
	{
		int64_t		cached_unit_pages;
		NodePagingState *n_state = &p_state->node_states[i];

		n_state->finish = false;
		n_state->batch_size = px_scan_unit_size;

		/* How many units to get from all the workers once */
		cached_unit_pages = n_state->batch_size * p_state->node_count;
		{
			int64_t		start,
						end;
			int64_t		header_unit_page;
			int64_t		tail_unit_page;

			if (p_state->scan_count == InvalidBlockNumber)
			{
				/* Scan all the blocks */
				start = 0;
				end = p_state->page_count - 1;
			}
			else
			{
				/* Forward scan part of blocks */
				if (p_state->direction == ForwardScanDirection)
				{
					start = p_state->scan_start;
					end = p_state->scan_start + p_state->scan_count - 1;
					if (end >= p_state->page_count)
						end = p_state->page_count - 1;
				}
				/* Backward scan blocks */
				else
				{
					end = p_state->scan_start;
					start = p_state->scan_start - p_state->scan_count + 1;
					if (start < 0)
						start = 0;
				}
			}
			header_unit_page = cached_unit_pages * (int64_t) (start / cached_unit_pages);
			tail_unit_page = cached_unit_pages * (int64_t) (end / cached_unit_pages);

			n_state->tail_unit_begin = tail_unit_page + n_state->batch_size * i;
			n_state->tail_unit_end = tail_unit_page + n_state->batch_size * (i + 1) - 1;
			if (n_state->tail_unit_begin > end)
			{
				n_state->tail_unit_begin -= cached_unit_pages;
				n_state->tail_unit_end -= cached_unit_pages;
			}
			else if (n_state->tail_unit_end > end)
				n_state->tail_unit_end = end;
			if (n_state->tail_unit_end < start)
				n_state->finish = true;
			else if (n_state->tail_unit_begin < start)
				n_state->tail_unit_begin = start;

			n_state->header_unit_begin = header_unit_page + n_state->batch_size * i;
			n_state->header_unit_end = header_unit_page + n_state->batch_size * (i + 1) - 1;
			if (n_state->header_unit_end < start)
			{
				n_state->header_unit_begin += cached_unit_pages;
				n_state->header_unit_end += cached_unit_pages;
			}
			else if (n_state->header_unit_begin < start)
				n_state->header_unit_begin = start;
			if (n_state->header_unit_begin > end)
				n_state->finish = true;
			else if (n_state->header_unit_end > end)
				n_state->header_unit_end = end;
			if (p_state->direction == ForwardScanDirection)
				n_state->current_page = n_state->header_unit_begin;
			else
				n_state->current_page = n_state->tail_unit_end;
		}
	}
}

/* Change batch size when this is the last only slice */
static bool
px_adps_can_get_next_unit(SeqscanPagingState *p_state, int idx, int64_t * start, int64_t * end, bool current_is_last)
{
	NodePagingState *n_state = &p_state->node_states[idx];
	int			cached_unit_pages = n_state->batch_size * p_state->node_count;

	/* This px worker has run out all the slices, return */
	if (n_state->finish)
		return false;

	/* Forward scan */
	if (p_state->direction == ForwardScanDirection)
	{
		if (n_state->current_page == n_state->header_unit_begin)
		{
			*start = n_state->current_page;
			*end = n_state->header_unit_end;
			n_state->current_page = n_state->header_unit_end + cached_unit_pages - n_state->batch_size + 1;
		}
		else if (n_state->current_page >= n_state->tail_unit_begin)
		{
			if (current_is_last)
			{
				/* When this is the last only slice, make batch_size small */
				int			small_batch = n_state->batch_size / 16;

				/* But at least one page */
				if (small_batch < 1)
					small_batch = 1;
				*start = n_state->current_page;
				*end = n_state->current_page + small_batch - 1;
				/* And not large than tail */
				if (*end > n_state->tail_unit_end)
					*end = n_state->tail_unit_end;
				n_state->current_page += small_batch;
			}
			else
			{
				*start = n_state->current_page;
				*end = n_state->tail_unit_end;
				n_state->finish = true;
			}
		}
		else
		{
			*start = n_state->current_page;
			*end = n_state->current_page + n_state->batch_size - 1;
			n_state->current_page += cached_unit_pages;
		}
		if (n_state->current_page > n_state->tail_unit_end)
			n_state->finish = true;
	}
	/* Backward scan */
	else
	{
		if (n_state->current_page == n_state->header_unit_end)
		{
			*start = n_state->current_page;
			*end = n_state->header_unit_begin;
			n_state->finish = true;
		}
		else if (n_state->current_page == n_state->tail_unit_end)
		{
			*start = n_state->current_page;
			*end = n_state->tail_unit_begin;
			n_state->current_page = n_state->tail_unit_begin - cached_unit_pages + n_state->batch_size - 1;
		}
		else
		{
			*start = n_state->current_page;
			*end = n_state->current_page - n_state->batch_size + 1;
			n_state->current_page -= cached_unit_pages;
		}
		if (n_state->current_page < n_state->header_unit_begin)
			n_state->finish = true;
	}
	return true;
}

/*
 * Find the last only px workers, if it exists, a smaller batch scan will be happened.
 * Get the next unit in current px workers, if it gets one unit, use it. Else, scan
 * all the other px workers to find an avaiable unit.
 * Save the result of next unit into pRes.
 */
static bool
px_adps_get_next_scan_unit(SeqscanPagingState *p_state, int node_idx, SeqscanPageResponse *pRes)
{
	int			i;
	bool		found_next_unit;
	int			last_only_idx = -1,
				unfinished = 0;
	int64_t		page_start = -1,
				page_end = -1;

	/* Find the last unfinished px worker, and mark the last only index */
	for (i = 0; i < p_state->node_count; i++)
	{
		if (!p_state->node_states[i].finish)
		{
			unfinished++;
			last_only_idx = i;
		}
	}
	last_only_idx = (unfinished == 1) ? i : (-1);

	found_next_unit = px_adps_can_get_next_unit(p_state, node_idx, &page_start, &page_end,
												last_only_idx == node_idx);
	if (!found_next_unit)
	{
		/*
		 * May be first task is empty, scan the other tasks.
		 */
		for (i = 0; i < p_state->node_count; i++)
		{
			/* Skip the worker in the first task */
			if (i == node_idx)
				continue;

			/* Scan other tasks on each px workers */
			found_next_unit = px_adps_can_get_next_unit(p_state, i, &page_start, &page_end, last_only_idx == i);
			if (found_next_unit)
				break;
		}
	}

	/* Get the scan unit, update the result's page_start and page_end */
	pRes->page_start = (BlockNumber) page_start;
	pRes->page_end = (BlockNumber) page_end;
	return found_next_unit;
}

/*
 * Return the scan response from px's scan request. PX worker have run out
 * it's scan blocks from the last task, get response from qc to find a new
 * task.
 * Ip binding per px workers and cached friendly.
 */
SeqscanPageResponse
px_adps_get_response_block(SeqscanPageRequest *seqReq, int node_idx, int node_count)
{
	SeqscanPageResponse seqRes;

	seqRes.page_start = InvalidBlockNumber;
	seqRes.page_end = InvalidBlockNumber;
	seqRes.success = 0;
	if (node_idx < 0 || node_count <= 0)
	{
		write_log("px_paging: error node args when px_adps_get_response_block");
		return seqRes;
	}
	{
		int			i;
		bool		found = false;

		/*
		 * Init seq_paging_array is 0, so at the beginning of searching, it
		 * will miss.
		 */
		for (i = 0; i < seq_paging_array.size; i++)
		{
			bool		has_finished = false;
			SeqscanPagingState *p_state = seq_paging_array.pStates[i];

			if (px_check_match_and_update_state(p_state, seqReq, &has_finished))
			{
				/* This round has consumed by other workers */
				if (has_finished)
					return seqRes;

				found = true;

				/* Search all the nodes to find the next unit(or page) to read */
				if (px_adps_get_next_scan_unit(p_state, node_idx, &seqRes))
				{
					BlockNumber page_count;

					seqRes.success = 1;
					page_count = Abs((int64_t) seqRes.page_end - (int64_t) seqRes.page_start) + 1;
					px_adps_add_page_count(p_state, seqReq->worker_id, page_count);
				}
				break;
			}
		}

		/*
		 * Can not find a task matches the request task, init a new task and
		 * record it
		 */
		if (!found)
		{
			SeqscanPagingState *p_state = make_adps_state(seqReq);

			if (!p_state)
			{
				write_log("px_paging: memory not enough when px_adps_get_response_block");
				return seqRes;
			}
			/* init node state */
			p_state->node_count = node_count;
			p_state->node_states = (NodePagingState *) malloc(sizeof(NodePagingState) * p_state->node_count);
			if (!p_state->node_states)
			{
				free(p_state);
				write_log("px_paging: memory not enough when px_adps_get_response_block");
				return seqRes;
			}
			px_adps_array_append(&seq_paging_array, p_state);
			px_init_adps_state_per_worker(p_state);
			if (px_adps_get_next_scan_unit(p_state, node_idx, &seqRes))
			{
				BlockNumber page_count;

				seqRes.success = 1;
				page_count = Abs((int64_t) seqRes.page_end - (int64_t) seqRes.page_start) + 1;
				px_adps_add_page_count(p_state, seqReq->worker_id, page_count);
			}
		}
	}
	/* Fix the result, rs_nblocks may be different between px workers */
	if (seqRes.success)
	{
		if (ForwardScanDirection == seqReq->direction)
		{
			if (seqRes.page_start >= seqReq->page_count)
				seqRes.success = false;
			if (seqRes.page_end >= seqReq->page_count)
				seqRes.page_end = seqReq->page_count - 1;
		}
		else
		{
			/* Move to the tail page */
			if (seqRes.page_start >= seqReq->page_count)
				seqRes.page_start = seqReq->page_count - 1;

			/* Move to the head page */
			if (seqRes.page_end >= seqReq->page_count)
			{
				seqReq->current_page = seqRes.page_end;
				return px_adps_get_response_block(seqReq, node_idx, node_count);
			}
		}
	}
	return seqRes;
}

/* Init px adaptive scan array */
bool
px_adps_check_valid(void)
{
	/* Check the seq_paging_array's size is reset to zero */
	if (seq_paging_array.size != 0)
	{
		write_log("px_paging: check dynamic paging error");
		return false;
	}
	return true;
}

/* Get dynamic seqscan result for explain analyze, called in qc */
void
px_adps_analyze_result(int scan_id, char **p_msg_buf)
{
	int			i;
	SeqscanPagingState *found_pStates = NULL;

	if (!p_msg_buf)
		return;

	*p_msg_buf = NULL;
	for (i = 0; i < seq_paging_array.size; i++)
	{
		if (scan_id == seq_paging_array.pStates[i]->task_id)
		{
			found_pStates = seq_paging_array.pStates[i];
			break;
		}
	}

	if (found_pStates != NULL)
	{
		int			pos = 0;
		int			count = found_pStates->worker_count;
		int			max_size = count * 15 + 10;

		/* Use palloc when in main thread */
		char	   *tmp_msg_buf = (char *) palloc(max_size);

		Assert(count > 0);
		tmp_msg_buf[pos] = '[';
		pos++;
		for (i = 0; i < count; i++)
		{
			if (found_pStates->pages_per_worker[i] > 0)
				pos += snprintf(tmp_msg_buf + pos, max_size - pos, "%u,", found_pStates->pages_per_worker[i]);
		}
		tmp_msg_buf[pos - 1] = ']';
		tmp_msg_buf[pos] = '\0';
		*p_msg_buf = tmp_msg_buf;
	}
}

/*
 * PX worker has run out of it's scan unit, request qc to get a new scan unit.
 * Called in px workers.
 */
SeqscanPageResponse
px_adps_request_scan_unit(SeqscanPageRequest *seqReq)
{
	StringInfoData buffer;
	SeqscanPageResponse seqRes;
	int			request_size,
				response_size;
	int			code;
	char	   *encoded_msg;
	const char *msg_buffer;

	seqRes.success = 0;

	/* TODO: pg15_px: enable if after adps becomes available */
#if 0
	if (unlikely(polar_trace_heap_scan_flow))
		PX_HEAPDEBUG_1;
#endif

	request_size = sizeof(SeqscanPageRequest);
	encoded_msg = (char *) palloc(request_size * 2 + 1);
	hex_encode((const char *) seqReq, request_size, encoded_msg);
	encoded_msg[request_size * 2] = '\0';
	pq_beginmessage(&buffer, 'S');
	pq_sendstring(&buffer, MSG_ADAPTIVE_PAGING);
	pq_sendstring(&buffer, encoded_msg);
	pfree(encoded_msg);
	pq_endmessage_reuse(&buffer);
	/* Must flush this message */
	if (EOF == pq_flush())
	{
		elog(ERROR, "px_paging: can`t send paging response");
		goto finish;
	}
	response_size = sizeof(SeqscanPageResponse);
	pq_startmsgread();
	code = pq_getbyte();
	if (code == EOF)
	{
		elog(ERROR, "px_paging: can`t get paging response");
		goto finish;
	}
	if (code != PACKET_TYPE_PAGING)
	{
		elog(ERROR, "px_paging: get an error paging response, code:%d", code);
		goto finish;
	}
	if (EOF == pq_getmessage(&buffer, response_size + sizeof(int)))
	{
		elog(ERROR, "px_paging: can`t get paging response");
		goto finish;
	}
	msg_buffer = pq_getmsgbytes(&buffer, response_size);
	memcpy(&seqRes, msg_buffer, response_size);
	/* TODO: pg15_px: enable if after adps becomes available */
#if 0
	if (seqRes.success)
	{
		if (unlikely(polar_trace_heap_scan_flow))
			PX_HEAPDEBUG_2;
	}
	else
	{
		if (unlikely(polar_trace_heap_scan_flow))
			PX_HEAPDEBUG_3;
	}
#endif

finish:
	if (pq_is_reading_msg())
		pq_endmsgread();
	pfree(buffer.data);

#ifdef FAULT_INJECTOR
	if (seqReq->worker_id == 0)
	{
		elog(DEBUG5, "px_paging: px_worker %d, inject a scan delay fault", seqReq->worker_id);
		SIMPLE_FAULT_INJECTOR("px_adaptive_scan_round_delay");
	}
#endif
	return seqRes;
}
