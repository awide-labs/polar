/*-------------------------------------------------------------------------
 *
 * px_util.c
 *	  Internal utility support functions for Greenplum Database/PostgreSQL.
 *
 * Portions Copyright (c) 2005-2011, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/px_util.c
 *
 * NOTES
 *
 *	- According to src/backend/executor/execHeapScan.c
 *		"tuples returned by heap_getnext() are pointers onto disk
 *		pages and were not created with palloc() and so should not
 *		be pfree()'d"
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/param.h>			/* for MAXHOSTNAMELEN */

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "common/ip.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "replication/polar_cluster_info.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/varlena.h"
#include "px/px_conn.h"
#include "px/px_hash.h"
#include "px/px_util.h"
#include "px/px_motion.h"
#include "px/px_vars.h"
#include "px/px_gang.h"

#define MAX_CACHED_1_GANGS 1

#define INCR_COUNT(pxinfo, arg) \
	(pxinfo)->arg++; \
	(pxinfo)->px_nodes->arg++;

#define DECR_COUNT(pxinfo, arg) \
	(pxinfo)->arg--; \
	(pxinfo)->px_nodes->arg--; \
	Assert((pxinfo)->arg >= 0); \
	Assert((pxinfo)->px_nodes->arg >= 0); \

char	   *polar_px_nodes = NULL;

#include "px/px_motion.h"
static PxNodes * px_nodes = NULL;
static int	px_node_configs_size = 0;

static int	px_node_configs_generation = POLAR_CLUSTER_INFO_INVALID_GENERATION;

static MemoryContext px_worker_context = NULL;
static PxNodeConfigEntry *px_node_configs = NULL;

static int	nextPXIdentifer(PxNodes * px_nodes);
static void GeneratePxNodeConfigs(void);

/* NB: all extern function should switch to this context */
MemoryContext
SwitchToPXWorkerContext(void)
{
	if (!px_worker_context)
		px_worker_context = AllocSetContextCreate(TopMemoryContext, "PX components Context",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);

	return MemoryContextSwitchTo(px_worker_context);
}

void
polar_invalid_px_nodes_cache()
{
	px_node_configs_generation = POLAR_CLUSTER_INFO_INVALID_GENERATION;
}

static void
ClearPxNodeConfigs(void)
{
	int			i;

	if (px_node_configs)
	{
		for (i = 0; i < px_node_configs_size - 1; i++)
		{
			pfree(px_node_configs[i].hostip);
			pfree(px_node_configs[i].name);
		}
		pfree(px_node_configs);
		px_node_configs = NULL;
		px_node_configs_size = 0;
	}
}

/* NB: should only be called from pxnode_getPxNodes */
static void
GeneratePxNodeConfigs(void)
{
	char	   *confstring;
	List	   *nodelist;
	ListCell   *lnode;
	int			size;
	int			idx;
	PxNodeConfigEntry *configs,
			   *qc_config;
	bool		use_all;
	int			i;
	int			count;
	polar_cluster_info_item *items;
	polar_cluster_info_item *item;
	PxNodeConfigEntry *config;
	int			error_level = polar_px_ignore_unusable_nodes ? WARNING : ERROR;

	ClearPxNodeConfigs();

	use_all = (polar_px_nodes == NULL || polar_px_nodes[0] == '\0');

	confstring = pstrdup(polar_px_nodes);
	(void) SplitIdentifierString(confstring, ',', &nodelist);
	size = list_length(nodelist);

	/* if not set, means use all nodes */
	if (size == 0)
		use_all = true;

fallback:
	LWLockAcquire(&polar_cluster_info_ctl->lock, LW_SHARED);
	count = polar_cluster_info_ctl->count;
	items = polar_cluster_info_ctl->items;
	configs = palloc0(sizeof(PxNodeConfigEntry) * (use_all ? count : size + 1));

	if (polar_cluster_info_ctl->generation == POLAR_CLUSTER_INFO_OFFLINE_GENERATION)
		elog(WARNING, "cluster info may be outdated, PX may not working");

	idx = 0;
	if (use_all)
	{
		for (i = 0; i < count; i++)
		{
			item = &items[i];
			if (item->type != POLAR_PRIMARY && item->type != POLAR_REPLICA && item->type != POLAR_STANDBY)
				continue;
			if (item->type == POLAR_PRIMARY && !px_use_master)
				continue;
			if (item->type == POLAR_STANDBY && !px_use_standby)
				continue;
			if (!(item->state == STANDBY_SNAPSHOT_READY || (item->type == POLAR_PRIMARY && px_use_master)))
				continue;

			config = &configs[idx];
			config->dbid = idx;
			config->role = 'x';
			config->node_idx = idx;
			config->dop = 1;
			config->port = item->port;
			config->hostip = pstrdup(item->host);
			config->name = pstrdup(item->name);
			++idx;
		}
	}
	else
		foreach(lnode, nodelist)
	{
		char	   *node_name = (char *) lfirst(lnode);

		for (i = 0; i < count; i++)
		{
			item = &items[i];

			if (strcmp(item->name, node_name) == 0)
			{
				if (item->type != POLAR_PRIMARY && item->type != POLAR_REPLICA && item->type != POLAR_STANDBY)
				{
					elog(error_level, "node %s is not useable for PX, consider adjust polar_px_nodes", node_name);
					goto next;
				}
				if (item->type == POLAR_PRIMARY && !px_use_master)
				{
					elog(error_level, "node %s is master, but polar_px_use_master is off, consider adjust polar_px_nodes or enable polar_px_use_master", node_name);
					goto next;
				}
				if (item->type == POLAR_STANDBY && !px_use_standby)
				{
					elog(error_level, "node %s is standby, but polar_px_use_standby is off, consider adjust polar_px_nodes or enable polar_px_use_standby", node_name);
					goto next;
				}
				if (!(item->state == STANDBY_SNAPSHOT_READY || (item->type == POLAR_PRIMARY && px_use_master)))
				{
					elog(error_level, "node %s is not ready for PX query, consider adjust polar_px_nodes", node_name);
					goto next;
				}

				config = &configs[idx];
				config->dbid = idx;
				config->role = 'x';
				config->node_idx = idx;
				config->dop = 1;
				config->port = item->port;
				config->hostip = pstrdup(item->host);
				config->name = pstrdup(item->name);
				++idx;
				break;
			}
		}
		if (i == count)
			elog(error_level, "GeneratePxNodeConfig: invalid node in polar_px_nodes: %s", node_name);
next:
		continue;
	}
	LWLockRelease(&polar_cluster_info_ctl->lock);

	if (idx == 0 && polar_px_ignore_unusable_nodes && !use_all)
	{
		use_all = true;
		elog(WARNING, "GeneratePxNodeConfig: all nodes in polar_px_nodes is unusable, fallback to use all node");
		pfree(configs);
		goto fallback;
	}
	if (idx == 0)
		elog(ERROR, "GeneratePxNodeConfig: no usable PX node");

	/**
	 * init qc config at last
	 */
	qc_config = &configs[idx++];
	qc_config->dbid = -1;
	qc_config->name = "master";
	qc_config->role = 'c';
	qc_config->node_idx = -1;
	qc_config->dop = 1;
	qc_config->port = PostPortNumber;
	qc_config->hostip = "";

	px_node_configs = configs;
	px_node_configs_size = idx;
}

/*
 * pxnode_getPxNodes
 *
 * Storage for the SegmentInstances block and all subsidiary
 * structures are allocated from the caller's context.
 */
PxNodes *
pxnode_getPxNodes()
{
	int			i;
	PxNodeInfo *pRow;
	StringInfo	cluster_map;
	MemoryContext oldContext;
	int			saved_generation = polar_cluster_info_ctl->generation;

	if (px_nodes && saved_generation == px_node_configs_generation)
		return px_nodes;

	oldContext = SwitchToPXWorkerContext();

	PG_TRY();
	{
		GeneratePxNodeConfigs();
	}
	PG_CATCH();
	{
		ClearPxNodeConfigs();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (px_node_configs == NULL || px_node_configs_size == 0)
		elog(ERROR, "polar_px_nodes is not correct");

	if (px_nodes != NULL)
	{
		Assert(px_nodes != NULL);
		Assert(px_nodes->qcInfo != NULL);
		Assert(px_nodes->pxInfo != NULL);

		pfree(px_nodes->pxInfo);
		pfree(px_nodes->qcInfo);
		pfree(px_nodes);
		px_nodes = NULL;
	}

	px_nodes = palloc0(sizeof(PxNodes));
	px_nodes->numActivePXs = 0;
	px_nodes->numIdlePXs = 0;
	px_nodes->pxCounter = 0;
	px_nodes->freeCounterList = NIL;

	px_nodes->pxInfo =
		(PxNodeInfo *) palloc0(sizeof(PxNodeInfo) * (px_node_configs_size - 1));
	px_nodes->totalPxNodes = px_node_configs_size - 1;

	px_nodes->qcInfo =
		(PxNodeInfo *) palloc0(sizeof(PxNodeInfo));
	px_nodes->totalQcNodes = 1;
	cluster_map = makeStringInfo();

	for (i = 0; i < px_node_configs_size; i++)
	{
		PxNodeConfigEntry *config = &px_node_configs[i];

		Assert(config->hostip != NULL);

		appendStringInfo(cluster_map, "| %d,%c,%d,%d,%d,%s,%s ", config->dbid, config->role, config->node_idx, config->dop, config->port, config->hostip, config->name);

		if (i == px_node_configs_size - 1)
			pRow = px_nodes->qcInfo;
		else
			pRow = &px_nodes->pxInfo[i];

		pRow->cm_node_idx = i;
		pRow->cm_node_size = px_node_configs_size - 1;
		pRow->px_nodes = px_nodes;
		pRow->config = config;
		pRow->freelist = NIL;
		pRow->numIdlePXs = 0;
		pRow->numActivePXs = 0;
	}

	MemoryContextSwitchTo(oldContext);
	px_node_configs_generation = saved_generation;
	elog(LOG, "PX using new cluster map: %s", cluster_map->data + 2);
	pfree(cluster_map->data);
	return px_nodes;
}

/*
 * Allocated a segdb
 *
 * If thers is idle segdb in the freelist, return it, otherwise, initialize
 * a new segdb.
 *
 * idle segdbs has an established connection with segment, but new segdb is
 * not setup yet, callers need to establish the connection by themselves.
 */
PxWorkerDescriptor *
pxnode_allocateIdlePX(int logicalWorkerIdx, int logicalTotalWorkers, SegmentType segmentType)
{
	PxWorkerDescriptor *pxWorkerDesc = NULL;
	PxNodeInfo *pxinfo;
	ListCell   *curItem = NULL;
	ListCell   *nextItem = NULL;
	MemoryContext oldContext;

	Assert(getPxWorkerCount() >= 1);

	if (logicalWorkerIdx == -1)
	{
		pxinfo = pxnode_getPxNodeInfo(-1, segmentType);
		logicalTotalWorkers = getPxWorkerCount();
	}
	else
		pxinfo = pxnode_getPxNodeInfo(logicalWorkerIdx, segmentType);

	if (pxinfo == NULL)
	{
		elog(ERROR, "could not find px component info for %d", logicalWorkerIdx);
	}

	oldContext = SwitchToPXWorkerContext();

	/*
	 * Always try to pop from the head.  Make sure to push them back to head
	 * in pxnode_recycleIdlePX().
	 */
	curItem = list_head(pxinfo->freelist);
	while (curItem != NULL)
	{
		PxWorkerDescriptor *tmp =
			(PxWorkerDescriptor *) lfirst(curItem);

		nextItem = lnext(pxinfo->freelist, curItem);
		Assert(tmp);

		if ((segmentType == SEGMENTTYPE_EXPLICT_WRITER && !tmp->isWriter) ||
			(segmentType == SEGMENTTYPE_EXPLICT_READER && tmp->isWriter))
		{
			curItem = nextItem;
			continue;
		}

		pxinfo->freelist = list_delete_cell(pxinfo->freelist, curItem);
		/* update numIdlePXs */
		DECR_COUNT(pxinfo, numIdlePXs);

		pxWorkerDesc = tmp;
		pxWorkerDesc->logicalWorkerInfo.total_count = logicalTotalWorkers;
		pxWorkerDesc->logicalWorkerInfo.idx = logicalWorkerIdx;
		break;
	}

	/* POLAR px */
	if (!pxWorkerDesc)
	{
		pxWorkerDesc = pxconn_createWorkerDescriptor(pxinfo,
													 nextPXIdentifer(pxinfo->px_nodes),
													 logicalWorkerIdx,
													 logicalTotalWorkers);
	}
	/* POLAR end */

	pxconn_setPXIdentifier(pxWorkerDesc, -1);

	INCR_COUNT(pxinfo, numActivePXs);

	MemoryContextSwitchTo(oldContext);

	return pxWorkerDesc;
}

static bool
cleanupPX(PxWorkerDescriptor *pxWorkerDesc)
{
	Assert(pxWorkerDesc != NULL);

	/*
	 * if the process is in the middle of blowing up... then we don't do
	 * anything here.  making libpq and other calls can definitely result in
	 * things getting HUNG.
	 */
	if (proc_exit_inprogress)
		return false;

	if (pxconn_isBadConnection(pxWorkerDesc))
		return false;

	/* Note, we cancel all "still running" queries */
	if (!pxconn_discardResults(pxWorkerDesc, 20))
	{
		elog(LOG, "cleaning up seg%d while it is still busy", pxWorkerDesc->logicalWorkerInfo.idx);
		return false;
	}

	/* PX is no longer associated with a slice. */
	pxconn_setPXIdentifier(pxWorkerDesc, /* slice index */ -1);

	return true;
}

void
pxnode_recycleIdlePX(PxWorkerDescriptor *pxWorkerDesc, bool forceDestroy)
{
	PxNodeInfo *pxinfo;
	MemoryContext oldContext;

	Assert(px_nodes);
	Assert(px_worker_context);

	pxinfo = pxWorkerDesc->pxNodeInfo;

	/* update num of active PXs */
	DECR_COUNT(pxinfo, numActivePXs);

	oldContext = SwitchToPXWorkerContext();

	if (forceDestroy || !cleanupPX(pxWorkerDesc))
		goto destroy_segdb;

	if (list_length(pxinfo->freelist) >= px_cached_px_workers)
		goto destroy_segdb;

	pxinfo->freelist = lcons(pxWorkerDesc, pxinfo->freelist);
	INCR_COUNT(pxinfo, numIdlePXs);
	MemoryContextSwitchTo(oldContext);
	return;

destroy_segdb:

	pxconn_termWorkerDescriptor(pxWorkerDesc);
	MemoryContextSwitchTo(oldContext);
}

static int
nextPXIdentifer(PxNodes * px_nodes)
{
	int			result;

	if (!px_nodes->freeCounterList)
	{
		result = px_nodes->pxCounter++;
		return result;
	}
	result = linitial_int(px_nodes->freeCounterList);
	px_nodes->freeCounterList = list_delete_first(px_nodes->freeCounterList);
	return result;
}

/*
 * Find PxNodeInfo in the array by segment index.
 */
PxNodeInfo *
pxnode_getPxNodeInfo(int contentId, SegmentType segmentType)
{
	PxNodeInfo *pxInfo = NULL;
	PxNodes    *px_nodes;
	int			i;

	px_nodes = pxnode_getPxNodes();

	if (contentId == -1)
		return px_nodes->qcInfo;

	/*
	 * POLAR px for PX DML Because the IP and Port of the RW and QC nodes are
	 * exactly the same , qcInfo can be used directly.
	 */
	if (SEGMENTTYPE_EXPLICT_WRITER == segmentType)
		return px_nodes->qcInfo;

	if (contentId < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("unexpected content id %d", contentId)));

	contentId %= px_nodes->totalPxNodes;
	for (i = 0; i < px_nodes->totalPxNodes; i++)
	{
		pxInfo = &px_nodes->pxInfo[i];
		if (pxInfo->config->node_idx == contentId)
			return pxInfo;
	}

	return NULL;
}

/*
 * performs all necessary setup required for Greenplum Database mode.
 *
 * This includes pxlink_setup() and initializing the Motion Layer.
 */
void
px_setup(void)
{
	elog(DEBUG1, "Initializing px components...");

	/* Initialize the Motion Layer IPC subsystem. */
	InitMotionLayerIPC();
}

/*
 * performs all necessary cleanup required when leaving Greenplum
 * Database mode.  This is also called when the process exits.
 *
 * NOTE: the arguments to this function are here only so that we can
 *		 register it with on_proc_exit().  These parameters should not
 *		 be used since there are some callers to this that pass them
 *		 as NULL.
 *
 */
void
px_cleanup(int code pg_attribute_unused(), Datum arg pg_attribute_unused())
{
	elog(DEBUG1, "Cleaning up Greenplum components...");

	if (px_role == PX_ROLE_QC)
	{
		if (px_total_plans > 0)
		{
			elog(DEBUG1, "session dispatched %d plans %d slices (%f), largest plan %d",
				 px_total_plans, px_total_slices,
				 ((double) px_total_slices / (double) px_total_plans),
				 px_max_slices);
		}
	}
	else if (px_role == PX_ROLE_PX && px_max_workers_number > 0)
		pg_atomic_sub_fetch_u32(&ProcGlobal->pxWorkerCounter, 1);

	/* shutdown our listener socket */
	CleanUpMotionLayerIPC();
}

/*
 * Given total number of primary segment databases and a number of
 * segments to "skip" - this routine creates a boolean map (array) the
 * size of total number of segments and randomly selects several
 * entries (total number of total_to_skip) to be marked as
 * "skipped". This is used for external tables with the 'gpfdist'
 * protocol where we want to get a number of *random* segdbs to
 * connect to a gpfdist client.
 *
 * Caller of this function should pfree skip_map when done with it.
 */
bool *
makeRandomSegMap(int total_primaries, int total_to_skip)
{
	int			randint;		/* some random int representing a seg    */
	int			skipped = 0;	/* num segs already marked to be skipped */
	bool	   *skip_map;

	skip_map = (bool *) palloc(total_primaries * sizeof(bool));
	MemSet(skip_map, false, total_primaries * sizeof(bool));

	while (total_to_skip != skipped)
	{
		/*
		 * create a random int between 0 and (total_primaries - 1).
		 */
		randint = pxhashrandomseg(total_primaries);

		/*
		 * mark this random index 'true' in the skip map (marked to be
		 * skipped) unless it was already marked.
		 */
		if (skip_map[randint] == false)
		{
			skip_map[randint] = true;
			skipped++;
		}
	}

	return skip_map;
}

int
getPxWorkerCount(void)
{
	int			numsegments = -1;

	if (px_role == PX_ROLE_QC)
	{
		Assert(px_dop_per_node >= 1);
		numsegments = pxnode_getPxNodes()->totalPxNodes
			* polar_get_stmt_px_dop();
	}
	else if (px_role == PX_ROLE_PX)
		numsegments = px_logical_total_workers;
	else if (px_role == PX_ROLE_UTILITY)
		numsegments = 1;
	return numsegments;
}
