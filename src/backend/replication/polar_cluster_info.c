/*-------------------------------------------------------------------------
 *
 * polar_cluster_info.c
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
 *  src/backend/replication/polar_cluster_info.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/ip.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "px/px_util.h"
#include "replication/polar_cluster_info.h"
#include "replication/walsender_private.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "replication/slot.h"
#include "storage/shmem.h"
#include "utils/guc.h"

polar_cluster_info_ctl_t *polar_cluster_info_ctl = NULL;

Size
polar_cluster_info_shmem_size(void)
{
	return sizeof(polar_cluster_info_ctl_t);
}

void
polar_cluster_info_shmem_init(void)
{
	bool		found = false;

	polar_cluster_info_ctl = (polar_cluster_info_ctl_t *)
		ShmemInitStruct("PolarDB cluster info control",
						sizeof(polar_cluster_info_ctl_t),
						&found);

	if (!IsUnderPostmaster)
	{
		polar_cluster_info_ctl->count = 0;
		polar_cluster_info_ctl->generation = 0;

		LWLockRegisterTranche(LWTRANCHE_POLAR_CLUSTER_INFO, "PolarDB cluster info lock");
		LWLockInitialize(&polar_cluster_info_ctl->lock, LWTRANCHE_POLAR_CLUSTER_INFO);
	}
	else
		Assert(found);
}

void
polar_update_cluster_info(void)
{
	int			i,
				count = 0;
	polar_cluster_info_item *items = polar_cluster_info_ctl->items;
	polar_cluster_info_item *item;

	/* POLAR: only master could maintain cluster info */
	if (!polar_in_master_mode())
		return;

	LWLockAcquire(&polar_cluster_info_ctl->lock, LW_EXCLUSIVE);

	/* self */
	item = &items[count++];
	if (polar_instance_name[0] == '\0')
		snprintf(item->name, NAMEDATALEN, "node%d", 0);
	else
		snprintf(item->name, NAMEDATALEN, "%s", polar_instance_name);
	snprintf(item->host, NI_MAXHOST, "%s", "127.0.0.1");
	item->port = PostPortNumber;
	snprintf(item->release_date, 20, "%s", polar_release_date);
	snprintf(item->version, 20, "%s", polar_version);
	snprintf(item->slot_name, NAMEDATALEN, "%s", "");
	item->type = polar_get_node_type();
	if (polar_get_available_state())
		item->state = polar_get_hot_standby_state();
	else
		item->state = POLAR_NODE_GOING_OFFLINE;
	item->cpu = 0;
	item->cpu_quota = 0;
	item->memory = 0;
	item->memory_quota = 0;
	item->iops = 0;
	item->iops_quota = 0;
	item->connection = 0;
	item->connection_quota = 0;
	item->px_connection = 0;
	item->px_connection_quota = 0;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		item = &items[count++];
		if (s->node_name[0] == '\0')
			snprintf(item->name, NAMEDATALEN, "node%d", count - 1);
		else
			snprintf(item->name, NAMEDATALEN, "%s", s->node_name);
		snprintf(item->host, NI_MAXHOST, "%s", s->node_host);
		item->port = s->node_port;
		snprintf(item->release_date, 20, "%s", s->node_release_date);
		snprintf(item->version, 20, "%s", s->node_version);
		snprintf(item->slot_name, NAMEDATALEN, "%s", s->node_slot_name);
		item->type = s->node_type;
		item->state = s->active_pid == 0 ? POLAR_NODE_OFFLINE : s->node_state;
		item->cpu = s->node_cpu;
		item->cpu_quota = s->node_cpu_quota;
		item->memory = s->node_memory;
		item->memory_quota = s->node_memory_quota;
		item->iops = s->node_iops;
		item->iops_quota = s->node_iops_quota;
		item->connection = s->node_connection;
		item->connection_quota = s->node_connection_quota;
		item->px_connection = s->node_px_connection;
		item->px_connection_quota = s->node_px_connection_quota;

		/* can hold no more, break */
		if (count == POLAR_CLUSTER_INFO_ITEMS_SIZE)
			break;
	}
	LWLockRelease(ReplicationSlotControlLock);

	POLAR_CLUSTER_INFO_INC_GENERATION();
	polar_cluster_info_ctl->count = count;
	LWLockRelease(&polar_cluster_info_ctl->lock);
	elog(LOG, "update cluster info, generation: %d, count: %d", polar_cluster_info_ctl->generation, count);
	WalSndWakeup();
}

/*
 * POLAR: send node info, can only be called from walreceiver
 */
void
polar_send_node_info(StringInfo output_message)
{
	if (!polar_enable_send_node_info)
		return;

	Assert(WalRcv->pid == MyProcPid);

	if (WalRcv->slotname[0] == '\0')
	{
		elog(DEBUG2, "No slot, skip send node info");
		return;
	}

	elog(LOG, "sending node info");

	/* construct the message... */
	resetStringInfo(output_message);
	pq_sendbyte(output_message, 'm');
	pq_sendstring(output_message, polar_instance_name);
	pq_sendstring(output_message, "");	/* host, left it empty */
	pq_sendint(output_message, PostPortNumber, 4);
	pq_sendstring(output_message, polar_release_date);
	pq_sendstring(output_message, polar_version);
	pq_sendstring(output_message, WalRcv->slotname);
	pq_sendint(output_message, polar_get_node_type(), 4);
	if (polar_get_available_state())
		pq_sendint(output_message, polar_get_hot_standby_state(), 4);
	else
		pq_sendint(output_message, POLAR_NODE_GOING_OFFLINE, 4);
	pq_sendint(output_message, 0, 4);	/* cpu */
	pq_sendint(output_message, 0, 4);
	pq_sendint(output_message, 0, 4);	/* memory */
	pq_sendint(output_message, 0, 4);
	pq_sendint(output_message, 0, 4);	/* iops */
	pq_sendint(output_message, 0, 4);
	pq_sendint(output_message, 0, 4);	/* connection */
	pq_sendint(output_message, 0, 4);
	pq_sendint(output_message, 0, 4);	/* px_connection */
	pq_sendint(output_message, 0, 4);
}

/*
 * POLAR: process node info, can only be called from walsender of RW
 */
void
polar_process_node_info(StringInfo input_message)
{
	int			ret;
	char		remote_host[NI_MAXHOST];
	Port	   *port = MyProcPort;
	ReplicationSlot *s = MyReplicationSlot;

	elog(LOG, "processing node info");
	Assert(s);

	/* POLAR: now we can send cluster info to this slot */
	if (s->polar_sent_cluster_info_generation == POLAR_CLUSTER_INFO_INVALID_GENERATION)
		s->polar_sent_cluster_info_generation = 0;

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);

	/*
	 * POLAR: failed or local socket, for now we don't support it, try to
	 * convert to localhost
	 */
	if (ret != 0 || strcmp(remote_host, "[local]") == 0)
		snprintf(remote_host, NI_MAXHOST, "127.0.0.1");

	snprintf(s->node_name, NAMEDATALEN, "%s", pq_getmsgstring(input_message));
	(void) pq_getmsgstring(input_message);	/* host, which is empty, drop it */
	snprintf(s->node_host, NI_MAXHOST, "%s", remote_host);
	s->node_port = pq_getmsgint(input_message, 4);
	snprintf(s->node_release_date, 20, "%s", pq_getmsgstring(input_message));
	snprintf(s->node_version, 20, "%s", pq_getmsgstring(input_message));
	snprintf(s->node_slot_name, NAMEDATALEN, "%s", pq_getmsgstring(input_message));
	s->node_type = pq_getmsgint(input_message, 4);
	s->node_state = pq_getmsgint(input_message, 4);
	s->node_cpu = pq_getmsgint(input_message, 4);
	s->node_cpu_quota = pq_getmsgint(input_message, 4);
	s->node_memory = pq_getmsgint(input_message, 4);
	s->node_memory_quota = pq_getmsgint(input_message, 4);
	s->node_iops = pq_getmsgint(input_message, 4);
	s->node_iops_quota = pq_getmsgint(input_message, 4);
	s->node_connection = pq_getmsgint(input_message, 4);
	s->node_connection_quota = pq_getmsgint(input_message, 4);
	s->node_px_connection = pq_getmsgint(input_message, 4);
	s->node_px_connection_quota = pq_getmsgint(input_message, 4);

	polar_update_cluster_info();
}

/*
 * POLAR: send cluster info, can only be called from walsender of RW
 */
void
polar_send_cluster_info(StringInfo output_message)
{
	int			i,
				ret;
	int			saved_generation = polar_cluster_info_ctl->generation;
	char		local_host[NI_MAXHOST];
	Port	   *port = MyProcPort;
	ReplicationSlot *s = MyReplicationSlot;
	polar_cluster_info_item *items = polar_cluster_info_ctl->items;
	polar_cluster_info_item *item;

	if (!polar_enable_send_cluster_info)
		return;

	if (s == NULL || SlotIsLogical(s) || saved_generation == s->polar_sent_cluster_info_generation ||
		s->polar_sent_cluster_info_generation == POLAR_CLUSTER_INFO_INVALID_GENERATION)
		return;

	elog(LOG, "sending cluster info, from %d to %d",
		 s->polar_sent_cluster_info_generation, saved_generation);

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 local_host, sizeof(local_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);

	/*
	 * POLAR: failed or local socket, for now we don't support it, try to
	 * convert to localhost
	 */
	if (ret != 0 || strcmp(local_host, "[local]") == 0)
		snprintf(local_host, NI_MAXHOST, "127.0.0.1");

	resetStringInfo(output_message);
	pq_sendbyte(output_message, 'M');

	LWLockAcquire(&polar_cluster_info_ctl->lock, LW_SHARED);

	/* construct the message... */
	pq_sendint(output_message, polar_cluster_info_ctl->count, 4);
	pq_sendint(output_message, polar_cluster_info_ctl->generation, 4);

	for (i = 0; i < polar_cluster_info_ctl->count; i++)
	{
		item = &items[i];
		pq_sendstring(output_message, item->name);
		if (strcmp(item->host, "127.0.0.1") == 0)
			pq_sendstring(output_message, local_host);
		else
			pq_sendstring(output_message, item->host);
		pq_sendint(output_message, item->port, 4);
		pq_sendstring(output_message, item->release_date);
		pq_sendstring(output_message, item->version);
		pq_sendstring(output_message, item->slot_name);
		pq_sendint(output_message, item->type, 4);
		pq_sendint(output_message, item->state, 4);
		pq_sendint(output_message, item->cpu, 4);
		pq_sendint(output_message, item->cpu_quota, 4);
		pq_sendint(output_message, item->memory, 4);
		pq_sendint(output_message, item->memory_quota, 4);
		pq_sendint(output_message, item->iops, 4);
		pq_sendint(output_message, item->iops_quota, 4);
		pq_sendint(output_message, item->connection, 4);
		pq_sendint(output_message, item->connection_quota, 4);
		pq_sendint(output_message, item->px_connection, 4);
		pq_sendint(output_message, item->px_connection_quota, 4);
	}
	LWLockRelease(&polar_cluster_info_ctl->lock);

	/* send it wrapped in CopyData */
	pq_putmessage_noblock('d', output_message->data, output_message->len);

	s->polar_sent_cluster_info_generation = saved_generation;
}

/*
 * POLAR: process cluster info, can only be called from walreceiver
 */
void
polar_process_cluster_info(StringInfo input_message)
{
	int			i,
				count,
				generation;
	polar_cluster_info_item *items = polar_cluster_info_ctl->items;
	polar_cluster_info_item *item;

	elog(LOG, "processing cluster info");

	count = pq_getmsgint(input_message, 4);
	Assert(count > 0);
	generation = pq_getmsgint(input_message, 4);

	LWLockAcquire(&polar_cluster_info_ctl->lock, LW_EXCLUSIVE);
	for (i = 0; i < count; ++i)
	{
		item = &items[i];
		snprintf(item->name, NAMEDATALEN, "%s", pq_getmsgstring(input_message));
		snprintf(item->host, NI_MAXHOST, "%s", pq_getmsgstring(input_message));
		item->port = pq_getmsgint(input_message, 4);
		snprintf(item->release_date, 20, "%s", pq_getmsgstring(input_message));
		snprintf(item->version, 20, "%s", pq_getmsgstring(input_message));
		snprintf(item->slot_name, NAMEDATALEN, "%s", pq_getmsgstring(input_message));
		item->type = pq_getmsgint(input_message, 4);
		item->state = pq_getmsgint(input_message, 4);
		item->cpu = pq_getmsgint(input_message, 4);
		item->cpu_quota = pq_getmsgint(input_message, 4);
		item->memory = pq_getmsgint(input_message, 4);
		item->memory_quota = pq_getmsgint(input_message, 4);
		item->iops = pq_getmsgint(input_message, 4);
		item->iops_quota = pq_getmsgint(input_message, 4);
		item->connection = pq_getmsgint(input_message, 4);
		item->connection_quota = pq_getmsgint(input_message, 4);
		item->px_connection = pq_getmsgint(input_message, 4);
		item->px_connection_quota = pq_getmsgint(input_message, 4);
	}
	polar_cluster_info_ctl->count = count;
	polar_cluster_info_ctl->generation = generation;
	LWLockRelease(&polar_cluster_info_ctl->lock);

	elog(LOG, "receive cluster info, generation: %d, count: %d", generation, count);
}

/*
 * POLAR: offline cluster info
 */
void
polar_cluster_info_offline(int code, Datum arg)
{
	elog(LOG, "mark cluster info offline");
	LWLockAcquire(&polar_cluster_info_ctl->lock, LW_EXCLUSIVE);
	polar_cluster_info_ctl->generation = POLAR_CLUSTER_INFO_OFFLINE_GENERATION;
	polar_cluster_info_ctl->items[0].state = POLAR_NODE_OFFLINE;
	LWLockRelease(&polar_cluster_info_ctl->lock);
	polar_invalid_px_nodes_cache();
}
