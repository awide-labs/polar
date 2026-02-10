/*-------------------------------------------------------------------------
 *
 * px_snapshot.c
 *	  Global snapshot methods.
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
 *	  src/backend/px/px_snapshot.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "port/pg_crc32c.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "px/px_snapshot.h"
#include "px/px_vars.h"
#include "utils/faultinjector.h"

typedef struct px_snapshot_t
{
	pg_crc32c	snapshot_crc;	/* crc of snapshot_size + snapshot_data */
	int32		snapshot_size;	/* total size of px_snapshot_t */
	char		snapshot_data[0];	/* varlen data part, so we can't put crc
									 * in the end */
} px_snapshot_t;

#define PX_SNAPSHOT_ADDITIONAL_SIZE (sizeof(int32) + sizeof(pg_crc32c))

static px_snapshot_t *px_sdsnapshot = NULL;
static Snapshot px_snapshot = InvalidSnapshot;
static pg_crc32c pxsn_calc_sdsnapshot_crc(void);
static bool pxsn_snapshot_precedes(Snapshot snapshot1, Snapshot snapshot2);

static pg_crc32c
pxsn_calc_sdsnapshot_crc(void)
{
	pg_crc32c	snapshot_crc;

	INIT_CRC32C(snapshot_crc);
	/* we calculate crc from the snapshot_size section */
	COMP_CRC32C(snapshot_crc, &px_sdsnapshot->snapshot_size, px_sdsnapshot->snapshot_size - sizeof(pg_crc32c));
	FIN_CRC32C(snapshot_crc);
	return snapshot_crc;
}

/*
 * Compare two giving snapshot:
 * is snapshot1 logically older than snapshot2?
 */
static bool
pxsn_snapshot_precedes(Snapshot snapshot1, Snapshot snapshot2)
{
	int			i;

#ifdef FAULT_INJECTOR
	Snapshot	snapshot_inject = (Snapshot) palloc0(sizeof(Snapshot));

	if (SIMPLE_FAULT_INJECTOR("pxsh_snapshot_equal") == FaultInjectorTypeEnable)
		snapshot1 = snapshot2;
	pfree(snapshot_inject);
#endif

	/* compare xmin and xmax first */
	if (TransactionIdPrecedes(snapshot1->xmin, snapshot2->xmin)
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_xmin_smaller") == FaultInjectorTypeEnable
#endif
		)
	{
		elog(DEBUG5, "xmin is smaller, snapshot1 is older");
		return true;
	}

	if (TransactionIdFollows(snapshot1->xmin, snapshot2->xmin)
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_xmin_bigger") == FaultInjectorTypeEnable
#endif
		)
	{
		elog(DEBUG5, "xmin is bigger, snapshot1 is newer");
		return false;
	}

	if (TransactionIdPrecedes(snapshot1->xmax, snapshot2->xmax)
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_xmax_smaller") == FaultInjectorTypeEnable
#endif
		)
	{
		elog(DEBUG5, "xmax is smaller, snapshot1 is older");
		return true;
	}

	if (TransactionIdFollows(snapshot1->xmax, snapshot2->xmax)
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_xmax_bigger") == FaultInjectorTypeEnable
#endif
		)
	{
		elog(DEBUG5, "xmax is bigger, snapshot1 is newer");
		return false;
	}

	/* xmin and xmax are equal, now compare sub xid, larger means newer */
	for (i = 0; i < snapshot1->subxcnt && i < snapshot2->subxcnt; i++)
	{
		if (TransactionIdPrecedes(snapshot1->subxip[i], snapshot2->subxip[i]))
		{
			elog(DEBUG5, "subxip is smaller, snapshot1 is older");
			return true;
		}
		if (TransactionIdFollows(snapshot1->subxip[i], snapshot2->subxip[i]))
		{
			elog(DEBUG5, "subxip is bigger, snapshot1 is newer");
			return false;
		}
	}

	/* now xid are all euqal, more xid means newer */
	if (snapshot1->subxcnt > snapshot2->subxcnt ||
		FAULT_COND(SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_subxcnt_more") == FaultInjectorTypeEnable))
	{
		elog(DEBUG5, "subxcnt is more, snapshot1 is older");
		return true;
	}
	else if (snapshot1->subxcnt == snapshot2->subxcnt ||
			 FAULT_COND(SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_subxcnt_same") == FaultInjectorTypeEnable))
	{
		elog(DEBUG5, "subxcnt is same, snapshot1 is same");
		return false;
	}
	else if (snapshot1->subxcnt < snapshot2->subxcnt ||
			 FAULT_COND(SIMPLE_FAULT_INJECTOR("pxsh_snapshot1_subxcnt_less") == FaultInjectorTypeEnable))
	{
		elog(DEBUG5, "subxcnt is less, snapshot1 is newer");
		return false;
	}

	Assert(false);				/* shouldn't reach here */
	return false;
}

void
pxsn_log_snapshot(Snapshot snapshot, const char *func)
{
	int			i;

	if (snapshot == InvalidSnapshot)
	{
		elog(DEBUG5, "%s: px_snapshot:%d InvalidSnapshot", func, PxIdentity.dbid);
		return;
	}

	elog(DEBUG5, "%s: px_snapshot: xmin:%d, xmax:%d, lsn:%lX, xcnt: %d, subxcnt: %d",
		 func, snapshot->xmin, snapshot->xmax, snapshot->lsn, snapshot->xcnt, snapshot->subxcnt);
	if (snapshot->xcnt != 0)
	{
		StringInfo	buf = makeStringInfo();

		appendStringInfo(buf, "xip: ");
		for (i = 0; i < snapshot->xcnt; ++i)
			appendStringInfo(buf, " %d", snapshot->xip[i]);
		elog(DEBUG5, "%s", buf->data);
	}
	if (snapshot->subxcnt != 0)
	{
		StringInfo	buf = makeStringInfo();

		appendStringInfo(buf, "subxip: ");
		for (i = 0; i < snapshot->subxcnt; ++i)
			appendStringInfo(buf, " %d", snapshot->subxip[i]);
		elog(DEBUG5, "%s", buf->data);
	}
}

void
pxsn_set_snapshot(Snapshot snapshot)
{
	if (px_snapshot != InvalidSnapshot)
		pfree(px_snapshot);
	if (snapshot != InvalidSnapshot)
		pxsn_log_snapshot(snapshot, __func__);
	px_snapshot = snapshot;
}

void
pxsn_set_oldest_snapshot(Snapshot snapshot)
{
	if (snapshot != InvalidSnapshot)
		pxsn_log_snapshot(snapshot, __func__);
	if (snapshot == InvalidSnapshot || px_snapshot == InvalidSnapshot)
	{
		px_snapshot = snapshot;
		return;
	}
	if (pxsn_snapshot_precedes(snapshot, px_snapshot))
	{
		pfree(px_snapshot);
		px_snapshot = snapshot;
	}
}

char *
pxsn_get_serialized_snapshot(void)
{
	pxsn_log_snapshot(px_snapshot, __func__);
	if (px_snapshot != InvalidSnapshot)
	{
		int32		snapshot_size;

		if (px_sdsnapshot != NULL)
			pfree(px_sdsnapshot);
		snapshot_size = EstimateSnapshotSpace(px_snapshot) + PX_SNAPSHOT_ADDITIONAL_SIZE;
		px_sdsnapshot = MemoryContextAlloc(TopMemoryContext, snapshot_size);
		SerializeSnapshot(px_snapshot, px_sdsnapshot->snapshot_data);

		px_sdsnapshot->snapshot_size = snapshot_size;
		px_sdsnapshot->snapshot_crc = pxsn_calc_sdsnapshot_crc();

		return (char *) px_sdsnapshot;
	}
	else
		return NULL;
}

int
pxsn_get_serialized_snapshot_size(void)
{
	pxsn_log_snapshot(px_snapshot, __func__);
	if (px_snapshot != InvalidSnapshot)
		return EstimateSnapshotSpace(px_snapshot) + PX_SNAPSHOT_ADDITIONAL_SIZE;
	else
		return 0;
}

char *
pxsn_get_serialized_snapshot_data(void)
{
	if (px_sdsnapshot)
		return px_sdsnapshot->snapshot_data;
	return NULL;
}

void
pxsn_set_serialized_snapshot(const char *sdsnapshot, int size)
{
	Assert(size != 0);
	Assert(sdsnapshot != NULL);

	if (px_sdsnapshot != NULL)
		pfree(px_sdsnapshot);
	px_sdsnapshot = MemoryContextAlloc(TopMemoryContext, size);
	memcpy(px_sdsnapshot, sdsnapshot, size);

	/* validate this snapshot */
	if (px_sdsnapshot->snapshot_size == 0)
	{
		pfree(px_sdsnapshot);
		elog(ERROR, "px_snapshot: wrong snapshot size");
	}

	if (!EQ_CRC32C(pxsn_calc_sdsnapshot_crc(), px_sdsnapshot->snapshot_crc))
	{
		pfree(px_sdsnapshot);
		elog(ERROR, "px_snapshot: wrong snapshot crc");
	}
}
