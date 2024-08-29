/*-------------------------------------------------------------------------
 *
 * test_ringbuf.c
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
 *	  src/test/modules/test_logindex/test_ringbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/polar_ringbuf.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "test_module_init.h"

#define RINGBUF_SIZE 4096

void		test_ringbuf_worker_main(Datum main_arg);

static void
test_fix_pktlen_overflow(void)
{
	uint8	   *data = malloc(RINGBUF_SIZE + 4);
	polar_ringbuf_t rbuf;
	uint32		pktlen;
	polar_ringbuf_ref_t ref;
	size_t		idx;
	uint16		i,
				j;
	uint32	   *overflow = (uint32 *) (&data[RINGBUF_SIZE]);

#define TEST_DATA_LEN (2)

	*overflow = UINT32_MAX;

	rbuf = polar_ringbuf_init(data, RINGBUF_SIZE, LWTRANCHE_POLAR_XLOG_QUEUE);

	Assert(polar_ringbuf_new_ref(rbuf, true, &ref, "test"));
	Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1);
	Assert(polar_ringbuf_avail(&ref) == 0);

	for (i = 0; i < 1000; i++)
	{
		idx = polar_ringbuf_pkt_reserve(rbuf, POLAR_RINGBUF_PKT_SIZE(TEST_DATA_LEN));

		Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1 - POLAR_RINGBUF_PKT_SIZE(TEST_DATA_LEN));
		Assert(polar_ringbuf_avail(&ref) == POLAR_RINGBUF_PKT_SIZE(TEST_DATA_LEN));

		polar_ringbuf_set_pkt_length(rbuf, idx, TEST_DATA_LEN);
		Assert(polar_ringbuf_pkt_write(rbuf, idx, 0, (unsigned char *) &i, TEST_DATA_LEN) == TEST_DATA_LEN);
		Assert(polar_ringbuf_next_ready_pkt(&ref, &pktlen) == POLAR_RINGBUF_PKT_INVALID_TYPE);
		polar_ringbuf_set_pkt_flag(rbuf, idx, POLAR_RINGBUF_PKT_WAL_META | POLAR_RINGBUF_PKT_READY);

		Assert(*overflow == UINT32_MAX);

		Assert(polar_ringbuf_next_ready_pkt(&ref, &pktlen) == POLAR_RINGBUF_PKT_WAL_META);
		Assert(pktlen == TEST_DATA_LEN);

		Assert(polar_ringbuf_read_next_pkt(&ref, 0, (unsigned char *) &j, TEST_DATA_LEN) == TEST_DATA_LEN);
		Assert(i == j);

		polar_ringbuf_update_ref(&ref);
		polar_ringbuf_update_keep_data(rbuf);
		Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1);
		Assert(polar_ringbuf_avail(&ref) == 0);
	}

	free(data);
}

static void
test_single_ringbuf()
{
	uint8	   *data = malloc(RINGBUF_SIZE + 4);
	polar_ringbuf_t rbuf = polar_ringbuf_init(data, RINGBUF_SIZE, LWTRANCHE_POLAR_XLOG_QUEUE);
	size_t		idx;
	uint8		buf[16];
	int			i,
				j;

	uint32		pktlen;
	polar_ringbuf_ref_t ref;
	uint32	   *overflow = (uint32 *) (&data[RINGBUF_SIZE]);

	*overflow = UINT32_MAX;

	Assert(polar_ringbuf_new_ref(rbuf, true, &ref, "test"));
	Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1);
	Assert(polar_ringbuf_avail(&ref) == 0);

	for (j = 0; j < 1000; j++)
	{
		idx = polar_ringbuf_pkt_reserve(rbuf, POLAR_RINGBUF_PKT_SIZE(16));
		Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1 - POLAR_RINGBUF_PKT_SIZE(16));
		Assert(polar_ringbuf_avail(&ref) == POLAR_RINGBUF_PKT_SIZE(16));

		memset(buf, 'A', 8);
		memset(buf + 8, 'B', 8);

		polar_ringbuf_set_pkt_length(rbuf, idx, 16);
		Assert(polar_ringbuf_pkt_write(rbuf, idx, 0, buf, 8) == 8);
		Assert(polar_ringbuf_pkt_write(rbuf, idx, 8, buf + 8, 8) == 8);
		Assert(polar_ringbuf_pkt_write(rbuf, idx, 16, buf, 8) == -1);

		Assert(polar_ringbuf_next_ready_pkt(&ref, &pktlen) == POLAR_RINGBUF_PKT_INVALID_TYPE);
		polar_ringbuf_set_pkt_flag(rbuf, idx, POLAR_RINGBUF_PKT_WAL_META | POLAR_RINGBUF_PKT_READY);

		Assert(*overflow == UINT32_MAX);

		Assert(polar_ringbuf_next_ready_pkt(&ref, &pktlen) == POLAR_RINGBUF_PKT_WAL_META);
		Assert(pktlen == 16);
		memset(buf, 0, 16);

		Assert(polar_ringbuf_read_next_pkt(&ref, 0, buf, 8) == 8);

		for (i = 0; i < 8; i++)
			Assert(buf[i] == 'A');

		Assert(polar_ringbuf_read_next_pkt(&ref, 8, buf, 8) == 8);

		for (i = 0; i < 8; i++)
			Assert(buf[i] == 'B');

		Assert(polar_ringbuf_read_next_pkt(&ref, 16, buf, 8) == -1);

		polar_ringbuf_update_ref(&ref);
		polar_ringbuf_update_keep_data(rbuf);
		Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1);
		Assert(polar_ringbuf_avail(&ref) == 0);

	}

	for (j = 0; j < 1000; j++)
	{
		uint16		len = (j + 16) % 16 + 1;
		char		c = (j % 2 == 0) ? 'A' : 'B';

		memset(buf, c, len);
		idx = polar_ringbuf_pkt_reserve(rbuf, POLAR_RINGBUF_PKT_SIZE(len));
		Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1 - POLAR_RINGBUF_PKT_SIZE(len));
		Assert(polar_ringbuf_avail(&ref) == len + POLAR_RINGBUF_PKTHDRSIZE);

		polar_ringbuf_set_pkt_length(rbuf, idx, len);
		Assert(polar_ringbuf_pkt_write(rbuf, idx, 0, buf, len) == len);
		Assert(polar_ringbuf_next_ready_pkt(&ref, &pktlen) == POLAR_RINGBUF_PKT_INVALID_TYPE);
		polar_ringbuf_set_pkt_flag(rbuf, idx, POLAR_RINGBUF_PKT_WAL_META | POLAR_RINGBUF_PKT_READY);

		Assert(polar_ringbuf_next_ready_pkt(&ref, &pktlen) == POLAR_RINGBUF_PKT_WAL_META);
		Assert(pktlen == len);
		memset(buf, 0, 16);
		Assert(polar_ringbuf_read_next_pkt(&ref, 0, buf, 16) == len);

		for (i = 0; i < len; i++)
			Assert(buf[i] == c);

		polar_ringbuf_update_ref(&ref);
		polar_ringbuf_update_keep_data(rbuf);
		Assert(polar_ringbuf_free_size(rbuf) == rbuf->size - 1);
		Assert(polar_ringbuf_avail(&ref) == 0);
	}

	free(data);
}

static void
test_ringbuf_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	SetLatch(MyLatch);
	errno = save_errno;
}

static bool start_test = false;

static void
test_ringbuf_sigusr2(SIGNAL_ARGS)
{
	int			save_errno = errno;

	start_test = true;
	SetLatch(MyLatch);
	errno = save_errno;
}

#define TEST_LOOP_TIMES 100

static void
test_ringbuf_read(polar_ringbuf_t rbuf)
{
	int			i,
				j;
	uint8		buf[16];
	polar_ringbuf_ref_t ref;

	Assert(polar_ringbuf_new_ref(rbuf, true, &ref, "test_read"));

	for (i = 0; i < TEST_LOOP_TIMES; i++)
	{
		uint32		pktlen;
		uint32		len = (i + 16) % 16 + 1;
		char		c = (i % 2 == 0) ? 'A' : 'B';

		while (!(polar_ringbuf_avail(&ref) &&
				 polar_ringbuf_next_ready_pkt(&ref, &pktlen) != POLAR_RINGBUF_PKT_INVALID_TYPE))
		{
			/*
			 * prevent the compiler to build infinite loop code via reordering
			 * loads
			 */
			pg_compiler_barrier();
		}

		Assert(pktlen == len);
		memset(buf, 0, 16);
		Assert(polar_ringbuf_read_next_pkt(&ref, 0, buf, 16) == len);

		for (j = 0; j < len; j++)
			Assert(buf[j] == c);

		polar_ringbuf_update_ref(&ref);
	}

	polar_ringbuf_release_ref(&ref);

	Assert(ref.rbuf == NULL);
}

static void
test_ringbuf_write(polar_ringbuf_t rbuf)
{
	int			i;
	uint8		buf[16];

	for (i = 0; i < TEST_LOOP_TIMES; i++)
	{
		size_t		idx;
		uint16		len = (i + 16) % 16 + 1;
		char		c = (i % 2 == 0) ? 'A' : 'B';

		memset(buf, c, len);

		while (polar_ringbuf_free_size(rbuf) < POLAR_RINGBUF_PKT_SIZE(len))
			polar_ringbuf_free_up(rbuf, POLAR_RINGBUF_PKT_SIZE(len), NULL);

		idx = polar_ringbuf_pkt_reserve(rbuf, POLAR_RINGBUF_PKT_SIZE(len));
		polar_ringbuf_set_pkt_length(rbuf, idx, len);
		Assert(polar_ringbuf_pkt_write(rbuf, idx, 0, buf, len) == len);
		polar_ringbuf_set_pkt_flag(rbuf, idx, POLAR_RINGBUF_PKT_WAL_META | POLAR_RINGBUF_PKT_READY);
	}
}

void
test_ringbuf_worker_main(Datum main_arg)
{
	bool		found;
	polar_ringbuf_t rbuf;

	rbuf = (polar_ringbuf_t) ShmemInitStruct("ringbuf_test", RINGBUF_SIZE, &found);;

	Assert(found == true);

	pqsignal(SIGTERM, test_ringbuf_sigterm);
	pqsignal(SIGUSR2, test_ringbuf_sigusr2);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	while (!start_test)
	{
		WaitLatch(&MyProc->procLatch,
				  WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
				  -1L, PG_WAIT_EXTENSION);

		/* Reset the latch, bail out if postmaster died, otherwise loop. */
		ResetLatch(&MyProc->procLatch);
	}

	test_ringbuf_read(rbuf);
}

static void
test_ringbuf_bgworker()
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle = NULL;
	pid_t		pid;
	BgwHandleStatus status;
	bool		found;
	polar_ringbuf_t rbuf;
	struct timespec start_time,
				end_time;
	long		cost;

	rbuf = (polar_ringbuf_t) ShmemInitStruct("ringbuf_test", RINGBUF_SIZE, &found);
	rbuf = polar_ringbuf_init((uint8 *) rbuf, RINGBUF_SIZE, LWTRANCHE_POLAR_XLOG_QUEUE);
	Assert(found == true);

	/* set up common data for all our workers */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "test_logindex");
	sprintf(worker.bgw_function_name, "test_ringbuf_worker_main");
	worker.bgw_notify_pid = MyProcPid;
	snprintf(worker.bgw_name, BGW_MAXLEN, "test_ringbuf");
	snprintf(worker.bgw_type, BGW_MAXLEN, "test_ringbuf");

	Assert(RegisterDynamicBackgroundWorker(&worker, &handle));
	status = WaitForBackgroundWorkerStartup(handle, &pid);

	Assert(status == BGWH_STARTED);

	sleep(1);
	kill(pid, SIGUSR2);

	clock_gettime(CLOCK_MONOTONIC, &start_time);
	test_ringbuf_write(rbuf);

	Assert(WaitForBackgroundWorkerShutdown(handle) == BGWH_STOPPED);
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	cost = (end_time.tv_sec - start_time.tv_sec) * 1000000000 +
		(end_time.tv_nsec - start_time.tv_nsec);

	ereport(LOG, (errmsg(
						 "total read/write %d ,cost %ld, qps=%ld", TEST_LOOP_TIMES, cost,
						 TEST_LOOP_TIMES * (1000000000 / cost))));
}

PG_FUNCTION_INFO_V1(test_ringbuf);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_ringbuf(PG_FUNCTION_ARGS)
{
	test_fix_pktlen_overflow();
	test_single_ringbuf();
	test_ringbuf_bgworker();
	PG_RETURN_VOID();
}

Size
test_ringbuf_request_shmem_size(void)
{
	return RINGBUF_SIZE;
}

void
test_ringbuf_shmem_startup(void)
{
	bool		found = false;
	polar_ringbuf_t rbuf = NULL;

	rbuf = (polar_ringbuf_t) ShmemInitStruct("ringbuf_test", RINGBUF_SIZE, &found);
	Assert(found == false);
	if (rbuf == NULL)
		ereport(PANIC, errmsg("failed to alloc ring buffer!"));
}
