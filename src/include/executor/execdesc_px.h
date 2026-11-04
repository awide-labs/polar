/*-------------------------------------------------------------------------
 *
 * execdesc_px.h
 *	  Slice and dispatcher state for PX.
 *
 * Portions Copyright (c) 2020, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/executor/execdesc_px.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDESC_PX_H
#define EXECDESC_PX_H

#include "nodes/execnodes.h"
#include "tcop/dest.h"

/*
 * PX Plan Slice information
 *
 * These structures summarize how a plan tree is sliced up into separate
 * units of execution or slices. A slice will execute on a each worker within
 * a gang of processes. Some gangs have a worker process on each of several
 * databases, others have a single worker.
 */
typedef struct ExecSlice
{
	/*
	 * The index in the global slice table of this slice. The root slice of
	 * the main plan is always 0. Slices that have senders at their local root
	 * have a sliceIndex equal to the motionID of their sender Motion.
	 *
	 * Undefined slices should have this set to -1.
	 */
	int			sliceIndex;

	/*
	 * The root slice of the slice tree of which this slice is a part.
	 */
	int			rootIndex;

	/*
	 * the index of parent in global slice table (origin 0) or -1 if this is
	 * root slice.
	 */
	int			parentIndex;

	/*
	 * nominal # of segments, for hash calculations. Can be different from
	 * gangSize, if direct dispatch.
	 */
	int			planNumSegments;

	/*
	 * An integer list of indices in the global slice table (origin  0) of the
	 * child slices of this slice, or -1 if this is a leaf slice. A child
	 * slice corresponds to a receiving motion in this slice.
	 */
	List	   *children;

	/* What kind of gang does this slice need? */
	GangType	gangType;

	/*
	 * A list of segment ids who will execute this slice.
	 *
	 * It is set before the process lists below and used to decide how to
	 * initialize them.
	 */
	List	   *segments;

	struct Gang *primaryGang;

	/*
	 * A list of PXProcess nodes corresponding to the worker processes
	 * allocated to implement this plan slice.
	 *
	 * The number of processes must agree with the the plan slice to be
	 * implemented.
	 */
	List	   *primaryProcesses;
	/* A bitmap to identify which PX should execute this slice */
	Bitmapset  *processesMap;
} ExecSlice;

/*
 * The SliceTable is a list of Slice structures organized into root slices
 * and motion slices as follows:
 *
 * Slice 0 is the root slice of plan as a whole.
 *
 * The rest root slices of initPlans, or sub-slices of the root slice or one
 * of the initPlan roots.
 */
typedef struct SliceTable
{
	NodeTag		type;

	int			localSlice;		/* Index of the slice to execute. */
	int			numSlices;
	ExecSlice  *slices;			/* Array of slices, indexed by SliceIndex */

	bool		hasMotions;		/* Are there any Motion nodes anywhere in the
								 * plan? */

	int			instrument_options; /* OR of InstrumentOption flags */
	uint32		ic_instance_id;
} SliceTable;

/*
 * Holds information about a cursor's current position.
 */
typedef struct CursorPosInfo
{
	NodeTag		type;

	char	   *cursor_name;
	int			px_worker_id;
	ItemPointerData ctid;
	Oid			table_oid;
} CursorPosInfo;


/* ----------------
 *		query dispatch information:
 *
 * a QueryDispatchDesc encapsulates extra information that need to be
 * dispatched from QC to PXs.
 *
 * A QueryDesc is created separately on each segment, but QueryDispatchDesc
 * is created in the QC, and passed to each segment.
 * ---------------------
 */
typedef struct QueryDispatchDesc
{
	NodeTag		type;

	/*
	 * For a SELECT INTO statement, this stores the tablespace to use for the
	 * new table and related auxiliary tables.
	 */
	char	   *intoTableSpaceName;

	/*
	 * Oids to use, for new objects created in a CREATE command.
	 */
	List	   *oidAssignments;

	/*
	 * This allows the slice table to accompany the plan as it moves around
	 * the executor.
	 *
	 * Currently, the slice table should not be installed on the QC. Rather is
	 * it shipped to PXs as a separate parameter to POLARPX. The
	 * implementation of POLARPX, which runs on the PXs, installs the slice
	 * table in the plan as required there.
	 */
	SliceTable *sliceTable;

	List	   *cursorPositions;

	/*
	 * Set to true for CTAS and SELECT INTO. Set to false for ALTER TABLE
	 * REORGANIZE. This is mainly used to track if the dispatched query is
	 * meant for internal rewrite on PX segments or just for holding data from
	 * a SELECT for a new relation. If DestIntoRel is set in the QC's
	 * queryDesc->dest, use the original table's reloptions. If DestRemote is
	 * set, use default reloptions + gp_default_storage_options.
	 */
	bool		useChangedAOOpts;
} QueryDispatchDesc;

#endif							/* EXECDESC_PX_H  */
