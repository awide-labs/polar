/*-------------------------------------------------------------------------
 *
 * polar_network_stats.c
 *	  network monitor for PolarDB
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
 *	  src/backend/libpq/polar_network_stats.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <linux/sockios.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "libpq/polar_network_stats.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "utils/guc.h"
#include "utils/timeout.h"

polar_net_stat *polar_network_stat_array = NULL;
static int	last_network_tcpinfo_update_time = 0;
static int	last_local_network_stat_init = 0;

#define POLAR_NETWORK_STAT_INTERVAL_IN_MS 1000
#define POLAR_NETWORK_STAT_INTERVAL_IN_S (POLAR_NETWORK_STAT_INTERVAL_IN_MS / 1000)

#define IS_MYBACKEND_ID_VALID() \
	(MyBackendId != InvalidBackendId && MyBackendId >= 1 && MyBackendId <= MaxBackends)

#define IS_MYAUX_PROC_TYPE_VALID() (MyAuxProcType != NotAnAuxProcess)

#define POLAR_NET_STAT_BACKEND_INDEX() \
	(IS_MYBACKEND_ID_VALID() ? MyBackendId - 1 : \
		(IS_MYAUX_PROC_TYPE_VALID() ? MaxBackends + MyAuxProcType : -1))

#define IS_POLAR_NET_STAT_OP_VALID() (op >= 0 && op < POLAR_NETWORK_STAT_MAX)

/*
 * POLAR: get network info for timer
 */
static void polar_network_stat_timer(void);
static void polar_network_get_tcpinfo(void);
static void polar_network_get_qlen(void);

static int	polar_network_stat_timerid = -1;
static bool polar_last_enable_track_network_flag = false;
static instr_time block_start_time;
static uint64 block_seq = 0;
static uint64 block_stat_seq = 0;

#define IS_NETWORK_STAT_TIMER_ID_VALID() \
	(polar_network_stat_timerid >= USER_TIMEOUT && polar_network_stat_timerid < MAX_TIMEOUTS)

/*
 * POLAR: network send/recv start record
 */
void
polar_local_network_stat(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	if (polar_enable_track_network_stat &&
		now.tv_sec - last_local_network_stat_init > 3 * POLAR_NETWORK_STAT_INTERVAL_IN_S &&
		now.tv_sec - last_network_tcpinfo_update_time > 2 * POLAR_NETWORK_STAT_INTERVAL_IN_S)
	{
		if (!IS_NETWORK_STAT_TIMER_ID_VALID())
			polar_network_stat_timerid = RegisterTimeout(USER_TIMEOUT, polar_network_stat_timer);
		disable_timeout(polar_network_stat_timerid, false);
		enable_timeout_after(polar_network_stat_timerid, POLAR_NETWORK_STAT_INTERVAL_IN_MS);
		polar_last_enable_track_network_flag = true;
		last_local_network_stat_init = now.tv_sec;
	}
}

/*
 * POLAR: network send/recv end record
 */
void
polar_network_sendrecv_stat(int op, uint64 n_bytes)
{
	int			index;

	if (!polar_enable_track_network_stat || !polar_network_stat_array)
		return;

	index = POLAR_NET_STAT_BACKEND_INDEX();
	if (index < 0)
		return;

	if (!IS_POLAR_NET_STAT_OP_VALID())
		return;

	polar_network_stat_array[index].opstat[op].count++;
	polar_network_stat_array[index].opstat[op].bytes += n_bytes;
}

/*
 * POLAR: network block start record
 */
void
polar_network_block_start(int op)
{
	block_seq++;
	if (polar_enable_track_network_stat && polar_network_stat_array && polar_enable_track_network_timing)
	{
		int			index = POLAR_NET_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		if (!IS_POLAR_NET_STAT_OP_VALID())
			return;

		INSTR_TIME_SET_CURRENT(block_start_time);
		polar_network_stat_array[index].opstat[op].last_block_time = INSTR_TIME_GET_MICROSEC(block_start_time);

		block_stat_seq = block_seq;
	}
}

/*
 * POLAR: network block end record
 */
void
polar_network_block_end(int op)
{
	if (polar_enable_track_network_stat && polar_network_stat_array && polar_enable_track_network_timing)
	{
		instr_time	end;
		uint64		diff;

		int			index = POLAR_NET_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		if (!IS_POLAR_NET_STAT_OP_VALID())
			return;

		if (block_seq != block_stat_seq)
			return;

		INSTR_TIME_SET_CURRENT(end);
		diff = INSTR_TIME_GET_MICROSEC(end) - INSTR_TIME_GET_MICROSEC(block_start_time);

		polar_network_stat_array[index].opstat[op].block_time += diff;
		polar_network_stat_array[index].opstat[op].last_block_time = 0;
	}
}

/*
 * POLAR: util function for block time.
 */
uint64
polar_network_real_block_time(polar_network_op_stat *stat)
{
	uint64		block_time = stat->block_time;

	if (stat->last_block_time != 0)
	{
		instr_time	end;

		INSTR_TIME_SET_CURRENT(end);
		block_time += (INSTR_TIME_GET_MICROSEC(end) - stat->last_block_time);
	}

	return block_time;
}

/*
 * POLAR: timer handler for network stat
 */
static void
polar_network_stat_timer(void)
{
	polar_network_get_tcpinfo();
	polar_network_get_qlen();


	if (polar_enable_track_network_stat && IS_NETWORK_STAT_TIMER_ID_VALID())
		enable_timeout_after(polar_network_stat_timerid, POLAR_NETWORK_STAT_INTERVAL_IN_MS);
}

/*
 * POLAR: tcpinfo
 */
static void
polar_network_get_tcpinfo(void)
{
	struct tcp_info info;
	int			length = sizeof(struct tcp_info);
	int			ret;

	if (!polar_network_stat_array)
		return;

	if (!MyProcPort)
		return;

	ret = getsockopt(MyProcPort->sock, SOL_TCP, TCP_INFO, (void *) &info, (socklen_t *) &length);
	if (ret == 0)
	{
		struct timeval now;
		int			index = POLAR_NET_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		gettimeofday(&now, NULL);

		polar_network_stat_array[index].cwnd = info.tcpi_snd_cwnd;
		polar_network_stat_array[index].rtt = info.tcpi_rtt;
		polar_network_stat_array[index].rttvar = info.tcpi_rttvar;
		polar_network_stat_array[index].total_retrans = info.tcpi_total_retrans;
		polar_network_stat_array[index].tcpinfo_update_time = now.tv_sec;
		last_network_tcpinfo_update_time = now.tv_sec;
	}
}

/*
 * POLAR: socket send-Q/recv-Q length
 */
static void
polar_network_get_qlen(void)
{
	int			queue_len;
	int			index;
	int			ret;

	if (!polar_network_stat_array)
		return;

	if (!MyProcPort)
		return;

	index = POLAR_NET_STAT_BACKEND_INDEX();
	if (index < 0)
		return;

	ret = ioctl(MyProcPort->sock, SIOCOUTQ, &queue_len);
	if (ret == 0)
		polar_network_stat_array[index].sendq = queue_len;

	ret = ioctl(MyProcPort->sock, SIOCINQ, &queue_len);
	if (ret == 0)
		polar_network_stat_array[index].recvq = queue_len;
}
