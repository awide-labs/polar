/*-------------------------------------------------------------------------
 *
 * polar_procpool.c
 *	  This list is used to manage free resources.
 *	  The resources are allocated as array items,
 *	  get the free item from polar_successor_list_pop
 *	  and call polar_successor_list_push when the item is released.
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
 *	  src/backend/storage/ipc/polar_procpool.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "pgstat.h"
#include "storage/polar_procpool.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/polar_bitpos.h"
#include "utils/polar_log.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timeout.h"

typedef struct proc_hold_task_t
{
	dlist_node	node;
	polar_task_node_t *task;
}			proc_hold_task_t;

typedef struct proc_repeat_task_t
{
	dlist_node	node;
	uint32		repeat_delay;
	polar_task_node_t *task;
}			proc_repeat_task_t;

typedef struct sub_task_ctl_t
{
	polar_task_sched_t *sched;
	Latch	   *dispatcher_latch;
	int32		sub_proc_id;
	dlist_head	hold_tasks;
	dlist_head	repeat_tasks;

	uint64		max_fetch_seq;
	uint32		fetch_task_idx;
	uint64		fetch_running_task;
	uint64		add_hold_task;
	uint64		remove_hold_task;
	uint64		add_repeat_task;
	uint64		remove_repeat_task;
}			sub_task_ctl_t;

#define NEXT_SCHED_PROC(ctl) \
	do { \
		(ctl)->dst_proc++; \
		if (unlikely((ctl)->dst_proc == (ctl)->sched->total_proc)) \
			(ctl)->dst_proc = 0; \
	} while (0)

#define TASK_HASH_SET_HEAD(ctl, hash_elem, node) \
	do { \
		char *p = (hash_elem) + (ctl)->task_tag_size; \
		memcpy(p, &(node), sizeof(polar_task_node_t *)); \
	} while (0)

#define TASK_HASH_GET_HEAD(ctl, hash_elem, head) \
	do { \
		char *p = (hash_elem) + (ctl)->task_tag_size; \
		memcpy(&(head), p, sizeof(polar_task_node_t *)); \
	} while (0)

#define TASK_QUEUE_IS_EMPTY(proc) ((proc)->task_head == proc->task_tail && !proc->ring_full)

Size
polar_calc_task_sched_shmem_size(Size parallel_num, Size task_node_size, Size task_queue_depth)
{
	Size		size = 0;

	if (parallel_num > 0)
	{
		POLAR_ASSERT_PANIC(task_queue_depth > 0);
		POLAR_ASSERT_PANIC(task_node_size > sizeof(polar_task_node_t));

		size = offsetof(polar_task_sched_t, task_nodes);

		size = add_size(size, mul_size(parallel_num, mul_size(task_node_size, task_queue_depth)));
	}

	return size;
}

/*
 * call if LockTimeout is set and LOCK_TIMEOUT is exceeded.
 * do nothing because sub task need wait to acquire lock
 */
static void
polar_sub_task_locktimeout_handler(void)
{
}

static void
polar_init_task_queue(polar_task_sched_t *sched)
{
	Size		i,
				total;
	char	   *p = (char *) (sched->task_nodes);

	total = sched->total_proc * sched->task_queue_depth;

	for (i = 0; i < total; i++)
	{
		polar_task_node_t *node = (polar_task_node_t *) p;

		POLAR_RESET_TASK_NODE(node);
		SpinLockInit(&node->lock);

		p += sched->task_node_size;
	}
}

polar_task_sched_t *
polar_create_proc_task_sched(const char *sched_name, Size parallel_num,
							 Size task_node_size, Size task_queue_depth, void *run_arg)
{
	bool		found;
	polar_task_sched_t *sched;
	Size		size;

	size = polar_calc_task_sched_shmem_size(parallel_num, task_node_size, task_queue_depth);
	sched = (polar_task_sched_t *) ShmemInitStruct(sched_name, size, &found);

	if (!found)
	{
		char	   *ptr = (char *) sched;

		MemSet(ptr, 0, size);
		strlcpy(sched->name, sched_name, POLAR_TASK_NAME_MAX_LEN);
		sched->total_proc = parallel_num;
		sched->task_node_size = task_node_size;
		sched->task_queue_depth = task_queue_depth;
		sched->run_arg = run_arg;
		sched->added_seq = 0;
		pg_atomic_init_u32(&sched->enable_shutdown, 0);
		pg_atomic_init_u64(&sched->finished_seq, 0);
	}

	return sched;
}

void
polar_sched_reg_handler(polar_task_sched_t *sched, polar_task_handle_startup task_startup,
						polar_task_handler task_handle,
						polar_task_handle_cleanup task_cleanup,
						polar_task_get_tag task_get_tag)
{
	sched->task_startup = task_startup;
	sched->task_handle = task_handle;
	sched->task_cleanup = task_cleanup;
	sched->task_tag = task_get_tag;
}

static void
polar_ring_task_nodes_init(polar_task_sched_ctl_t *ctl, int proc_num)
{
	polar_task_sched_t *sched = ctl->sched;
	polar_sub_proc_t *sub_proc = &ctl->sub_proc[proc_num];
	char	   *p = (char *) (sched->task_nodes);

	sub_proc->ring_task_nodes_begin = (polar_task_node_t *) (p + (proc_num * sched->task_node_size * sched->task_queue_depth));
	sub_proc->ring_task_nodes_end = POLAR_SCHED_TASK_POINT(sched, sub_proc->ring_task_nodes_begin, sched->task_queue_depth - 1);

	sub_proc->task_head = sub_proc->ring_task_nodes_begin;
	sub_proc->task_tail = sub_proc->ring_task_nodes_begin;
	sub_proc->ring_full = false;
}

polar_task_sched_ctl_t *
polar_create_task_sched_ctl(polar_task_sched_t *sched, Size task_tag_size, HashValueFunc hash, HashCompareFunc match)
{
	Size		size = offsetof(polar_task_sched_ctl_t, sub_proc) + sizeof(polar_sub_proc_t) * sched->total_proc;
	polar_task_sched_ctl_t *ctl = palloc0(size);
	Size		max_hash_elem = sched->total_proc * sched->task_queue_depth;
	HASHCTL		hash_ctl;
	int			i,
				hash_flag = HASH_ELEM;

	polar_init_task_queue(sched);
	sched->running_queue_head = NULL;

	ctl->running_task_head = ctl->running_task_tail = NULL;
	ctl->sched = sched;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = task_tag_size;
	hash_ctl.entrysize = task_tag_size + sizeof(polar_task_node_t *);

	if (hash)
	{
		hash_ctl.hash = hash;
		hash_flag |= HASH_FUNCTION;
	}
	else
	{
		hash_flag |= HASH_BLOBS;
	}

	if (match)
	{
		hash_ctl.match = match;
		hash_flag |= HASH_COMPARE;
	}

	ctl->task_hash = hash_create("PolarProcPool", max_hash_elem, &hash_ctl, hash_flag);

	for (i = 0; i < sched->total_proc; i++)
		polar_ring_task_nodes_init(ctl, i);

	ctl->task_tag_size = task_tag_size;
	polar_sched_enable_shutdown(sched, false);

	return ctl;
}

static void
sub_task_update_ps_display(int32 sub_id)
{
	const char *old_status;
	char	   *new_status;
	int			len;

	old_status = get_ps_display(&len);
	new_status = (char *) palloc0(len + 32 + 1);
	memcpy(new_status, old_status, len);
	sprintf(new_status + len, " subid=%d", sub_id);
	set_ps_display(new_status);
	pfree(new_status);
}

static polar_task_node_t *
fetch_waiting_task_from_repeat_list(sub_task_ctl_t * task_ctl)
{
	dlist_node *cur_node,
			   *next_node;
	polar_task_node_t *running_task = NULL;
	int			status;

	if (dlist_is_empty(&task_ctl->repeat_tasks))
		return NULL;

	cur_node = dlist_head_node(&task_ctl->repeat_tasks);

	while (cur_node != NULL)
	{
		proc_repeat_task_t *repeat_task = dlist_container(proc_repeat_task_t, node, cur_node);

		if (dlist_has_next(&task_ctl->repeat_tasks, cur_node))
			next_node = dlist_next_node(&task_ctl->repeat_tasks, cur_node);
		else
			next_node = NULL;

		status = POLAR_TASK_NODE_STATUS(repeat_task->task);

		if (unlikely(status != POLAR_TASK_NODE_RUNNING))
		{
			elog(PANIC, "Task status is incorrect in %s, expected POLAR_TASK_NODE_RUNNING, got %x, task_addr %p",
				 PG_FUNCNAME_MACRO, status, repeat_task->task);
		}

		if (repeat_task->repeat_delay == 0)
		{
			running_task = repeat_task->task;
			dlist_delete(cur_node);
			pfree(repeat_task);
			task_ctl->remove_repeat_task++;
			break;
		}
		else
			repeat_task->repeat_delay--;

		cur_node = next_node;
	}

	return running_task;
}

static polar_task_node_t *
fetch_waiting_task_from_hold_list(sub_task_ctl_t * task_ctl)
{
	dlist_node *cur_node,
			   *next_node;
	int			status;
	polar_task_node_t *running_task = NULL;

	if (dlist_is_empty(&task_ctl->hold_tasks))
		return NULL;

	cur_node = dlist_head_node(&task_ctl->hold_tasks);

	while (cur_node != NULL)
	{
		proc_hold_task_t *hold_task = dlist_container(proc_hold_task_t, node, cur_node);

		if (dlist_has_next(&task_ctl->hold_tasks, cur_node))
			next_node = dlist_next_node(&task_ctl->hold_tasks, cur_node);
		else
			next_node = NULL;

		status = POLAR_TASK_NODE_STATUS(hold_task->task);

		POLAR_ASSERT_PANIC(status == POLAR_TASK_NODE_HOLD || status == POLAR_TASK_NODE_RUNNING);

		if (status == POLAR_TASK_NODE_RUNNING)
		{
			running_task = hold_task->task;
			dlist_delete(cur_node);
			pfree(hold_task);
			task_ctl->remove_hold_task++;
			break;
		}

		cur_node = next_node;
	}

	return running_task;
}

static void
proc_add_hold_task(sub_task_ctl_t * task_ctl, polar_task_node_t *task)
{
	proc_hold_task_t *hold_task = palloc0(sizeof(proc_hold_task_t));

	hold_task->task = task;
	dlist_push_tail(&task_ctl->hold_tasks, &hold_task->node);

	task_ctl->add_hold_task++;
}

static void
proc_add_repeat_task(sub_task_ctl_t * task_ctl, polar_task_node_t *task)
{
	proc_repeat_task_t *repeat_task;

	POLAR_ASSERT_PANIC(POLAR_TASK_NODE_RUNNING == POLAR_TASK_NODE_STATUS(task));

	repeat_task = palloc0(sizeof(proc_repeat_task_t));
	repeat_task->task = task;
	repeat_task->repeat_delay = POLAR_TASK_MAX_REPEAT_DELAY_TIMES;
	dlist_push_head(&task_ctl->repeat_tasks, &repeat_task->node);

	task_ctl->add_repeat_task++;
}

static polar_task_node_t *
fetch_waiting_task_from_queue(sub_task_ctl_t * task_ctl)
{
	char	   *p = (char *) (task_ctl->sched->task_nodes);
	polar_task_node_t *dst_task = NULL;
	int			status;

	while (true)
	{
		if (task_ctl->fetch_task_idx >= task_ctl->sched->task_queue_depth)
			task_ctl->fetch_task_idx = 0;

		dst_task = (polar_task_node_t *) (p +
										  (task_ctl->sub_proc_id * task_ctl->sched->task_queue_depth + task_ctl->fetch_task_idx)
										  * task_ctl->sched->task_node_size);

		/*
		 * We fetch task from the ring queue, if add_seq == max_fetch_seq,
		 * then we reach the end of this queue
		 */
		if (dst_task->add_seq > task_ctl->max_fetch_seq)
		{
			/*
			 * POLAR: It must guarantee the read order of add_seq and
			 * task_status. Besides polar_sched_proc_add_task guarantee the
			 * write order of task_status and add_seq.
			 */
			pg_read_barrier();
			status = POLAR_TASK_NODE_STATUS(dst_task);

			if (status == POLAR_TASK_NODE_RUNNING)
			{
				task_ctl->fetch_task_idx++;
				task_ctl->fetch_running_task++;
				task_ctl->max_fetch_seq = dst_task->add_seq;

				return dst_task;
			}
			else if (status == POLAR_TASK_NODE_HOLD)
			{
				proc_add_hold_task(task_ctl, dst_task);
				task_ctl->fetch_task_idx++;
				task_ctl->max_fetch_seq = dst_task->add_seq;
			}
			else if (status != POLAR_TASK_NODE_IDLE)
			{
				elog(PANIC, "Got unexpect task status %x in %s, which task is %p",
					 status, PG_FUNCNAME_MACRO, dst_task);
			}
		}
		else
			break;
	}

	return NULL;
}

static polar_task_node_t *
fetch_waiting_task_node(sub_task_ctl_t * task_ctl)
{
	polar_task_node_t *task;

	if ((task = fetch_waiting_task_from_repeat_list(task_ctl)))
		return task;

	if ((task = fetch_waiting_task_from_hold_list(task_ctl)))
		return task;

	return fetch_waiting_task_from_queue(task_ctl);
}

static bool
proc_task_queue_is_full(polar_task_sched_ctl_t *ctl, uint32 sub_proc)
{
	polar_sub_proc_t *proc = &ctl->sub_proc[sub_proc];

	return proc->ring_full;
}

static PGPROC *
polar_sched_get_worker_proc(BackgroundWorkerHandle *handle)
{
	pid_t		pid;

	if (GetBackgroundWorkerPid(handle, &pid) == BGWH_STARTED)
	{
		POLAR_ASSERT_PANIC(pid > 0);

		return polar_search_proc(pid);
	}

	return NULL;
}

static int
polar_sched_get_next_proc(polar_task_sched_ctl_t *ctl)
{
	polar_task_sched_t *sched = ctl->sched;
	int			i;

	for (i = 0; i < sched->total_proc; i++)
	{
		int			proc_num = ctl->dst_proc;

		NEXT_SCHED_PROC(ctl);

		/*
		 * If proc is NULL then we will try to get its PGPROC base on
		 * BackgroundHandle. And if the return value is NULL, then this
		 * process is not started.
		 */
		if (unlikely(ctl->sub_proc[proc_num].proc == NULL))
		{
			ctl->sub_proc[proc_num].proc = polar_sched_get_worker_proc(ctl->sub_proc[proc_num].handle);

			if (ctl->sub_proc[proc_num].proc == NULL)
				continue;
		}

		if (!proc_task_queue_is_full(ctl, proc_num))
			return proc_num;
	}

	return -1;
}

static int
polar_sched_get_dst_proc(polar_task_sched_ctl_t *ctl, polar_task_node_t *node)
{
	int32		proc_num = -1;
	void	   *task_tag = ctl->sched->task_tag(node);
	polar_task_node_t *task_head = NULL;
	char	   *hash_elem;

	hash_elem = (char *) hash_search(ctl->task_hash, task_tag, HASH_FIND, NULL);

	if (hash_elem)
		TASK_HASH_GET_HEAD(ctl, hash_elem, task_head);

	if (task_head != NULL)
	{
		polar_task_node_t *task_tail;

		task_tail = (polar_task_node_t *) SHMQueuePrev(&task_head->depend_task, &task_head->depend_task, offsetof(polar_task_node_t, depend_task));

		if (!task_tail)
			task_tail = task_head;

		proc_num = POLAR_TASK_NODE_PROC(task_tail);

		if (proc_task_queue_is_full(ctl, proc_num))
		{
			polar_sched_remove_finished_task(ctl);

			if (proc_task_queue_is_full(ctl, proc_num))
				proc_num = -1;
		}
	}

	if (proc_num == -1)
		proc_num = polar_sched_get_next_proc(ctl);

	return proc_num;
}

static void
polar_sched_proc_advance_ring(polar_task_sched_ctl_t *ctl, polar_sub_proc_t *proc)
{
	polar_task_sched_t *sched = ctl->sched;

	if (unlikely(proc->ring_full))
		POLAR_SCHED_TASK_ADVANCE(sched, proc, proc->task_tail);

	POLAR_SCHED_TASK_ADVANCE(sched, proc, proc->task_head);
	proc->ring_full = (proc->task_head == proc->task_tail);
}

static void
polar_sched_proc_retreat_ring(polar_task_sched_ctl_t *ctl, polar_sub_proc_t *proc)
{
	polar_task_sched_t *sched = ctl->sched;

	proc->ring_full = false;
	POLAR_SCHED_TASK_ADVANCE(sched, proc, proc->task_tail);
}

static void
polar_notify_task_proc(polar_task_sched_ctl_t *ctl, polar_task_node_t *task)
{
	uint32		proc_num;
	PGPROC	   *proc;

	proc_num = POLAR_TASK_NODE_PROC(task);
	POLAR_ASSERT_PANIC(proc_num >= 0 && proc_num < ctl->sched->total_proc);
	proc = ctl->sub_proc[proc_num].proc;

	SetLatch(&proc->procLatch);
}

static uint32
polar_sched_add_task_hash_table(polar_task_sched_ctl_t *ctl, polar_task_node_t *node, polar_sub_proc_t *proc)
{
	polar_task_sched_t *sched = ctl->sched;
	void	   *task_tag = sched->task_tag(node);
	uint32		status = POLAR_TASK_NODE_RUNNING;
	bool		found;
	char	   *hash_elem;

	hash_elem = (char *) hash_search(ctl->task_hash, task_tag, HASH_ENTER, &found);

	/*
	 * In this hash table we save task_tag as key, and polar_task_node_t * as
	 * value
	 */
	if (!found)
	{
		POLAR_ASSERT_PANIC(!found);
		SHMQueueInit(&node->depend_task);
		TASK_HASH_SET_HEAD(ctl, hash_elem, node);
	}
	else
	{
		polar_task_node_t *task_head = NULL;
		polar_task_node_t *prev_node = NULL;

		TASK_HASH_GET_HEAD(ctl, hash_elem, task_head);
		SHMQueueInsertBefore(&task_head->depend_task, &node->depend_task);

		prev_node = (polar_task_node_t *) SHMQueuePrev(&node->depend_task, &node->depend_task, offsetof(polar_task_node_t, depend_task));

		SpinLockAcquire(&prev_node->lock);

		if (POLAR_TASK_NODE_STATUS(prev_node) != POLAR_TASK_NODE_FINISHED)
		{
			status = POLAR_TASK_NODE_HOLD;
			prev_node->next_latch = &proc->proc->procLatch;
			POLAR_UPDATE_TASK_NODE_STATUS(node, status);
			ctl->hold_task_num++;
		}

		SpinLockRelease(&prev_node->lock);
	}

	if (status == POLAR_TASK_NODE_RUNNING)
		POLAR_UPDATE_TASK_NODE_STATUS(node, status);

	ctl->added_task_num++;

	return status;
}

static void
polar_sched_add_running_queue(polar_task_sched_ctl_t *ctl, polar_task_node_t *node)
{
	if (ctl->running_task_head == NULL)
	{
		SHMQueueInit(&node->running_task);
		ctl->running_task_head = &node->running_task;
		ctl->running_task_tail = &node->running_task;

		SpinLockAcquire(&ctl->sched->lock);
		ctl->sched->running_queue_head = ctl->running_task_head;
		SpinLockRelease(&ctl->sched->lock);
	}
	else
	{
		SHMQueueInsertAfter(ctl->running_task_tail, &node->running_task);
		ctl->running_task_tail = &node->running_task;
	}
}

static polar_task_node_t *
polar_sched_proc_add_task(polar_task_sched_ctl_t *ctl, int32 dst_proc_num, polar_task_node_t *node)
{
	polar_task_sched_t *sched = ctl->sched;
	polar_sub_proc_t *proc = &ctl->sub_proc[dst_proc_num];
	polar_task_node_t *dst_node = proc->task_head;
	int			extend_offset = offsetof(polar_task_node_t, task_status) + sizeof(pg_atomic_uint32);

	memcpy((char *) dst_node + extend_offset, (char *) node + extend_offset, sched->task_node_size - extend_offset);

	SpinLockInit(&dst_node->lock);

	POLAR_TASK_NODE_INIT_STATUS(dst_node);
	POLAR_UPDATE_TASK_NODE_PROC(dst_node, dst_proc_num);

	pg_write_barrier();
	dst_node->add_seq = ++sched->added_seq;

	polar_sched_add_running_queue(ctl, dst_node);

	if (polar_sched_add_task_hash_table(ctl, dst_node, proc) == POLAR_TASK_NODE_RUNNING)
		polar_notify_task_proc(ctl, dst_node);

	polar_sched_proc_advance_ring(ctl, proc);
	proc->running_tasks_num++;

	return dst_node;
}


polar_task_node_t *
polar_sched_add_task(polar_task_sched_ctl_t *ctl, polar_task_node_t *task)
{
	polar_task_node_t *dst_node;
	int32		dst_proc;

	dst_proc = polar_sched_get_dst_proc(ctl, task);

	if (dst_proc == -1)
	{
		ctl->fail_add_task_num++;
		return NULL;
	}

	dst_node = polar_sched_proc_add_task(ctl, dst_proc, task);

	return dst_node;
}

static void
sched_remove_from_running_queue(polar_task_sched_ctl_t *ctl, polar_task_node_t *task)
{
	if (ctl->running_task_tail == &task->running_task)
	{
		if (SHMQueueEmpty(&task->running_task))
			ctl->running_task_tail = NULL;
		else
			ctl->running_task_tail = task->running_task.prev;
	}

	if (ctl->running_task_head == &task->running_task)
	{
		if (SHMQueueEmpty(&task->running_task))
			ctl->running_task_head = NULL;
		else
			ctl->running_task_head = task->running_task.next;

		SpinLockAcquire(&ctl->sched->lock);
		ctl->sched->running_queue_head = ctl->running_task_head;
		SpinLockRelease(&ctl->sched->lock);
	}

	SHMQueueDelete(&task->running_task);
}

static void
sched_proc_mark_finished_tasks_removed(polar_task_sched_ctl_t *ctl, polar_task_node_t *task)
{
	polar_task_sched_t *sched = ctl->sched;
	void	   *task_tag = sched->task_tag(task);
	char	   *hash_elem = (char *) hash_search(ctl->task_hash, task_tag, HASH_FIND, NULL);
	polar_task_node_t *task_head,
			   *prev_task,
			   *next_task;
	bool		done = false;

	TASK_HASH_GET_HEAD(ctl, hash_elem, task_head);

	prev_task = task_head;

	/*
	 * If task is finished, then its previous dependent task should be
	 * finished too
	 */
	do
	{
		uint32		prev_status;

		prev_status = POLAR_TASK_NODE_STATUS(prev_task);

		if (unlikely(prev_status != POLAR_TASK_NODE_FINISHED))
			elog(PANIC, "Previous task %p status=%x is incorrect ,expect POLAR_TASK_NODE_FINISHED, task_head %p", prev_task, prev_status, task_head);

		/* Call dispatcher registerd function to handle the finished task */
		if (ctl->handle_finished)
			ctl->handle_finished(prev_task, ctl->finished_arg);

		sched_remove_from_running_queue(ctl, prev_task);

		next_task = (polar_task_node_t *) SHMQueueNext(&prev_task->depend_task,
													   &prev_task->depend_task, offsetof(polar_task_node_t, depend_task));

		if (prev_task == task)
		{
			if (next_task == NULL)
			{
				bool		found;

				hash_search(ctl->task_hash, task_tag, HASH_REMOVE, &found);
				POLAR_ASSERT_PANIC(found);
			}
			else
				TASK_HASH_SET_HEAD(ctl, hash_elem, next_task);

			done = true;
		}

		SHMQueueDelete(&prev_task->depend_task);
		POLAR_UPDATE_TASK_NODE_STATUS(prev_task, POLAR_TASK_NODE_REMOVED);
		prev_task = next_task;
		ctl->finished_task_num++;
	}
	while (!done);
}

static void
polar_sched_proc_set_tasks_removed(polar_task_sched_ctl_t *ctl)
{
	polar_task_sched_t *sched = ctl->sched;
	int			i;

	for (i = 0; i < sched->total_proc; i++)
	{
		polar_sub_proc_t *proc = &ctl->sub_proc[i];
		polar_task_node_t *task_finished;

		if (TASK_QUEUE_IS_EMPTY(proc))
			continue;

		task_finished = proc->task_tail;

		do
		{
			polar_task_node_t *task = task_finished;
			uint32		status;

			status = POLAR_TASK_NODE_STATUS(task);

			if (status == POLAR_TASK_NODE_FINISHED)
				sched_proc_mark_finished_tasks_removed(ctl, task);
			else if (status != POLAR_TASK_NODE_REMOVED)
				break;

			if (unlikely(task_finished == proc->task_head))
				break;

			POLAR_SCHED_TASK_ADVANCE(sched, proc, task_finished);
		}
		while (true);
	}
}

static int
polar_sched_proc_reset_removed_tasks(polar_task_sched_ctl_t *ctl)
{
	polar_task_sched_t *sched = ctl->sched;
	int			removed = 0;
	int			i;

	for (i = 0; i < sched->total_proc; i++)
	{
		polar_sub_proc_t *proc = &ctl->sub_proc[i];

		while (!TASK_QUEUE_IS_EMPTY(proc))
		{
			polar_task_node_t *task = proc->task_tail;

			if (POLAR_TASK_NODE_STATUS(task) != POLAR_TASK_NODE_REMOVED)
				break;

			POLAR_RESET_TASK_NODE(task);

			polar_sched_proc_retreat_ring(ctl, proc);
			removed++;
		}
	}

	ctl->reset_task_num += removed;
	return removed;
}

/*
 * Remove finished task from the running queue.
 * Return the number of removed finished task in this cycle
 */
int
polar_sched_remove_finished_task(polar_task_sched_ctl_t *ctl)
{
	polar_sched_proc_set_tasks_removed(ctl);
	return polar_sched_proc_reset_removed_tasks(ctl);
}

static bool
cleanup_task(polar_task_sched_t *sched)
{
	if (sched->task_cleanup)
		return sched->task_cleanup(sched->run_arg);

	return true;
}

void
polar_sub_task_main(Datum main_arg)
{
	sub_task_ctl_t task_ctl;
	Latch	   *dispatcher_latch;

	MemSet(&task_ctl, 0, sizeof(sub_task_ctl_t));

	task_ctl.sched = (polar_task_sched_t *) main_arg;
	dispatcher_latch = &(task_ctl.sched->dispatcher->procLatch);

	memcpy(&task_ctl.sub_proc_id, MyBgworkerEntry->bgw_extra, sizeof(int32));

	POLAR_ASSERT_PANIC(task_ctl.sub_proc_id >= 0 && task_ctl.sub_proc_id < task_ctl.sched->total_proc);
	POLAR_ASSERT_PANIC(MyProc != NULL && MyLatch == &MyProc->procLatch);

	task_ctl.fetch_task_idx = 0;
	dlist_init(&task_ctl.hold_tasks);
	dlist_init(&task_ctl.repeat_tasks);
	sub_task_update_ps_display(task_ctl.sub_proc_id);

	CurrentResourceOwner = ResourceOwnerCreate(NULL, "parallel proc pool");
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "parallel proc pool",
												 ALLOCSET_DEFAULT_SIZES);

	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);

	if (task_ctl.sched->task_startup)
		task_ctl.sched->task_startup(task_ctl.sched->run_arg);

	BackgroundWorkerUnblockSignals();

	/* register timeout handler, timeout will be enabled in ProcSleep */
	RegisterTimeout(DEADLOCK_TIMEOUT, CheckDeadLockAlert);
	RegisterTimeout(LOCK_TIMEOUT, polar_sub_task_locktimeout_handler);
	RegisterTimeout(STANDBY_DEADLOCK_TIMEOUT, StandbyDeadLockHandler);
	RegisterTimeout(STANDBY_LOCK_TIMEOUT, StandbyLockTimeoutHandler);

	SetLatch(dispatcher_latch);

	elog(LOG, "PolarDB proc pool start subprocess %d for %s", task_ctl.sub_proc_id, task_ctl.sched->name);

	while (true)
	{
		int			rc = 0;
		polar_task_node_t *task;

		/*
		 * Exit this process if we get sigterm signal and parallel schedule
		 * enable shutdown. Processes may have order to exit, we use
		 * cleanup_task to control it
		 */
		if (unlikely(ShutdownRequestPending) && polar_sched_shutdown_enabled(task_ctl.sched))
		{
			if (cleanup_task(task_ctl.sched))
				break;
		}

		if (unlikely(ConfigReloadPending))
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		task = fetch_waiting_task_node(&task_ctl);

		if (task != NULL)
		{
			polar_task_node_t *next_task = NULL;
			Latch	   *next_latch = NULL;

			if (!task_ctl.sched->task_handle(task_ctl.sched, task))
			{
				proc_add_repeat_task(&task_ctl, task);
				continue;
			}

			task->finished_seq = pg_atomic_fetch_add_u64(&(task_ctl.sched->finished_seq), 1);

			SpinLockAcquire(&task->lock);
			next_latch = task->next_latch;

			if (next_latch)
			{
				next_task = (polar_task_node_t *) SHMQueueNext(&task->depend_task, &task->depend_task,
															   offsetof(polar_task_node_t, depend_task));
			}

			POLAR_UPDATE_TASK_NODE_STATUS(task, POLAR_TASK_NODE_FINISHED);
			SpinLockRelease(&task->lock);

			if (next_latch)
			{
				POLAR_ASSERT_PANIC(next_task != NULL);
				POLAR_ASSERT_PANIC(POLAR_TASK_NODE_STATUS(next_task) == POLAR_TASK_NODE_HOLD);

				POLAR_UPDATE_TASK_NODE_STATUS(next_task, POLAR_TASK_NODE_RUNNING);

				if (next_latch != MyLatch)
					SetLatch(next_latch);
			}

			SetLatch(dispatcher_latch);
		}
		else
		{
			int			evt = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH;
			int			timeout = -1;

			if (!dlist_is_empty(&task_ctl.repeat_tasks))
			{
				evt |= WL_TIMEOUT;
				timeout = 10;	/* ms */
			}

			rc = WaitLatch(MyLatch, evt, timeout, WAIT_EVENT_POLAR_SUB_TASK_MAIN);

			if (rc & WL_LATCH_SET)
				ResetLatch(MyLatch);
		}
	}

	elog(LOG, "PolarDB proc pool exit subprocess %d for %s, fetch_running_task=%ld, add_hold_task=%ld, remove_hold_task=%ld, add_repeat_task=%ld, remove_repeat_task=%ld",
		 task_ctl.sub_proc_id, task_ctl.sched->name,
		 task_ctl.fetch_running_task, task_ctl.add_hold_task, task_ctl.remove_hold_task,
		 task_ctl.add_repeat_task, task_ctl.remove_repeat_task);

	/*
	 * From here on, elog(ERROR) should end with exit(1), not send control
	 * back to the sigsetjmp block above
	 */
	ExitOnAnyError = true;
	/* Normal exit from the bgwriter is here */
	proc_exit(0);				/* done */
}

static void
polar_reg_sub_task(polar_task_sched_ctl_t *ctl, uint32 i)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle = NULL;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_CRASH_ON_ERROR;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "polar_sub_task_main");
	worker.bgw_notify_pid = MyProcPid;
	memcpy(worker.bgw_extra, &i, sizeof(uint32));
	strlcpy(worker.bgw_name, ctl->sched->name, BGW_MAXLEN);
	strlcpy(worker.bgw_type, ctl->sched->name, BGW_MAXLEN);
	worker.bgw_main_arg = (Datum) (ctl->sched);

	RegisterDynamicBackgroundWorker(&worker, &handle);

	POLAR_ASSERT_PANIC(handle != NULL);

	ctl->sub_proc[i].handle = handle;
	ctl->sub_proc[i].proc = NULL;
}

void
polar_start_proc_pool(polar_task_sched_ctl_t *ctl)
{
	uint32		i;
	polar_task_sched_t *sched = ctl->sched;

	if (sched->dispatcher == NULL)
		sched->dispatcher = MyProc;

	POLAR_ASSERT_PANIC(ctl->sched->dispatcher != NULL);

	for (i = 0; i < sched->total_proc; i++)
		polar_reg_sub_task(ctl, i);
}

void
polar_release_task_sched_ctl(polar_task_sched_ctl_t *ctl)
{
	int			i;
	Size		size,
				task_nodes_size;
	polar_task_sched_t *sched = ctl->sched;
	char	   *task_nodes_ptr = ((char *) sched) + offsetof(polar_task_sched_t, task_nodes);

	polar_sched_enable_shutdown(ctl->sched, true);

	for (i = 0; i < ctl->sched->total_proc; i++)
	{
		BackgroundWorkerHandle *handle = ctl->sub_proc[i].handle;
		pid_t		pid;
		BgwHandleStatus status;

		status = GetBackgroundWorkerPid(handle, &pid);

		if (status != BGWH_STOPPED)
		{
			TerminateBackgroundWorker(handle);
			polar_wait_bg_worker_shutdown(handle, true);
		}

		pfree(handle);
	}

	elog(LOG, "Release polar_task_sched_ctl_t, added_task_num=%ld, fail_add_task_num=%ld, finished_task_num=%ld, hold_task_num=%ld, reset_task_num=%ld",
		 ctl->added_task_num, ctl->fail_add_task_num, ctl->finished_task_num, ctl->hold_task_num, ctl->reset_task_num);

	hash_destroy(ctl->task_hash);

	size = polar_calc_task_sched_shmem_size(sched->total_proc, sched->task_node_size, sched->task_queue_depth);
	task_nodes_size = size - offsetof(polar_task_sched_t, task_nodes);

	MemSet(task_nodes_ptr, 0, task_nodes_size);

	pfree(ctl);
}

void
polar_sched_ctl_reg_handler(polar_task_sched_ctl_t *ctl, polar_dispatcher_handle_finished handle_finished, void *finished_arg)
{
	ctl->handle_finished = handle_finished;
	ctl->finished_arg = finished_arg;
}
