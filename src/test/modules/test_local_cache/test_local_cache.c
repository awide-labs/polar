/*-------------------------------------------------------------------------
 *
 * test_local_cache.c
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
 *	  src/test/modules/test_local_cache/test_local_cache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>
#include <unistd.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/polar_fd.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/polar_local_cache.h"
#include "utils/polar_log.h"

#ifdef Assert
#undef Assert
#endif
#define Assert(condition) POLAR_ASSERT_PANIC(condition)

#define LOCAL_TEST_DIR "local_cache_local_dir"

#define BLOCK_SIZE (8192)
#define BLOCK_PER_SEGMENT (32)
#define LOOP_MAX_SEGMENT  (3)
#define MAX_SEGMENTS (5)

#define LOCAL_CACHE_STAT_INIT (0)
#define LOCAL_CACHE_STAT_START (1)
#define LOCAL_CACHE_STAT_STOP (2)

void		_PG_init(void);
void		test_local_cache_write_worker(Datum main_arg);
void		test_local_cache_read_worker(Datum main_arg);
void		test_local_cache_flush_worker(Datum main_arg);
void		test_local_cache_remove_worker(Datum main_arg);

typedef struct test_local_cache_meta
{
	slock_t		lock;
	uint64		curr_page;
	uint64		min_page;
	uint64		max_page;
	uint64		read_min_page;
	int			status;
	uint64		del_seg;
	polar_local_cache cache;
}			test_local_cache_meta;


PG_MODULE_MAGIC;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
test_prepare_dir(void)
{
	char		path[MAXPGPATH];

	Assert(polar_datadir && polar_enable_shared_storage_mode);

	Assert(MakePGDirectory(LOCAL_TEST_DIR) == 0);

	snprintf(path, MAXPGPATH, "%s/%s", polar_datadir, LOCAL_TEST_DIR);
	Assert(MakePGDirectory(path) == 0);
}

static void
test_set_block_data(uint64 block, uint64 *data)
{
	uint64		range = BLOCK_SIZE / sizeof(uint64);
	uint64		start = block * range;
	uint64		i;

	for (i = 0; i < range; i++)
		data[i] = start + i;
}

static void
test_verify_block_data(uint64 block, uint64 *data)
{
	uint64		range = BLOCK_SIZE / sizeof(uint64);
	uint64		start = block * range;
	uint64		i;

	for (i = 0; i < range; i++)
		Assert(data[i] == start + i);
}

static void
test_verify_block_zero(uint64 *data)
{
	uint64		range = BLOCK_SIZE / sizeof(uint64);
	int			i;

	for (i = 0; i < range; i++)
		Assert(data[i] == 0);
}

static void
test_verify_block_cleard(uint64 *data)
{
	uint64		range = BLOCK_SIZE / sizeof(uint64);
	uint64		i;

	for (i = 0; i < range; i++)
		Assert(data[i] == 0);
}

static void
test_local_cache_write(polar_local_cache cache, int max_segment)
{
	char		buf[BLOCK_SIZE];
	int			i,
				segno;
	int			test_blocks;
	uint32		offset;
	polar_cache_io_error io_error;
	bool		result;

	test_blocks = max_segment * BLOCK_PER_SEGMENT * LOOP_MAX_SEGMENT;

	for (i = 0; i < test_blocks; i++)
	{
		test_set_block_data(i, (uint64 *) buf);
		segno = i / BLOCK_PER_SEGMENT;
		offset = (i % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

		result = polar_local_cache_write(cache, segno, offset, buf, BLOCK_SIZE, &io_error);

		if (!result)
			polar_local_cache_report_error(cache, &io_error, PANIC);

		Assert(result == true);
	}
}

static void
test_local_cache_clear_segments(polar_local_cache cache, int max_segment)
{
	char		buf[BLOCK_SIZE] = {0};
	int			i,
				segno;
	int			test_blocks;
	uint32		offset;
	polar_cache_io_error io_error;
	bool		result;

	test_blocks = max_segment * BLOCK_PER_SEGMENT * LOOP_MAX_SEGMENT;

	for (i = 0; i < test_blocks; i++)
	{
		segno = i / BLOCK_PER_SEGMENT;
		offset = (i % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

		result = polar_local_cache_write(cache, segno, offset, buf, BLOCK_SIZE, &io_error);

		if (!result)
			polar_local_cache_report_error(cache, &io_error, PANIC);

		Assert(result == true);
	}
}

static void
test_local_cache_check_segments_cleard(polar_local_cache cache, int max_segment)
{
	char		buf[BLOCK_SIZE];
	int			i,
				segno;
	int			test_blocks;
	uint32		offset;
	polar_cache_io_error io_error;
	bool		result;

	test_blocks = max_segment * BLOCK_PER_SEGMENT * LOOP_MAX_SEGMENT;

	for (i = 0; i < test_blocks; i++)
	{
		segno = i / BLOCK_PER_SEGMENT;
		offset = (i % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

		result = polar_local_cache_read(cache, segno, offset, buf, BLOCK_SIZE, &io_error);

		if (!result)
			polar_local_cache_report_error(cache, &io_error, PANIC);

		Assert(result == true);
		test_verify_block_cleard((uint64 *) buf);
	}
}

static void
test_local_cache_read_asc(polar_local_cache cache, int max_segment)
{
	char		buf[BLOCK_SIZE];
	int			i,
				segno;
	int			test_blocks;
	uint32		offset;
	polar_cache_io_error io_error;
	bool		result;

	test_blocks = max_segment * BLOCK_PER_SEGMENT * LOOP_MAX_SEGMENT;

	for (i = 0; i < test_blocks; i++)
	{
		segno = i / BLOCK_PER_SEGMENT;
		offset = (i % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

		result = polar_local_cache_read(cache, segno, offset, buf, BLOCK_SIZE, &io_error);

		if (!result)
			polar_local_cache_report_error(cache, &io_error, PANIC);

		Assert(result == true);
		test_verify_block_data(i, (uint64 *) buf);
	}

	/* read one page from the end offset of segment with errno 11 left before */
	errno = 11;
	segno = (test_blocks - 1) / BLOCK_PER_SEGMENT;
	offset = BLOCK_PER_SEGMENT * BLOCK_SIZE;
	result = polar_local_cache_read(cache, segno, offset, buf, BLOCK_SIZE, &io_error);
	polar_local_cache_report_error(cache, &io_error, WARNING);
	Assert(result == false);
	Assert(io_error.io_return == 0);
	Assert(io_error.file_type == POLAR_LOCAL_CACHE_FILE);
	Assert(io_error.errcause == POLAR_CACHE_READ_FAILED);
	Assert(io_error.save_errno == ERANGE);
}

static void
test_local_cache_read_desc(polar_local_cache cache, int max_segment)
{
	char		buf[BLOCK_SIZE];
	int			i,
				segno;
	int			test_blocks;
	uint32		offset;
	polar_cache_io_error io_error;
	bool		result;

	test_blocks = max_segment * BLOCK_PER_SEGMENT * LOOP_MAX_SEGMENT;

	for (i = test_blocks - 1; i >= 0; i--)
	{
		segno = i / BLOCK_PER_SEGMENT;
		offset = (i % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

		result = polar_local_cache_read(cache, segno, offset, buf, BLOCK_SIZE, &io_error);

		if (!result)
			polar_local_cache_report_error(cache, &io_error, PANIC);

		Assert(result == true);
		test_verify_block_data(i, (uint64 *) buf);
	}
}

static void
test_local_cache_not_exists(polar_local_cache cache, int max_segment)
{
	uint64		i,
				segno;
	polar_cache_io_error io_error;
	uint64		test_blocks = max_segment * BLOCK_PER_SEGMENT * LOOP_MAX_SEGMENT;
	uint32		offset;
	char		buf[BLOCK_SIZE];
	bool		result;

	for (i = 0; i < test_blocks; i++)
	{
		segno = i / BLOCK_PER_SEGMENT;
		offset = (i % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

		result = polar_local_cache_read(cache, segno, offset, buf, BLOCK_SIZE, &io_error);

		Assert(result == false);
		Assert(POLAR_SEGMENT_NOT_EXISTS(&io_error));
	}
}

static void
test_local_cache_remove(polar_local_cache cache, int max_segment)
{
	uint64		i;
	polar_cache_io_error io_error;

	for (i = 0; i < max_segment * LOOP_MAX_SEGMENT; i++)
	{
		bool		ret = polar_local_cache_remove(cache, i, &io_error);

		if (!ret)
			polar_local_cache_report_error(cache, &io_error, PANIC);

		Assert(ret == true);
	}
}

static void
test_local_cache_shared_exists(polar_local_cache cache, uint32 max_segment)
{
	int			i;
	char		path[MAXPGPATH];
	char	   *local_buf,
			   *shared_buf;
	size_t		seg_size = BLOCK_SIZE * BLOCK_PER_SEGMENT;

	local_buf = malloc(seg_size);
	shared_buf = malloc(seg_size);

	for (i = 0; i < max_segment * LOOP_MAX_SEGMENT; i++)
	{
		int			fd;

		POLAR_LOCAL_FILE_PATH(path, cache, i);
		fd = polar_open(path, PG_BINARY | O_RDONLY, 0);
		Assert(fd >= 0);
		Assert(polar_read(fd, local_buf, seg_size) == seg_size);
		polar_close(fd);

		POLAR_SHARED_FILE_PATH(path, cache, i);

		fd = polar_open(path, PG_BINARY | O_RDONLY, 0);
		Assert(fd >= 0);
		Assert(polar_read(fd, shared_buf, seg_size) == seg_size);
		polar_close(fd);

		Assert(memcmp(local_buf, shared_buf, seg_size) == 0);
	}

	free(local_buf);
	free(shared_buf);
}

static void
test_local_cache_shared_not_exists(polar_local_cache cache, uint32 max_segment)
{
	struct stat st;
	int			i;
	char		path[MAXPGPATH];
	size_t		seg_size = BLOCK_SIZE * BLOCK_PER_SEGMENT;

	for (i = 0; i < max_segment * LOOP_MAX_SEGMENT; i++)
	{
		POLAR_LOCAL_FILE_PATH(path, cache, i);
		Assert(stat(path, &st) == 0);
		Assert(st.st_size == seg_size);

		POLAR_SHARED_FILE_PATH(path, cache, i);
		Assert(stat(path, &st) < 0);
		Assert(errno == ENOENT);
	}
}

static void
test_local_cache_check_consistent(polar_local_cache cache, uint32 max_segment, uint32 io_permission)
{
	if (io_permission & POLAR_CACHE_SHARED_FILE_WRITE)
		test_local_cache_shared_exists(cache, max_segment);
	else
		test_local_cache_shared_not_exists(cache, max_segment);
}

static void
test_local_cache_flushlist_exists(polar_local_cache cache)
{
	char		path[MAXPGPATH];
	struct stat st;

	POLAR_LOCAL_CACHE_META_PATH(path, cache, cache->local_flushed_times - 1);
	Assert(stat(path, &st) == 0);

	POLAR_LOCAL_CACHE_META_PATH(path, cache, cache->local_flushed_times - 2);
	Assert(stat(path, &st) == 0);

	POLAR_LOCAL_CACHE_META_PATH(path, cache, cache->local_flushed_times - 3);
	Assert(stat(path, &st) < 0 && errno == ENOENT);

}

/* Test shared storage with rw permission */
static void
test_local_cache_io(polar_local_cache cache, int max_segment, uint32 io_permission)
{
	polar_cache_io_error io_error;
	bool		ret;
	uint64		prev_flushed_times = cache->local_flushed_times;

	polar_local_cache_set_io_permission(cache, io_permission, &io_error);

	test_local_cache_write(cache, max_segment);
	test_local_cache_read_asc(cache, max_segment);
	test_local_cache_read_desc(cache, max_segment);

	test_local_cache_remove(cache, max_segment);
	test_local_cache_not_exists(cache, max_segment);

	test_local_cache_write(cache, max_segment);
	test_local_cache_read_desc(cache, max_segment);
	test_local_cache_read_asc(cache, max_segment);

	test_local_cache_remove(cache, max_segment);
	test_local_cache_not_exists(cache, max_segment);

	test_local_cache_write(cache, max_segment);

	ret = polar_local_cache_flush_all(cache, &io_error);

	if (!ret)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(ret == true);

	if (!(io_permission & POLAR_CACHE_SHARED_FILE_WRITE))
		Assert(prev_flushed_times == cache->local_flushed_times - 1);

	test_local_cache_read_asc(cache, max_segment);
	test_local_cache_read_desc(cache, max_segment);

	test_local_cache_check_consistent(cache, max_segment, io_permission);

	/* Test rewrite segments */
	test_local_cache_clear_segments(cache, max_segment);
	test_local_cache_check_segments_cleard(cache, max_segment);

	ret = polar_local_cache_flush_all(cache, &io_error);

	if (!ret)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(ret == true);

	if (!(io_permission & POLAR_CACHE_SHARED_FILE_WRITE))
		Assert(prev_flushed_times == cache->local_flushed_times - 2);

	test_local_cache_write(cache, max_segment);

	ret = polar_local_cache_flush_all(cache, &io_error);

	if (!ret)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(ret == true);

	if (!(io_permission & POLAR_CACHE_SHARED_FILE_WRITE))
	{
		Assert(prev_flushed_times == cache->local_flushed_times - 3);
		test_local_cache_flushlist_exists(cache);
	}

	test_local_cache_read_asc(cache, max_segment);
	test_local_cache_read_desc(cache, max_segment);

	test_local_cache_check_consistent(cache, max_segment, io_permission);

	test_local_cache_remove(cache, max_segment);
}

static void
test_local_cache_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	SetLatch(MyLatch);
	errno = save_errno;
}

static bool start_test = false;

static void
test_local_cache_sigusr2(SIGNAL_ARGS)
{
	int			save_errno = errno;

	start_test = true;
	SetLatch(MyLatch);
	errno = save_errno;
}

#define TEST_LOOP_TIMES 100

typedef void (*test_worker_func) (Datum);

void
test_local_cache_write_worker(Datum main_arg)
{
	uint64		write_page = 0;
	char		buf[BLOCK_SIZE];
	test_local_cache_meta *test_meta;
	bool		found;

	test_meta = (test_local_cache_meta *) ShmemInitStruct("test_local_cache_meta", sizeof(test_local_cache_meta), &found);
	Assert(found == true);

	pqsignal(SIGTERM, test_local_cache_sigterm);
	pqsignal(SIGUSR2, test_local_cache_sigusr2);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	for (;;)
	{
		int			status;
		uint64		segno;
		uint32		offset;
		bool		result;
		bool		need_sleep = false;
		polar_cache_io_error io_error;

		SpinLockAcquire(&test_meta->lock);
		status = test_meta->status;

		if (status == LOCAL_CACHE_STAT_START)
		{
			/*
			 * We need to make sure that the last block of the previous
			 * segment file is already written down. Otherwise, read workers
			 * may be failed to get that block.
			 */
			if (test_meta->curr_page % BLOCK_PER_SEGMENT == 0 &&
				test_meta->max_page + 1 < test_meta->curr_page)
			{
				need_sleep = true;
			}
			else
			{
				write_page = test_meta->curr_page;
				test_meta->curr_page++;
			}
		}

		SpinLockRelease(&test_meta->lock);

		if (status == LOCAL_CACHE_STAT_INIT || need_sleep)
		{
			pg_usleep(100);
			continue;
		}
		else if (status == LOCAL_CACHE_STAT_STOP)
			proc_exit(1);
		else
		{
			test_set_block_data(write_page, (uint64 *) buf);
			segno = write_page / BLOCK_PER_SEGMENT;
			offset = (write_page % BLOCK_PER_SEGMENT) * BLOCK_SIZE;
			result = polar_local_cache_write(test_meta->cache, segno, offset, buf, BLOCK_SIZE, &io_error);

			if (!result)
				polar_local_cache_report_error(test_meta->cache, &io_error, PANIC);

			Assert(result == true);

			SpinLockAcquire(&test_meta->lock);
			test_meta->max_page = Max(test_meta->max_page, write_page);
			SpinLockRelease(&test_meta->lock);
		}
	}
}

void
test_local_cache_read_worker(Datum main_arg)
{
	char		buf[BLOCK_SIZE];
	int			i = 0;
	test_local_cache_meta *test_meta;
	bool		found;
	uint64		read_page = 0;
	uint64		segno;
	uint32		offset;

	test_meta = (test_local_cache_meta *) ShmemInitStruct("test_local_cache_meta", sizeof(test_local_cache_meta), &found);
	Assert(found == true);

	pqsignal(SIGTERM, test_local_cache_sigterm);
	pqsignal(SIGUSR2, test_local_cache_sigusr2);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	for (;;)
	{
		int			status;
		polar_cache_io_error io_error;
		int			r = rand();
		bool		need_read = false;
		bool		result;

		i++;

		SpinLockAcquire(&test_meta->lock);
		status = test_meta->status;

		if (status == LOCAL_CACHE_STAT_START)
		{
			if (test_meta->max_page > test_meta->read_min_page)
			{
				read_page = r % (test_meta->max_page - test_meta->read_min_page);
				read_page += test_meta->read_min_page;

				if (i % 3 == 0 && test_meta->read_min_page < test_meta->max_page)
					test_meta->read_min_page++;

				need_read = true;
			}
		}

		SpinLockRelease(&test_meta->lock);

		if (status == LOCAL_CACHE_STAT_STOP)
			proc_exit(1);
		else if (status == LOCAL_CACHE_STAT_INIT || !need_read)
		{
			pg_usleep(100);
			continue;
		}
		else
		{
			segno = read_page / BLOCK_PER_SEGMENT;
			offset = (read_page % BLOCK_PER_SEGMENT) * BLOCK_SIZE;

			result = polar_local_cache_read(test_meta->cache, segno, offset, buf, BLOCK_SIZE, &io_error);

			if (!result && io_error.save_errno != ERANGE)
				polar_local_cache_report_error(test_meta->cache, &io_error, PANIC);

			/* It could be all zero if we write larger page first */
			if (((uint64 *) buf)[1] == 0)
				test_verify_block_zero((uint64 *) buf);
			else if (io_error.save_errno != ERANGE)
				test_verify_block_data(read_page, (uint64 *) buf);
		}
	}
}

void
test_local_cache_flush_worker(Datum main_arg)
{
	test_local_cache_meta *test_meta;
	bool		found;

	test_meta = (test_local_cache_meta *) ShmemInitStruct("test_local_cache_meta", sizeof(test_local_cache_meta), &found);
	Assert(found == true);

	pqsignal(SIGTERM, test_local_cache_sigterm);
	pqsignal(SIGUSR2, test_local_cache_sigusr2);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	for (;;)
	{
		polar_cache_io_error io_error;
		int			status;
		bool		result;

		SpinLockAcquire(&test_meta->lock);
		status = test_meta->status;
		SpinLockRelease(&test_meta->lock);

		if (status == LOCAL_CACHE_STAT_INIT)
		{
			pg_usleep(100);
			continue;
		}

		if (status == LOCAL_CACHE_STAT_STOP)
			proc_exit(1);

		result = polar_local_cache_flush_all(test_meta->cache, &io_error);

		if (!result)
			polar_local_cache_report_error(test_meta->cache, &io_error, PANIC);

		Assert(result == true);
		pg_usleep(1000);
	}
}

void
test_local_cache_remove_worker(Datum main_arg)
{
	test_local_cache_meta *test_meta;
	bool		found;

	test_meta = (test_local_cache_meta *) ShmemInitStruct("test_local_cache_meta", sizeof(test_local_cache_meta), &found);
	Assert(found == true);

	pqsignal(SIGTERM, test_local_cache_sigterm);
	pqsignal(SIGUSR2, test_local_cache_sigusr2);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	for (;;)
	{
		polar_cache_io_error io_error;
		int			status;
		bool		result,
					need_remove = false;
		uint64		segno;

		SpinLockAcquire(&test_meta->lock);
		status = test_meta->status;

		if (status == LOCAL_CACHE_STAT_START)
		{
			if (test_meta->max_page > test_meta->read_min_page)
			{
				if (test_meta->read_min_page - test_meta->min_page > BLOCK_PER_SEGMENT)
				{
					segno = test_meta->min_page % BLOCK_PER_SEGMENT;
					need_remove = true;
				}
			}
		}

		SpinLockRelease(&test_meta->lock);

		if (status == LOCAL_CACHE_STAT_INIT)
		{
			pg_usleep(100);
			continue;
		}

		if (status == LOCAL_CACHE_STAT_STOP)
			proc_exit(1);

		if (!need_remove)
		{
			pg_usleep(100);
			continue;
		}

		result = polar_local_cache_remove(test_meta->cache, segno, &io_error);

		if (!result)
			polar_local_cache_report_error(test_meta->cache, &io_error, PANIC);

		Assert(result == true);

		SpinLockAcquire(&test_meta->lock);
		test_meta->min_page += BLOCK_PER_SEGMENT;
		SpinLockRelease(&test_meta->lock);
	}
}

static BackgroundWorkerHandle *
test_local_cache_launch_worker(BackgroundWorker *worker, char *func)
{
	BackgroundWorkerHandle *handler = NULL;

	memset(worker, 0, sizeof(BackgroundWorker));
	worker->bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker->bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker->bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker->bgw_library_name, "test_local_cache");
	sprintf(worker->bgw_function_name, "%s", func);
	worker->bgw_notify_pid = MyProcPid;
	snprintf(worker->bgw_name, BGW_MAXLEN, "%s", func);
	snprintf(worker->bgw_type, BGW_MAXLEN, "test_local_cache");

	Assert(RegisterDynamicBackgroundWorker(worker, &handler));

	return handler;
}

static void
test_local_cache_bgworker(polar_local_cache cache)
{
#define MAX_BACK_PROCESS (6)
	bool		found;
	test_local_cache_meta *test_meta = NULL;
	BackgroundWorkerHandle *handler[MAX_BACK_PROCESS];
	BackgroundWorker worker[MAX_BACK_PROCESS];
	char	   *back_func[] =
	{
		"test_local_cache_write_worker",
		"test_local_cache_write_worker",
		"test_local_cache_read_worker",
		"test_local_cache_read_worker",
		"test_local_cache_flush_worker",
		"test_local_cache_remove_worker"
	};

	int			i = 0;

	test_meta = (test_local_cache_meta *) ShmemInitStruct("test_local_cache_meta", sizeof(test_local_cache_meta), &found);
	Assert(found == true);

	SpinLockInit(&test_meta->lock);

	for (i = 0; i < MAX_BACK_PROCESS; i++)
		handler[i] = test_local_cache_launch_worker(&worker[i], back_func[i]);

	for (i = 0; i < MAX_BACK_PROCESS; i++)
	{
		pid_t		pid;
		BgwHandleStatus status = WaitForBackgroundWorkerStartup(handler[i], &pid);

		Assert(status == BGWH_STARTED);
	}

	SpinLockAcquire(&test_meta->lock);
	test_meta->status = LOCAL_CACHE_STAT_START;
	SpinLockRelease(&test_meta->lock);

	pg_usleep(30 * 1000 * 1000);

	SpinLockAcquire(&test_meta->lock);
	test_meta->status = LOCAL_CACHE_STAT_STOP;
	SpinLockRelease(&test_meta->lock);

	for (i = 0; i < MAX_BACK_PROCESS; i++)
		Assert(WaitForBackgroundWorkerShutdown(handler[i]) == BGWH_STOPPED);
}

static bool
test_local_cache_dir_empty(const char *dirname)
{
	DIR		   *dir;
	struct dirent *de;
	int			i = 0;

	dir = AllocateDir(dirname);

	if (dir == NULL)
	{
		int			save_errno = errno;

		elog(PANIC, "Could not open dir %s, %d", dirname, save_errno);
	}

	while ((de = ReadDir(dir, dirname)) != NULL)
	{
		i++;

		if (i > 2)
			break;
	}

	FreeDir(dir);

	return i <= 2;
}

static void
test_local_cache_move_trash(polar_local_cache cache)
{
	struct stat st;

	Assert(!test_local_cache_dir_empty(cache->dir_name));
	Assert(polar_local_cache_move_trash(cache->dir_name));
	Assert(stat(cache->dir_name, &st) == 0);
	Assert(test_local_cache_dir_empty(cache->dir_name));
	Assert(polar_local_cache_empty_trash());
	Assert(test_local_cache_dir_empty(POLAR_CACHE_TRASH_DIR));
}

static void
test_local_cache_write_before_read(polar_local_cache cache)
{
	char		path[MAXPGPATH];
	char		buf[BLOCK_SIZE];
	int			fd;
	bool		result;
	polar_cache_io_error io_error;

	POLAR_SHARED_FILE_PATH(path, cache, 0);
	test_set_block_data(0, (uint64 *) buf);
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	Assert(fd >= 0);

	Assert(polar_write(fd, buf, BLOCK_SIZE) == BLOCK_SIZE);
	CloseTransientFile(fd);

	test_set_block_data(1, (uint64 *) buf);
	result = polar_local_cache_write(cache, 0, BLOCK_SIZE, buf, BLOCK_SIZE, &io_error);

	if (!result)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(result);

	result = polar_local_cache_read(cache, 0, 0, buf, BLOCK_SIZE, &io_error);

	if (!result)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(result == true);
	test_verify_block_data(0, (uint64 *) buf);

	result = polar_local_cache_remove(cache, 0, &io_error);

	if (!result)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(result);
}

static void
test_copy_to_local_failed(polar_local_cache cache)
{
	char		path[MAXPGPATH];
	char		buf[BLOCK_SIZE];
	int			fd;
	bool		result;
	polar_cache_io_error io_error;
	int			i;

	POLAR_SHARED_FILE_PATH(path, cache, 0);
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	Assert(fd >= 0);

	for (i = 0; i < BLOCK_PER_SEGMENT; i++)
	{
		test_set_block_data(i, (uint64 *) buf);
		Assert(polar_write(fd, buf, BLOCK_SIZE) == BLOCK_SIZE);
	}

	CloseTransientFile(fd);

	i = BLOCK_PER_SEGMENT / 2;
	result = polar_local_cache_read(cache, 0, i * BLOCK_SIZE, buf, BLOCK_SIZE, &io_error);

	if (!result)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(result == true);
	test_verify_block_data(i, (uint64 *) buf);

	POLAR_LOCAL_FILE_PATH(path, cache, 0);

	Assert(truncate(path, BLOCK_SIZE) == 0);
	result = polar_local_cache_read(cache, 0, i * BLOCK_SIZE, buf, BLOCK_SIZE, &io_error);

	if (!result)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(result == true);
	test_verify_block_data(i, (uint64 *) buf);

	result = polar_local_cache_remove(cache, 0, &io_error);

	if (!result)
		polar_local_cache_report_error(cache, &io_error, PANIC);

	Assert(result);
}

PG_FUNCTION_INFO_V1(test_local_cache);

/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_local_cache(PG_FUNCTION_ARGS)
{
	polar_local_cache cache;
	uint32		io_permission;

	io_permission = POLAR_CACHE_LOCAL_FILE_READ | POLAR_CACHE_LOCAL_FILE_WRITE;

	cache = polar_create_local_cache("test_local_cache", LOCAL_TEST_DIR,
									 MAX_SEGMENTS, BLOCK_SIZE * BLOCK_PER_SEGMENT, LWTRANCHE_FIRST_USER_DEFINED,
									 io_permission, NULL);

	test_local_cache_io(cache, MAX_SEGMENTS, io_permission);

	io_permission = POLAR_CACHE_LOCAL_FILE_READ | POLAR_CACHE_LOCAL_FILE_WRITE |
		POLAR_CACHE_SHARED_FILE_READ | POLAR_CACHE_SHARED_FILE_WRITE;
	test_local_cache_io(cache, MAX_SEGMENTS, io_permission);

	test_local_cache_write_before_read(cache);
	test_copy_to_local_failed(cache);

	test_local_cache_bgworker(cache);
	test_local_cache_move_trash(cache);

	PG_RETURN_VOID();
}

static void
test_local_cache_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(polar_local_cache_shmem_size(MAX_SEGMENTS));
	RequestAddinShmemSpace(sizeof(test_local_cache_meta));
}

static void
test_local_cache_shmem_startup(void)
{
	bool		found;
	polar_local_cache cache;
	uint32		io_permission;
	test_local_cache_meta *test_meta;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	io_permission = POLAR_CACHE_LOCAL_FILE_READ | POLAR_CACHE_LOCAL_FILE_WRITE;

	test_prepare_dir();
	cache = polar_create_local_cache("test_local_cache", LOCAL_TEST_DIR,
									 MAX_SEGMENTS, BLOCK_SIZE * BLOCK_PER_SEGMENT, LWTRANCHE_FIRST_USER_DEFINED,
									 io_permission, NULL);
	if (cache)
		polar_local_cache_move_trash(cache->dir_name);

	test_meta = (test_local_cache_meta *) ShmemInitStruct("test_local_cache_meta", sizeof(test_local_cache_meta), &found);
	Assert(found == false && test_meta);
	MemSet(test_meta, 0, sizeof(test_local_cache_meta));
	test_meta->cache = cache;
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(FATAL, errmsg("module should be in shared_preload_libraries"));

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_local_cache_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_local_cache_shmem_startup;
}
