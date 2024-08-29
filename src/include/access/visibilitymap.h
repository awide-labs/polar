/*-------------------------------------------------------------------------
 *
 * visibilitymap.h
 *		visibility map interface
 *
 *
 * Portions Copyright (c) 2007-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/visibilitymap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAP_H
#define VISIBILITYMAP_H

#include "access/visibilitymapdefs.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "utils/relcache.h"

/* POLAR */
#include "access/polar_logindex_redo.h"
#include "replication/walreceiver.h"

/* Macros for visibilitymap test */
#define VM_ALL_VISIBLE(r, b, v) \
	((visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_VISIBLE) != 0)
#define VM_ALL_FROZEN(r, b, v) \
	((visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_FROZEN) != 0)

/* POLAR */
#define POLAR_RECOVERY_CONFLICT_RESOLVED() (!polar_logindex_redo_instance || polar_enable_resolve_conflict)

#define POLAR_REPLICA_ENABLE_VISIBILITYMAP() \
	(polar_is_replica() && polar_hot_standby_enable_vm && \
	 (hot_standby_feedback || POLAR_RECOVERY_CONFLICT_RESOLVED()))

#define POLAR_VISIBILITYMAP_ENABLED() \
	(polar_is_primary() || polar_is_standby() || POLAR_REPLICA_ENABLE_VISIBILITYMAP())
/* POLAR end */

extern bool visibilitymap_clear(Relation rel, BlockNumber heapBlk,
								Buffer vmbuf, uint8 flags,
								XLogReaderState *polar_record);
extern void visibilitymap_pin(Relation rel, BlockNumber heapBlk,
							  Buffer *vmbuf);
extern bool visibilitymap_pin_ok(BlockNumber heapBlk, Buffer vmbuf);
extern void visibilitymap_set(Relation rel, BlockNumber heapBlk, Buffer heapBuf,
							  XLogRecPtr recptr, Buffer vmBuf, TransactionId cutoff_xid,
							  uint8 flags, XLogRecPtr polar_read_rec_ptr);
extern uint8 visibilitymap_get_status(Relation rel, BlockNumber heapBlk, Buffer *vmbuf);
extern void visibilitymap_count(Relation rel, BlockNumber *all_visible, BlockNumber *all_frozen);
extern BlockNumber visibilitymap_prepare_truncate(Relation rel,
												  BlockNumber nheapblocks);

/* POLAR */
/*
 * Size of the bitmap on each visibility map page, in bytes. There's no
 * extra headers, so the whole page minus the standard page header is
 * used for the bitmap.
 */
#define MAPSIZE (BLCKSZ - MAXALIGN(SizeOfPageHeaderData))

/* Number of heap blocks we can represent in one byte */
#define HEAPBLOCKS_PER_BYTE (BITS_PER_BYTE / BITS_PER_HEAPBLOCK)

/* Number of heap blocks we can represent in one visibility map page. */
#define HEAPBLOCKS_PER_PAGE (MAPSIZE * HEAPBLOCKS_PER_BYTE)

/* Mapping from heap block number to the right bit in the visibility map */
#define HEAPBLK_TO_MAPBLOCK(x) ((x) / HEAPBLOCKS_PER_PAGE)
#define HEAPBLK_TO_MAPBYTE(x) (((x) % HEAPBLOCKS_PER_PAGE) / HEAPBLOCKS_PER_BYTE)
#define HEAPBLK_TO_OFFSET(x) (((x) % HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK)

extern void polar_visibilitymap_set(BlockNumber heapBlk, Buffer vmBuf, uint8 flags);

/* POLAR end */

#endif							/* VISIBILITYMAP_H */
