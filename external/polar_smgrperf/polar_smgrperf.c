/*-------------------------------------------------------------------------
 *
 * polar_smgrperf.c
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
 *	  external/polar_smgrperf/polar_smgrperf.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/file_utils.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/smgr.h"
#include "utils/timeout.h"

PG_MODULE_MAGIC;

#define INVALID_PROC_NUMBER	InvalidBackendId
#define RelFileLocator		RelFileNode

#define MAX_RELSEG			(MaxBlockNumber / RELSEG_SIZE)
#define MAX_NBLOCKS			(MAX_RELSEG * RELSEG_SIZE)

#define PERF_REL_NUMBER		1
#define PERF_RLOCATOR(relnumber)	((RelFileLocator) {MyDatabaseTableSpace, MyDatabaseId, relnumber})
#define PERF_SMGROPEN(relnumber)	smgropen(PERF_RLOCATOR(relnumber), INVALID_PROC_NUMBER)

#define REPORT_PERF_STATS_PREPARE(with_bandwidth_option) \
	{ \
		sigjmp_buf local_sigjmp_buf; \
		perf_exception_stack = PG_exception_stack; \
		with_bandwidth = with_bandwidth_option; \
		if (sigsetjmp(local_sigjmp_buf, 1) != 0) \
		{ \
			PG_exception_stack = perf_exception_stack; \
			report_summary_perf_stats(); \
			if (perf_report_timerid != -1) \
				disable_timeout(perf_report_timerid, false); \
			pg_re_throw(); \
		} \
		PG_exception_stack = &local_sigjmp_buf; \
		MemSet(&stats, 0, sizeof(perf_stats)); \
		MemSet(&accum_stats, 0, sizeof(perf_stats)); \
		if (perf_report_timerid == -1) \
			perf_report_timerid = RegisterTimeout(USER_TIMEOUT, report_perf_stats_timeout_handler); \
		enable_timeout_after(perf_report_timerid, 1000); \
	}

typedef struct perf_stats
{
	uint64		count;
	uint64		blocks;
	uint64		time;
}			perf_stats;

static perf_stats stats;
static perf_stats accum_stats;

static ForkNumber forknum = MAIN_FORKNUM;
static BlockNumber zero_blkno = 0;
static void *zero_buffer = NULL;
static int	max_bs = 0;
static int	perf_report_timerid = -1;
static bool with_bandwidth = true;
static sigjmp_buf *perf_exception_stack = NULL;
static bool report_perf_stats_pending = false;
static instr_time start;

static inline BlockNumber
select_next_blkno(BlockNumber current_blkno, BlockNumber begin_blkno, BlockNumber end_blkno, int bs, bool sequential)
{
	BlockNumber next_blkno = InvalidBlockNumber;

	if (sequential)
	{
		if (current_blkno == InvalidBlockNumber)
			next_blkno = begin_blkno;
		else
			next_blkno = current_blkno + bs;

		if (next_blkno + bs > end_blkno)
			next_blkno = begin_blkno;
	}
	else
		next_blkno = begin_blkno + random() % (end_blkno - begin_blkno - bs + 1);

	return next_blkno;
}

static void
report_perf_stats(perf_stats * stats, char *prefix)
{
	double		iops,
				bps,
				mbps,
				lat;
#define NANOPERSECOND ((uint64) 1000 * 1000 * 1000)

	if (stats->time == 0)
		return;

	iops = (double) stats->count * NANOPERSECOND / stats->time;
	lat = (double) stats->time / stats->count / 1000;	/* to micro-second */

	HOLD_INTERRUPTS();

	if (with_bandwidth)
	{
		bps = (double) stats->blocks * NANOPERSECOND / stats->time;
		mbps = (double) stats->blocks * NANOPERSECOND * BLCKSZ / 1024 / 1024 / stats->time;

		elog(INFO, "%siops=%.1f/s, lat=%.1fus, bps=%.1f/s, mbps=%.1fMB/s",
			 prefix, iops, lat, bps, mbps);
	}
	else
		elog(INFO, "%siops=%.1f/s, lat=%.2fus", prefix, iops, lat);

	RESUME_INTERRUPTS();

	MemSet(stats, 0, sizeof(perf_stats));

#undef NANOPERSECOND
}

static void
report_perf_stats_timeout_handler(void)
{
	report_perf_stats_pending = true;
}

static void
report_summary_perf_stats(void)
{
	report_perf_stats(&accum_stats, "Summary: ");
}

static void
collect_perf_stats_begin(void)
{
	INSTR_TIME_SET_CURRENT(start);
}

static void
collect_perf_stats_end(int blocks)
{
	instr_time	duration;

	CHECK_FOR_INTERRUPTS();

	INSTR_TIME_SET_CURRENT(duration);
	INSTR_TIME_SUBTRACT(duration, start);
	stats.time += INSTR_TIME_GET_NANOSEC(duration);
	stats.blocks += blocks;
	stats.count++;

	if (report_perf_stats_pending)
	{
		accum_stats.count += stats.count;
		accum_stats.blocks += stats.blocks;
		accum_stats.time += stats.time;

		report_perf_stats(&stats, "");

		enable_timeout_after(perf_report_timerid, 1000);

		report_perf_stats_pending = false;
	}
}

static void
smgrperf_initialize()
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use smgrperf functions"))));

	/* initialize zero buffer */
	zero_buffer = polar_zero_buffer;
	max_bs = polar_zero_buffer_size / BLCKSZ;
}

PG_FUNCTION_INFO_V1(polar_smgrperf_prepare);
Datum
polar_smgrperf_prepare(PG_FUNCTION_ARGS)
{
	int			nblocks = PG_GETARG_INT32(0);
	SMgrRelation smgr = PERF_SMGROPEN(PERF_REL_NUMBER);

	if (nblocks < 0 || nblocks > MAX_NBLOCKS)
		elog(ERROR, "nblocks should be in [1, %d], current %d", MAX_NBLOCKS, nblocks);

	smgrperf_initialize();

	if (!smgrexists(smgr, forknum))
		smgrcreate(smgr, forknum, false);

	smgrtruncate(smgr, &forknum, 1, &zero_blkno);

	smgrzeroextend(smgr, forknum, 0, nblocks, true);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_smgrperf_cleanup);
Datum
polar_smgrperf_cleanup(PG_FUNCTION_ARGS)
{
	SMgrRelation smgr = PERF_SMGROPEN(PERF_REL_NUMBER);

	smgrperf_initialize();

	smgrdounlinkall(&smgr, 1, false);
	smgrclose(smgr);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_smgrperf_read);
Datum
polar_smgrperf_read(PG_FUNCTION_ARGS)
{
	int			bs = PG_GETARG_INT32(0);
	int			begin_blkno = PG_GETARG_INT32(1);
	int			end_blkno = PG_GETARG_INT32(2);
	bool		sequential = PG_GETARG_BOOL(3);
	BlockNumber current_blkno = InvalidBlockNumber;
	SMgrRelation smgr = PERF_SMGROPEN(PERF_REL_NUMBER);

	smgrperf_initialize();

	if (bs < 1 || bs > max_bs)
		elog(ERROR, "bs should be in [1, %d], current %d", max_bs, bs);

	if (begin_blkno < 0 || begin_blkno >= end_blkno)
		elog(ERROR, "\"begin_blkno\" should be in [0, %d), current %d", end_blkno, begin_blkno);

	if (end_blkno <= begin_blkno || end_blkno > MAX_NBLOCKS)
		elog(ERROR, "\"end_blkno\" should be in (%d, %d], current %d", begin_blkno, MAX_NBLOCKS, end_blkno);

	REPORT_PERF_STATS_PREPARE(true);

	while (true)
	{
		current_blkno = select_next_blkno(current_blkno, begin_blkno, end_blkno, bs, sequential);

		collect_perf_stats_begin();
		polar_smgrbulkread(smgr, forknum, current_blkno, bs, zero_buffer);
		collect_perf_stats_end(bs);
	}

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_smgrperf_write);
Datum
polar_smgrperf_write(PG_FUNCTION_ARGS)
{
	int			bs = PG_GETARG_INT32(0);
	int			begin_blkno = PG_GETARG_INT32(1);
	int			end_blkno = PG_GETARG_INT32(2);
	bool		sequential = PG_GETARG_BOOL(3);
	BlockNumber current_blkno = InvalidBlockNumber;
	SMgrRelation smgr = PERF_SMGROPEN(PERF_REL_NUMBER);

	smgrperf_initialize();

	if (bs < 1 || bs > max_bs)
		elog(ERROR, "bs should be in [1, %d], current %d", max_bs, bs);

	if (begin_blkno < 0 || begin_blkno >= end_blkno)
		elog(ERROR, "\"begin_blkno\" should be in [0, %d), current %d", end_blkno, begin_blkno);

	if (end_blkno <= begin_blkno || end_blkno > MAX_NBLOCKS)
		elog(ERROR, "\"end_blkno\" should be in (%d, %d], current %d", begin_blkno, MAX_NBLOCKS, end_blkno);

	REPORT_PERF_STATS_PREPARE(true);

	while (true)
	{
		current_blkno = select_next_blkno(current_blkno, begin_blkno, end_blkno, bs, sequential);

		collect_perf_stats_begin();
		polar_smgrbulkwrite(smgr, forknum, current_blkno, bs, zero_buffer, false);
		collect_perf_stats_end(bs);
	}

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_smgrperf_extend);
Datum
polar_smgrperf_extend(PG_FUNCTION_ARGS)
{
	int			bs = PG_GETARG_INT32(0);
	BlockNumber current_blkno = 0;
	SMgrRelation smgr = PERF_SMGROPEN(PERF_REL_NUMBER);

	smgrperf_initialize();

	if (bs < 1 || bs > max_bs)
		elog(ERROR, "bs should be in [1, %d], current %d", max_bs, bs);

	REPORT_PERF_STATS_PREPARE(true);

	if (!smgrexists(smgr, forknum))
		smgrcreate(smgr, forknum, false);

	smgrtruncate(smgr, &forknum, 1, &zero_blkno);

	while (true)
	{
		if ((current_blkno + bs) >= RELSEG_SIZE)
		{
			smgrtruncate(smgr, &forknum, 1, &zero_blkno);

			current_blkno = 0;
		}

		collect_perf_stats_begin();
		smgrzeroextend(smgr, forknum, current_blkno, bs, true);
		collect_perf_stats_end(bs);

		current_blkno += bs;
	}

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_smgrperf_nblocks);
Datum
polar_smgrperf_nblocks(PG_FUNCTION_ARGS)
{
	Oid			relnumber = PG_GETARG_INT32(0);
	bool		nblocks_cached = PG_GETARG_BOOL(1);
	bool		fd_cached = PG_GETARG_BOOL(2);

	SMgrRelation smgr = smgropen(PERF_RLOCATOR(relnumber), INVALID_PROC_NUMBER);

	smgrperf_initialize();

	if (relnumber == InvalidOid)
		elog(ERROR, "relnumber cannot be %d", InvalidOid);

	REPORT_PERF_STATS_PREPARE(false);

	elog(INFO, "Testing smgrnblocks on file with %u blocks", smgrnblocks(smgr, forknum));

	while (true)
	{
		if (!fd_cached)
		{
			smgrclose(smgr);
			smgr = smgropen(PERF_RLOCATOR(relnumber), INVALID_PROC_NUMBER);
		}

		collect_perf_stats_begin();

		if (nblocks_cached)
			smgrnblocks(smgr, forknum);
		else
			smgrnblocks_real(smgr, forknum);

		collect_perf_stats_end(0);
	}

	PG_RETURN_VOID();
}
