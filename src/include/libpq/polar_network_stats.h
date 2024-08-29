/*-------------------------------------------------------------------------
 *
 * polar_network_stats.h
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
 *	  src/include/libpq/polar_network_stats.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_NETWORK_STATS_H
#define POLAR_NETWORK_STATS_H

#include "portability/instr_time.h"

/*
 * POLAR: network operations: just send/recv now
 */
enum polar_network_op
{
	POLAR_NETWORK_SEND_STAT = 0,
	POLAR_NETWORK_RECV_STAT = 1,
	POLAR_NETWORK_STAT_MAX = 2
};

typedef struct polar_network_op_stat
{
	uint64		count;			/* call send()/recv() count */
	uint64		bytes;			/* send/recv bytes */
	uint64		block_time;		/* send/recv block time */
	uint64		last_block_time;	/* last send/recv block start timestamp */
} polar_network_op_stat;

/* net stat */
typedef struct polar_net_stat
{
	/* from local network send/recv stat */
	polar_network_op_stat opstat[POLAR_NETWORK_STAT_MAX];

	/* from tcp info */
	uint32		cwnd;
	uint32		rtt;
	uint32		rttvar;
	uint32		tcpinfo_update_time;
	uint64		total_retrans;

	/* for send/recv queue len */
	int32		sendq;
	int32		recvq;
} polar_net_stat;

extern void polar_local_network_stat(void);
extern void polar_network_sendrecv_stat(int op, uint64 n_bytes);
extern void polar_network_block_start(int op);
extern void polar_network_block_end(int op);
extern uint64 polar_network_real_block_time(polar_network_op_stat *stat);

extern polar_net_stat *polar_network_stat_array;

#endif							/* POLAR_NETWORK_STATS_H */
