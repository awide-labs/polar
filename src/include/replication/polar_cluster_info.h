/*-------------------------------------------------------------------------
 *
 * polar_cluster_info.h
 *	  PolarDB cluster info routines.
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
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
 *  src/include/replication/polar_cluster_info.h
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "storage/lock.h"

typedef struct polar_cluster_info_item
{
	char		name[NAMEDATALEN];
	char		host[NI_MAXHOST];
	int			port;
	char		release_date[20];
	char		version[20];
	int			type;
	int			state;
	char		slot_name[NAMEDATALEN];
	/* PX: used for cross node load balancing, left empty for now */
	int			cpu;
	int			cpu_quota;
	int			memory;
	int			memory_quota;
	int			iops;
	int			iops_quota;
	int			connection;
	int			connection_quota;
	int			px_connection;
	int			px_connection_quota;
} polar_cluster_info_item;

#define POLAR_NODE_GOING_OFFLINE -1
#define POLAR_NODE_OFFLINE -2
#define POLAR_CLUSTER_INFO_ITEMS_SIZE 64
#define POLAR_CLUSTER_INFO_INVALID_GENERATION (-1)
#define POLAR_CLUSTER_INFO_OFFLINE_GENERATION (-2)
#define POLAR_CLUSTER_INFO_INC_GENERATION() do \
	{ \
		polar_cluster_info_ctl->generation++; \
		if (unlikely(polar_cluster_info_ctl->generation < 0)) \
			polar_cluster_info_ctl->generation = 0; \
	} while (0);

typedef struct polar_cluster_info_ctl_t
{
	int32		generation;
	int32		count;
	LWLock		lock;			/* Lock to protect node info table */
	polar_cluster_info_item items[POLAR_CLUSTER_INFO_ITEMS_SIZE];
} polar_cluster_info_ctl_t;
extern polar_cluster_info_ctl_t *polar_cluster_info_ctl;

extern Size polar_cluster_info_shmem_size(void);
extern void polar_cluster_info_shmem_init(void);
extern void polar_update_cluster_info(void);
extern void polar_send_node_info(StringInfo output_message);
extern void polar_process_node_info(StringInfo input_message);
extern void polar_send_cluster_info(StringInfo output_message);
extern void polar_process_cluster_info(StringInfo input_message);
extern void polar_cluster_info_offline(int code, Datum arg);
