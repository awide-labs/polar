/*-------------------------------------------------------------------------
 *
 * plannodes_px.h
 *	  definitions for query plan nodes for px
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/plannodes_px.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_PX_H
#define PLANNODES_PX_H

#include "nodes/primnodes.h"

struct Plan;

typedef struct DirectDispatchInfo
{
	/**
	 * if true then this Slice requires an n-gang but the gang can be targeted to
	 *   fewer segments than the entire cluster.
	 *
	 * When true, directDispatchContentId and directDispathCount will combine to indicate
	 *    the content ids that need segments.
	 */
	bool		isDirectDispatch;
	List	   *contentIds;

	/* only used while planning, in createplan.c */
	bool		haveProcessedAnyCalculations;
} DirectDispatchInfo;

typedef enum PlanGenerator
{
	PLANGEN_PG = 0,				/* plan produced by the planner */
	PLANGEN_PX,					/* plan produced by the optimizer */
} PlanGenerator;

typedef enum DMLAction
{
	DML_DELETE,
	DML_INSERT
} DMLAction;

/*
 * FlowType - kinds of tuple flows in parallelized plans.
 *
 * This enum is a PX extension.
 */
typedef enum FlowType
{
	FLOW_UNDEFINED,				/* used prior to calculation of type of
								 * derived flow */
	FLOW_SINGLETON,				/* flow has single stream */
	FLOW_REPLICATED,			/* flow is replicated across IOPs */
	FLOW_PARTITIONED,			/* flow is partitioned across IOPs */
} FlowType;

/*----------
 * Flow - describes a tuple flow in a parallelized plan
 *
 * This node type is a PX extension.
 *
 * Plan nodes contain a reference to a Flow that characterizes the output
 * tuple flow of the node.
 *----------
 */
typedef struct Flow
{
	NodeTag		type;			/* T_Flow */
	FlowType	flotype;		/* Type of flow produced by the plan. */

	/*
	 * If flotype is FLOW_SINGLETON, then this is the segment (-1 for entry)
	 * on which tuples occur.
	 */
	int			worker_idx;		/* Segment index of singleton flow. */
	int			numsegments;

} Flow;


/* GangType enumeration is used in several structures related to PX
 * slice plan support.
 */
typedef enum GangType
{
	GANGTYPE_UNALLOCATED,		/* a root slice executed by the qDisp */
	GANGTYPE_ENTRYDB_READER,	/* a 1-gang with read access to the entry db */
	GANGTYPE_SINGLETON_READER,	/* a 1-gang to read the segment dbs */
	GANGTYPE_PRIMARY_READER,	/* a 1-gang or N-gang to read the segment dbs */
	GANGTYPE_PRIMARY_WRITER		/* the N-gang that can update the segment dbs */
} GangType;

/*
 * PlanSlice represents one query slice, to be executed by a separate gang
 * of executor processes.
 */
typedef struct PlanSlice
{
	int			sliceIndex;
	int			parentIndex;

	GangType	gangType;

	/* # of segments in the gang, for PRIMARY_READER/WRITER slices */
	int			numsegments;
	/* segment to execute on, for SINGLETON_READER slices */
	int			worker_idx;

	/* direct dispatch information, for PRIMARY_READER/WRITER slices */
	DirectDispatchInfo directDispatch;
} PlanSlice;


/*
 * Share type of sharing a node.
 */
typedef enum ShareType
{
	SHARE_NOTSHARED,
	SHARE_MATERIAL,				/* Sharing a material node */
	SHARE_MATERIAL_XSLICE,		/* Sharing a material node, across slice */
	SHARE_SORT,					/* Sharing a sort */
	SHARE_SORT_XSLICE			/* Sharing a sort, across slice */
	/* Other types maybe added later, like sharing a hash */
} ShareType;


/* -------------------------
 *		motion node structs
 * -------------------------
 */
typedef enum MotionType
{
	MOTIONTYPE_GATHER,			/* Send tuples from N senders to one receiver */
	MOTIONTYPE_GATHER_SINGLE,	/* Execute subplan on N nodes, but only send
								 * the tuples from one */
	MOTIONTYPE_HASH,			/* Use hashing to select a worker_idx
								 * destination */
	MOTIONTYPE_BROADCAST,		/* Send tuples from one sender to a fixed set
								 * of worker_idxes */
	MOTIONTYPE_EXPLICIT,		/* Send tuples to the segment explicitly
								 * specified in their segid column */
	MOTIONTYPE_OUTER_QUERY		/* Gather or Broadcast to outer query's slice,
								 * don't know which one yet */
} MotionType;


/* ----------------
 *		index type information
 */
typedef enum LogicalIndexType
{
	INDTYPE_BTREE = 0,
	INDTYPE_BITMAP = 1,
	INDTYPE_GIST = 2,
	INDTYPE_GIN = 3
} LogicalIndexType;

typedef struct LogicalIndexInfo
{
	Oid			logicalIndexOid;	/* OID of the logical index */
	int			nColumns;		/* Number of columns in the index */
	AttrNumber *indexKeys;		/* column numbers of index keys */
	List	   *indPred;		/* predicate if partial index, or NIL */
	List	   *indExprs;		/* index on expressions */
	bool		indIsUnique;	/* unique index */
	LogicalIndexType indType;	/* index type: btree or bitmap */
	Node	   *partCons;		/* concatenated list of check constraints of
								 * each partition on which this index is
								 * defined */
	List	   *defaultLevels;	/* Used to identify a default partition */
} LogicalIndexInfo;

#endif							/* PLANNODES_PX_H */
